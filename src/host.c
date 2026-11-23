#define _GNU_SOURCE
#include "kobo.h"
#include "codec.h"
#include "cJSON.h"
#include "light_panel.h"
#include "power_worker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <linux/input.h>
#ifdef REEDEN_HEADLESS_DIAGNOSTIC
#include "headless_guard.h"
#endif

volatile int app_quit;
static FlutterEngine engine;
static Display screen;
#ifndef REEDEN_HEADLESS_DIAGNOSTIC
static void observe_input(uint64_t at){display_note_input(&screen,at);}
#endif
static int wake_pipe[2]={-1,-1};
static pthread_t platform_thread;
static pthread_mutex_t task_lock=PTHREAD_MUTEX_INITIALIZER;
typedef struct Pending { FlutterTask task; uint64_t time; struct Pending *next; } Pending;
static Pending *tasks;
static unsigned long tasks_posted,tasks_run;
static char *clipboard;
static const char *root;
static bool sleeping;
static uint64_t cover_at;
static char cover_path[4096];
static unsigned cover_width,cover_height,cover_channels=1;
static int saved_brightness=-1;
static pid_t power_worker=-1;
static char heartbeat_path[4096];
static void respond(const FlutterPlatformMessage *,const void *,size_t);
static void empty_reply(const FlutterPlatformMessage *);
static void heartbeat_touch(void){
    int fd=open(heartbeat_path,O_WRONLY|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd>=0){futimens(fd,NULL);close(fd);}
}
#ifndef REEDEN_HEADLESS_DIAGNOSTIC
static void light_panel_changed(void *u){
    Display *d=u;pthread_mutex_lock(&d->lock);
    if(light_panel_redraw(d->pending,d->pending_stride,d->v.xres,d->v.yres)){
        uint64_t now=monotonic_ns();if(!d->dirty)d->first_dirty=now;
        d->dirty=true;d->last_present=now;
    }
    pthread_mutex_unlock(&d->lock);
}
#endif
static void state_file(const char *name,bool create){
    char path[4096];snprintf(path,sizeof(path),"%s/state/%s",root,name);
    if(create){FILE *f=fopen(path,"w");if(f)fclose(f);}else unlink(path);
}
static void power_trace(const char *phase){
    struct timespec wall;clock_gettime(CLOCK_REALTIME,&wall);
    fprintf(stderr,"[power] phase=%s wall=%lld mono_ms=%llu\n",phase,
        (long long)wall.tv_sec,(unsigned long long)(monotonic_ns()/1000000ULL));
}
static void lifecycle(const char *value){
    FlutterPlatformMessage m={.struct_size=sizeof(m),.channel="flutter/lifecycle",.message=(const uint8_t*)value,.message_size=strlen(value)};
    fprintf(stderr,"[lifecycle] %s result=%d\n",value,FlutterEngineSendPlatformMessage(engine,&m));
}
static void sleep_cover_channel(const FlutterPlatformMessage *m,const char *method,Reader args){
    Writer w={0};w_byte(&w,0);
    if(!strcmp(method,"platform"))w_string(&w,"kindle");
    else if(!strcmp(method,"currentImage"))w_null(&w);
    else if(!strcmp(method,"isSupported")||!strcmp(method,"rgb"))w_bool(&w,1);
    else if(!strcmp(method,"setImage")){
        Value path={0},width={0},height={0},ch={0};
        bool valid=read_argument(args,"path",&path)&&path.type==7&&read_argument(args,"width",&width)&&read_argument(args,"height",&height)&&width.integer>0&&height.integer>0&&width.integer<=4096&&height.integer<=4096;
        unsigned channels=(read_argument(args,"channels",&ch)&&ch.integer>=1&&ch.integer<=4)?(unsigned)ch.integer:1;
        if(valid){snprintf(cover_path,sizeof(cover_path),"%s",path.string);cover_width=width.integer;cover_height=height.integer;cover_channels=channels;if(sleeping)cover_at=monotonic_ns()+100000000ULL;fprintf(stderr,"[sleep-cover] registered %ux%u channels=%u\n",cover_width,cover_height,cover_channels);}
        w_bool(&w,valid);
    }else {empty_reply(m);return;}
    respond(m,w.data,w.n);
}
static void set_sleep(Touch *touch,bool asleep){
    if(sleeping==asleep)return;
    sleeping=asleep;
    if(asleep){
        state_file("power-phase",false);power_trace("locking");
        state_file("power-cancel",false);state_file("asleep",true);
        saved_brightness=read_number("/sys/class/backlight/mxc_msp430.0/brightness",0);
        touch_close(touch,engine);
        lifecycle("AppLifecycleState.paused");cover_at=monotonic_ns()+1400000000ULL;
        fprintf(stderr,"[power] screen locked; engine retained\n");
    }else{
        power_trace("restoring-reader");
        state_file("power-cancel",true);
        if(saved_brightness>=0)write_number("/sys/class/backlight/mxc_msp430.0/brightness",saved_brightness);
        lifecycle("AppLifecycleState.resumed");FlutterEngineScheduleFrame(engine);display_full(&screen);
        if(touch_open(touch)){perror("touch resume");app_quit=1;}
        heartbeat_touch();state_file("asleep",false);cover_at=0;
        fprintf(stderr,"[power] resumed same engine and book\n");
        state_file("power-phase",false);power_trace("reader-resumed");
    }
}
static char logged_channels[64][160];
static unsigned channel_count;
static void wake(void){if(wake_pipe[1]>=0){char c=1;ssize_t r=write(wake_pipe[1],&c,1);(void)r;}}
static void quit_signal(int n){(void)n;app_quit=1;wake();}
/* Only the platform thread may execute tasks for this runner. */
static bool on_platform_thread(void *u){(void)u;return pthread_equal(platform_thread,pthread_self());}
static void post_task(FlutterTask task,uint64_t target,void *u){
    (void)u;Pending *p=malloc(sizeof(*p));if(!p){app_quit=1;wake();return;}
    /* Flutter timestamps are already absolute monotonic nanoseconds. */
    uint64_t due=target;
    *p=(Pending){.task=task,.time=due};
    pthread_mutex_lock(&task_lock);
    Pending **at=&tasks;while(*at&&(*at)->time<=due)at=&(*at)->next;p->next=*at;*at=p;tasks_posted++;
    pthread_mutex_unlock(&task_lock);
    wake();
}
static void run_tasks(void){
    /* Walk the whole queue and run every task whose due time has arrived.
       A single far-future task must not block tasks queued behind it. */
    uint64_t budget=monotonic_ns()+8000000ULL;
    for(unsigned i=0;i<64&&!app_quit;i++){
        uint64_t now=FlutterEngineGetCurrentTime();
        Pending *due=NULL;
        pthread_mutex_lock(&task_lock);
        Pending **at=&tasks;
        while(*at){
            if((*at)->time<=now){due=*at;*at=due->next;break;}
            at=&(*at)->next;
        }
        pthread_mutex_unlock(&task_lock);
        if(!due)break;
        FlutterEngineResult r=FlutterEngineRunTask(engine,&due->task);
        free(due);tasks_run++;
        if(r!=kSuccess)fprintf(stderr,"[engine] RunTask error=%d\n",r);
        if(monotonic_ns()>=budget)break;
    }
}
static int wait_ms(uint64_t host_deadline){
    pthread_mutex_lock(&task_lock);
    uint64_t t=host_deadline;
    for(Pending *p=tasks;p;p=p->next)if(p->time<t)t=p->time;
    pthread_mutex_unlock(&task_lock);
    uint64_t now=FlutterEngineGetCurrentTime();if(t<=now)return 0;
    uint64_t d=t-now;if(!d)return 0;return d<5000000000ULL?(int)((d+999999)/1000000):5000;
}
static void respond(const FlutterPlatformMessage *m,const void *p,size_t n){if(m->response_handle){FlutterEngineResult r=FlutterEngineSendPlatformMessageResponse(engine,m->response_handle,p,n);if(r!=kSuccess)fprintf(stderr,"[engine] reply channel=%s error=%d\n",m->channel,r);}}
static void json_reply(const FlutterPlatformMessage *m,cJSON *value){
    cJSON *a=cJSON_CreateArray();cJSON_AddItemToArray(a,value?value:cJSON_CreateNull());
    char *s=cJSON_PrintUnformatted(a);respond(m,s,strlen(s));free(s);cJSON_Delete(a);
}
static void empty_reply(const FlutterPlatformMessage *m){respond(m,NULL,0);}
static void json_platform(const FlutterPlatformMessage *m){
    cJSON *j=cJSON_ParseWithLength((const char*)m->message,m->message_size);
    if(!j){json_reply(m,NULL);return;}
    const cJSON *method=cJSON_GetObjectItemCaseSensitive(j,"method"),*args=cJSON_GetObjectItemCaseSensitive(j,"args");
    const char *name=cJSON_IsString(method)?method->valuestring:"";
    if(!strcmp(name,"SystemNavigator.pop")){app_quit=1;json_reply(m,NULL);}
    else if(!strcmp(name,"Clipboard.setData")){const cJSON *s=cJSON_GetObjectItemCaseSensitive(args,"text");free(clipboard);clipboard=cJSON_IsString(s)?strdup(s->valuestring):NULL;json_reply(m,NULL);}
    else if(!strcmp(name,"Clipboard.getData")){cJSON *v=NULL;if(clipboard){v=cJSON_CreateObject();cJSON_AddStringToObject(v,"text",clipboard);}json_reply(m,v);}
    else if(!strcmp(name,"Clipboard.hasStrings")){cJSON *v=cJSON_CreateObject();cJSON_AddBoolToObject(v,"value",clipboard&&*clipboard);json_reply(m,v);}
    else if(!strncmp(name,"SystemChrome.",13)||!strcmp(name,"SystemSound.play")||!strcmp(name,"HapticFeedback.vibrate")||!strcmp(name,"System.initializationComplete"))json_reply(m,NULL);
    else {fprintf(stderr,"[platform] unsupported JSON method=%s (defaulting to [null])\n",name);json_reply(m,NULL);}
    cJSON_Delete(j);
}
static void standard_error(Writer *w,const char *code,const char *message){w->n=0;w_byte(w,1);w_string(w,code);w_string(w,message);w_null(w);}
static void eink_channel(const FlutterPlatformMessage *m,const char *method,Reader args){
    Writer w={0};w_byte(&w,0);
    if(!strcmp(method,"isSupported"))w_bool(&w,1);
    else if(!strcmp(method,"fullRefresh")||!strcmp(method,"refresh")){Value delay={0};unsigned ms=0;if(read_argument(args,"delayMs",&delay)&&delay.integer>0&&delay.integer<=60000)ms=(unsigned)delay.integer;display_request_full(&screen,ms);fprintf(stderr,"[refresh-request] method=%s delay_ms=%u\n",method,ms);w_null(&w);}
    else if(!strcmp(method,"setRefreshMode")){
        Value mode={0};bool found=read_argument(args,"mode",&mode);if(!found)found=read_argument(args,NULL,&mode);
        if(found&&mode.type==7&&(!strcmp(mode.string,"balanced")||!strcmp(mode.string,"hd")||!strcmp(mode.string,"regal")||!strcmp(mode.string,"fast"))){pthread_mutex_lock(&screen.lock);snprintf(screen.mode,sizeof(screen.mode),"%s",mode.string);pthread_mutex_unlock(&screen.lock);fprintf(stderr,"[refresh-mode] %s\n",mode.string);w_bool(&w,1);}else w_bool(&w,0);
    }else if(!strcmp(method,"capabilities")){
        w_map(&w,7);w_string(&w,"platform");w_string(&w,"kindle");w_string(&w,"driver");w_string(&w,"kobo-hwtcon");w_string(&w,"fullRefresh");w_bool(&w,1);w_string(&w,"supported");w_bool(&w,1);w_string(&w,"modes");w_list(&w,4);w_string(&w,"hd");w_string(&w,"regal");w_string(&w,"balanced");w_string(&w,"fast");w_string(&w,"armPageTurn");w_bool(&w,1);w_string(&w,"color");w_bool(&w,1);
    }else if(!strcmp(method,"diagnostics")){
        w_map(&w,5);w_string(&w,"driver");w_string(&w,"kobo-hwtcon");w_string(&w,"mode");w_string(&w,screen.mode);w_string(&w,"width");w_int(&w,screen.v.xres);w_string(&w,"height");w_int(&w,screen.v.yres);w_string(&w,"refreshes");w_int(&w,screen.refreshes);
    }else if(!strcmp(method,"armPageTurn")){
        /* The official Kindle runner exposes this through ireader_epdc.
         * Match its three argument keys and default forward direction. */
        Value direction={0};
        bool forward=!read_argument(args,"forward",&direction)||direction.type!=2;
        Value vertical={0},speed={0};
        bool is_vertical=read_argument(args,"vertical",&vertical)&&vertical.type==1;
        unsigned steps=24;
        if(read_argument(args,"speedBits",&speed)&&(speed.type==3||speed.type==4))steps=speed.integer<=63?16:speed.integer<=127?24:32;
        bool armed=display_arm_page_turn(&screen,forward,is_vertical,steps);
        fprintf(stderr,"[page-turn] arm type=%d forward=%u vertical=%u steps=%u accepted=%u\n",direction.type,forward,is_vertical,steps,armed);
        w_bool(&w,armed);
    }
    else {empty_reply(m);return;}
    respond(m,w.data,w.error?0:w.n);
}
static void kobo_channel(const FlutterPlatformMessage *m,const char *method,Reader args){
    Writer w={0};w_byte(&w,0);Value v={0};
    if(!strcmp(method,"getBatteryLevel"))w_int(&w,read_number("/sys/class/power_supply/bd71827_bat/capacity",-1));
    else if(!strcmp(method,"getBrightness"))w_int(&w,read_number("/sys/class/backlight/mxc_msp430.0/brightness",-1));
    else if(!strcmp(method,"getWarmth")){int n=read_number("/sys/class/backlight/lm3630a_led/color",-1);w_int(&w,n<0?-1:(10-n)*10);}
    else if(!strcmp(method,"setBrightness")||!strcmp(method,"setWarmth")){
        if(!screen.mem)standard_error(&w,"not-active","Hardware handoff has not completed");
        else if(!read_argument(args,NULL,&v)||v.type!=3||v.integer<0||v.integer>100)standard_error(&w,"invalid-argument","Expected integer 0..100");
        else {bool warm=!strcmp(method,"setWarmth");int n=warm?10-(int)(v.integer+5)/10:(int)v.integer;int r=write_number(warm?"/sys/class/backlight/lm3630a_led/color":"/sys/class/backlight/mxc_msp430.0/brightness",n);if(r)standard_error(&w,"io-error",strerror(errno));else w_null(&w);}
    }else if(!strcmp(method,"exit")){app_quit=1;w_null(&w);}
    else {empty_reply(m);return;}
    respond(m,w.data,w.error?0:w.n);
}
static void platform_message(const FlutterPlatformMessage *m,void *u){
    (void)u;
    if(!strcmp(m->channel,"flutter/platform")){json_platform(m);return;}
    if(!strcmp(m->channel,"flutter/navigation")){json_reply(m,NULL);return;}
    if(!strcmp(m->channel,"flutter/isolate")){empty_reply(m);return;}
    bool seen=false;for(unsigned i=0;i<channel_count;i++)if(!strcmp(logged_channels[i],m->channel))seen=true;
    if(!seen&&channel_count<64){snprintf(logged_channels[channel_count++],160,"%s",m->channel);
        fprintf(stderr,"[channel] %s\n",m->channel);}
    Reader r={m->message,m->message_size,0,0};char method[160]={0};
    if(!read_method(&r,method,sizeof(method))){empty_reply(m);return;}
    /* Kindle's existing ripple UI queries ireader_epdc, despite the name.
     * Advertising support only on eink_epd leaves that UI hidden. */
    if(!strcmp(m->channel,"app.reeden.eink_epd")||!strcmp(m->channel,"app.reeden.ireader_epdc")){eink_channel(m,method,r);return;}
    if(!strcmp(m->channel,"app.reeden.kobo")){kobo_channel(m,method,r);return;}
    if(!strcmp(m->channel,"app.reeden.eink_sleep_cover")){sleep_cover_channel(m,method,r);return;}
    if(!strcmp(m->channel,"app.reeden.kindle_screen")){
        Writer w={0};w_byte(&w,0);
        if(!strcmp(method,"state")){w_map(&w,2);w_string(&w,"supported");w_bool(&w,0);w_string(&w,"landscape");w_bool(&w,0);}
        else if(!strcmp(method,"setLandscape"))w_bool(&w,0);
        else {empty_reply(m);return;}
        respond(m,w.data,w.n);return;
    }
    if(!strcmp(m->channel,"flutter/keyboard")){
        /* Keyboard uses StandardMethodCodec: success byte, then key map. */
        Writer w={0};w_byte(&w,0);
        if(!strcmp(method,"getKeyboardState"))w_map(&w,0);
        else w_null(&w);
        respond(m,w.data,w.n);
        return;
    }
    if(!strcmp(m->channel,"app.reeden.channel")){
        Writer w={0};w_byte(&w,0);
        if(!strcmp(method,"getSystemFonts"))w_list(&w,0);
        else if(!strcmp(method,"getPlatform"))w_string(&w,"kindle");
        else {fprintf(stderr,"[channel] unimplemented app method=%s\n",method);empty_reply(m);return;}
        respond(m,w.data,w.n);return;
    }
    if(!strcmp(m->channel,"io.material.plugins/dynamic_color")){
        Writer w={0};w_byte(&w,0);w_null(&w);
        respond(m,w.data,w.n);return;
    }
    if(!strcmp(m->channel,"plugins.flutter.io/path_provider")){
        char p[4096];const char *suffix=NULL;
        if(!strcmp(method,"getTemporaryDirectory"))suffix="/cache";
        else if(!strcmp(method,"getCacheDirectory"))suffix="/cache";
        else if(!strcmp(method,"getApplicationSupportDirectory"))suffix="/userdata";
        else if(!strcmp(method,"getApplicationDocumentsDirectory"))suffix="/storage/books";
        else if(!strcmp(method,"getDownloadsDirectory"))suffix="/storage/books";
        if(suffix){snprintf(p,sizeof(p),"%s%s",root,suffix);Writer w={0};w_byte(&w,0);w_string(&w,p);respond(m,w.data,w.n);return;}
    }
    fprintf(stderr,"[channel] unimplemented channel=%s method=%s\n",m->channel,method);
    empty_reply(m);
}
static void dart_log(const char *tag,const char *message,void *u){(void)u;fprintf(stderr,"[dart:%s] %s\n",tag?tag:"",message?message:"");}
static bool present(void *u,const void *pixels,size_t row,size_t height){
    Display *d=u;
    if(!d->frames)fprintf(stderr,"[display] callback row=%zu height=%zu expected=%zux%u\n",row,height,d->pending_stride,d->v.yres);
#ifdef REEDEN_HEADLESS_DIAGNOSTIC
    char path[4096];snprintf(path,sizeof(path),"%s/frame.ppm",root);
    FILE *f=fopen(path,"wb");if(f){fprintf(f,"P6\n%u %zu\n255\n",d->v.xres,height);
        const unsigned char *px=pixels;for(size_t y=0;y<height;y++){for(unsigned x=0;x<d->v.xres;x++){
            const unsigned char *p=px+y*row+x*4;unsigned char rgb[]={p[2],p[1],p[0]};fwrite(rgb,1,3,f);
        }}fclose(f);}
#endif
    bool result=display_present(u,pixels,row,height);wake();return result;
}
static void send_json(const char *channel,const char *json){FlutterPlatformMessage m={.struct_size=sizeof(m),.channel=channel,.message=(const uint8_t*)json,.message_size=strlen(json)};FlutterEngineSendPlatformMessage(engine,&m);}
int main(int argc,char **argv){
    (void)argc;(void)argv;setvbuf(stderr,NULL,_IONBF,0);root=getenv("REEDEN_KOBO_DIR");if(!root){fprintf(stderr,"Run through run.sh; REEDEN_KOBO_DIR missing\n");return 64;}
    fprintf(stderr,"[host] Reeden Kobo experimental 0.7.5 / official core checked by installer\n");
#ifdef REEDEN_HEADLESS_DIAGNOSTIC
    snprintf(heartbeat_path,sizeof(heartbeat_path),"%s/state/heartbeat",root);
#else
    snprintf(heartbeat_path,sizeof(heartbeat_path),"/tmp/reeden-kobo-%ld.heartbeat",(long)getpid());
#endif
    platform_thread=pthread_self();if(pipe2(wake_pipe,O_NONBLOCK|O_CLOEXEC)){perror("pipe");return 1;}
    signal(SIGINT,quit_signal);signal(SIGTERM,quit_signal);signal(SIGHUP,quit_signal);signal(SIGPIPE,SIG_IGN);
    Touch touch;
#ifdef REEDEN_HEADLESS_DIAGNOSTIC
    if(!getenv("REEDEN_DIAGNOSTIC_DATA")||strcmp(root,getenv("REEDEN_DIAGNOSTIC_DATA"))){fprintf(stderr,"Diagnostic requires a separate data root\n");return 64;}
    if(diagnostic_guard()){perror("diagnostic_guard");return 77;}
    memset(&screen,0,sizeof(screen));screen.fd=-1;screen.v.xres=1072;screen.v.yres=1448;screen.pending_stride=1072*4;pthread_mutex_init(&screen.lock,NULL);screen.pending=malloc(screen.pending_stride*1448);if(!screen.pending)return 2;
    memset(&touch,0,sizeof(touch));touch.fd=-1;
#else
    if(display_open(&screen)){perror("display_open");display_close(&screen);return 2;}
    if(screen.turn_options.trace)touch_set_observer(observe_input);
    memset(&touch,0,sizeof(touch));touch.fd=-1;
    light_panel_init(root,(LightPanelOps){.read_number=read_number,.write_number=write_number,.invalidate=light_panel_changed,.userdata=&screen});
    touch_set_filter(light_panel_pointer);
#endif
    char assets[4096],icu[4096],aot_path[4096],cache[4096],ver_path[4096];
    char core[4096];snprintf(core,sizeof(core),"%s/core",root);const char *bundle=getenv("REEDEN_BUNDLE_DIR");if(!bundle)bundle=core;
    snprintf(assets,sizeof(assets),"%s/data/flutter_assets",bundle);snprintf(icu,sizeof(icu),"%s/data/icudtl.dat",bundle);snprintf(aot_path,sizeof(aot_path),"%s/lib/libapp.so",bundle);snprintf(cache,sizeof(cache),"%s/cache",root);
#ifndef REEDEN_HEADLESS_DIAGNOSTIC
    snprintf(ver_path,sizeof(ver_path),"%s/version.json",assets);
    bool core_is_1431=false;
    {FILE *vf=fopen(ver_path,"r");if(vf){char vbuf[256]={0};size_t vn=fread(vbuf,1,sizeof(vbuf)-1,vf);fclose(vf);if(vn>0&&strstr(vbuf,"\"1.43.1\""))core_is_1431=true;}}
    screen.use_light_overlay=core_is_1431;
    if(!core_is_1431)touch_set_filter(NULL);
#else
    (void)ver_path;
#endif
    const char *asset_override=getenv("REEDEN_ASSETS_DIR");
    if(asset_override)snprintf(assets,sizeof(assets),"%s",asset_override);
    FlutterEngineAOTData aot=NULL;FlutterEngineAOTDataSource source={.type=kFlutterEngineAOTDataSourceTypeElfPath,.elf_path=aot_path};
    FlutterEngineResult r=FlutterEngineCreateAOTData(&source,&aot);
    if(r!=kSuccess){fprintf(stderr,"[engine] AOT load failed=%d\n",r);touch_close(&touch,NULL);display_close(&screen);return 4;}
    FlutterRendererConfig renderer={.type=kSoftware,.software={.struct_size=sizeof(FlutterSoftwareRendererConfig),.surface_present_callback=present}};
    FlutterTaskRunnerDescription runner={.struct_size=sizeof(runner),.runs_task_on_current_thread_callback=on_platform_thread,.post_task_callback=post_task,.identifier=1};
    FlutterCustomTaskRunners runners={.struct_size=sizeof(runners),.platform_task_runner=&runner};
    const char *engine_args[]={"reeden-kindle"};
    FlutterProjectArgs args={.struct_size=sizeof(args),.assets_path=assets,.icu_data_path=icu,.command_line_argc=1,.command_line_argv=engine_args,.platform_message_callback=platform_message,.persistent_cache_path=cache,.custom_task_runners=&runners,.shutdown_dart_vm_when_done=true,.aot_data=aot,.log_message_callback=dart_log,.log_tag="reeden"};
    r=FlutterEngineRun(FLUTTER_ENGINE_VERSION,&renderer,&args,&screen,&engine);
    fprintf(stderr,"[engine] FlutterEngineRun result=%d\n",r);if(r!=kSuccess){touch_close(&touch,NULL);display_close(&screen);FlutterEngineCollectAOTData(aot);return 5;}
    FlutterEngineDisplay display={.struct_size=sizeof(display),.display_id=0,.single_display=true,.refresh_rate=60,.width=screen.v.xres,.height=screen.v.yres,.device_pixel_ratio=2};
    fprintf(stderr,"[engine] display update=%d\n",FlutterEngineNotifyDisplayUpdate(engine,kFlutterEngineDisplaysUpdateTypeStartup,&display,1));
    double ratio=2;const char *ratio_env=getenv("REEDEN_PIXEL_RATIO");if(ratio_env){double v=strtod(ratio_env,NULL);if(v>=1&&v<=4)ratio=v;}
    FlutterWindowMetricsEvent metrics={.struct_size=sizeof(metrics),.width=screen.v.xres,.height=screen.v.yres,.pixel_ratio=ratio};fprintf(stderr,"[engine] window metrics=%d size=%zu ratio=%.1f\n",FlutterEngineSendWindowMetricsEvent(engine,&metrics),sizeof(metrics),ratio);
    FlutterLocale zh={.struct_size=sizeof(zh),.language_code="zh",.country_code="CN"};
    FlutterLocale en={.struct_size=sizeof(en),.language_code="en",.country_code="US"};
    const FlutterLocale *locales[]={&zh,&en};FlutterEngineUpdateLocales(engine,locales,2);
    /* disabled FlutterEngineUpdateAccessibilityFeatures to allow Ticker to run */
    send_json("flutter/settings","{\"textScaleFactor\":1.0,\"alwaysUse24HourFormat\":true,\"platformBrightness\":\"light\"}");
    /* flutter/lifecycle expects StringCodec (raw UTF-8), not JSON. */
    {const char *lc="AppLifecycleState.resumed";
     FlutterPlatformMessage lm={.struct_size=sizeof(lm),.channel="flutter/lifecycle",
        .message=(const uint8_t*)lc,.message_size=strlen(lc)};
     fprintf(stderr,"[lifecycle] send result=%d\n",FlutterEngineSendPlatformMessage(engine,&lm));}
    FlutterEngineScheduleFrame(engine);
    int power=-1;bool acquired=false,ready=false,power_down=false;
    char ready_path[4096],handoff[4096];snprintf(ready_path,sizeof(ready_path),"%s/state/ready",root);snprintf(handoff,sizeof(handoff),"%s/state/handoff",root);
    uint64_t escape_at=0;
    uint64_t heartbeat_at=0;
    uint64_t frame_sched_at=monotonic_ns()+500000000ULL;
    while(!app_quit){
        if(monotonic_ns()>heartbeat_at){heartbeat_touch();heartbeat_at=monotonic_ns()+5000000000ULL;}
#ifndef REEDEN_HEADLESS_DIAGNOSTIC
        if(power_worker>0){int status=0;pid_t result=power_worker_poll(power_worker,&status);
            if(result==power_worker){power_worker=-1;fprintf(stderr,"[power] helper status=%d\n",status);
                power_trace("helper-finished");
                if(sleeping&&!(WIFEXITED(status)&&WEXITSTATUS(status)==3)){
                    struct input_event stale[16];while(read(power,stale,sizeof(stale))>0){}power_down=false;
                    set_sleep(&touch,false);
                }else state_file("power-phase",false);
            }else if(result<0&&errno!=EINTR){
                fprintf(stderr,"[power] helper wait failed: %s; returning to native UI\n",strerror(errno));
                power_worker=-1;state_file("asleep",false);app_quit=1;
            }
        }
#endif
        run_tasks();
        if(screen.frames==0&&monotonic_ns()>frame_sched_at){
            FlutterEngineScheduleFrame(engine);
            frame_sched_at=monotonic_ns()+500000000ULL;
        }
        pthread_mutex_lock(&screen.lock);bool frame_available=screen.frames>0;pthread_mutex_unlock(&screen.lock);
        if(frame_available&&!ready){FILE *f=fopen(ready_path,"w");if(!f){perror("ready");break;}fputs("FIRST_FRAME_READY\n",f);fclose(f);ready=true;fprintf(stderr,"[host] first frame ready; waiting for launcher handoff\n");}
#ifndef REEDEN_HEADLESS_DIAGNOSTIC
        if(ready&&!acquired&&access(handoff,F_OK)==0){
            if(display_acquire(&screen)||touch_open(&touch)){perror("hardware handoff");app_quit=1;break;}
            acquired=true;fprintf(stderr,"[host] display/touch ownership acquired\n");
            power=open("/dev/input/event2",O_RDONLY|O_NONBLOCK|O_CLOEXEC);
            if(power<0||ioctl(power,EVIOCGRAB,1)<0){perror("exclusive power key");app_quit=1;break;}
            fprintf(stderr,"[power] exclusive power key acquired\n");
            light_panel_acquired();
        }
        if(acquired&&!sleeping)display_flush(&screen);
        if(acquired&&sleeping&&cover_at&&monotonic_ns()>=cover_at){
            cover_at=0;
            if(*cover_path){
                if(display_cover(&screen,cover_path,cover_width,cover_height,cover_channels)<0){
                    fprintf(stderr,"[sleep-cover] image not confirmed; cancelling suspend\n");
                    if(!app_quit)set_sleep(&touch,false);
                    continue;
                }
                fprintf(stderr,"[sleep-cover] displayed Reeden image channels=%u\n",cover_channels);
            }
            else fprintf(stderr,"[sleep-cover] not registered; keeping current page\n");
            if(display_wait(&screen)<0){perror("sleep display completion");app_quit=1;continue;}
            write_number("/sys/class/backlight/mxc_msp430.0/brightness",0);
            char marker[4096];snprintf(marker,sizeof(marker),"%s/state/power-screen-only",root);
            if(power_worker<0&&access(marker,F_OK)!=0){
                char helper[4096];snprintf(helper,sizeof(helper),"%s/power.sh",root);
                power_trace("starting-helper");power_worker=power_worker_start(helper);
                if(power_worker<0){perror("power helper");state_file("asleep",false);app_quit=1;}
            }
        }

#endif
        bool left=false,right=false;
        for(unsigned i=0;i<16;i++)if(touch.contacts[i].down&&touch.contacts[i].last_y<160){if(touch.contacts[i].last_x<160)left=true;if(touch.contacts[i].last_x>screen.v.xres-160)right=true;}
        if(acquired&&left&&right){if(!escape_at)escape_at=monotonic_ns();else if(monotonic_ns()-escape_at>=2000000000ULL){fprintf(stderr,"[touch] both upper corners held: exit\n");app_quit=1;}}else escape_at=0;
        uint64_t deadline=heartbeat_at;
        uint64_t now=monotonic_ns();
        if(!frame_available&&frame_sched_at<deadline)deadline=frame_sched_at;
        if(!acquired&&now+100000000ULL<deadline)deadline=now+100000000ULL;
        if(sleeping&&cover_at&&cover_at<deadline)deadline=cover_at;
        if(power_worker>0&&now+250000000ULL<deadline)deadline=now+250000000ULL;
        if(escape_at&&escape_at+2000000000ULL<deadline)deadline=escape_at+2000000000ULL;
        if(acquired&&!sleeping){uint64_t update_at=display_deadline(&screen);if(update_at<deadline)deadline=update_at;}
        struct pollfd fds[]={{wake_pipe[0],POLLIN,0},{sleeping?-1:touch.fd,POLLIN,0},{power,POLLIN,0}};
        int n=poll(fds,3,wait_ms(deadline));if(n<0&&errno!=EINTR){perror("poll");break;}
        if(n<0)continue;
        bool input_error=false;
        for(unsigned i=0;i<3;i++)if(fds[i].revents&(POLLNVAL|POLLERR|POLLHUP)){
            fprintf(stderr,"[host] poll descriptor %u failed: revents=0x%x; restoring native UI\n",i,fds[i].revents);
            input_error=true;
        }
        if(input_error)break;
        if(fds[0].revents&POLLIN){char buffer[128];while(read(wake_pipe[0],buffer,sizeof(buffer))>0){}}
        if(fds[1].revents&POLLIN)touch_process(&touch,engine,screen.v.xres,screen.v.yres);
        if(fds[2].revents&POLLIN){struct input_event ev[8];ssize_t k=read(power,ev,sizeof(ev));for(size_t i=0;k>0&&i<(size_t)k/sizeof(*ev);i++)if(ev[i].type==EV_KEY&&ev[i].code==KEY_POWER){
            if(ev[i].value==1)power_down=true;
            if(ev[i].value==0&&power_down){power_down=false;fprintf(stderr,"[power] key release\n");
                if(sleeping&&power_worker>0){state_file("power-cancel",true);fprintf(stderr,"[power] wake requested; waiting for hardware restoration\n");}
                else set_sleep(&touch,!sleeping);
            }
        }}
    }
    fprintf(stderr,"[host] exiting frames=%u updates=%u clean=%u gray=%u color=%u night=%u animated=%u\n",screen.frames,screen.refreshes,screen.clean_refreshes,screen.gray_refreshes,screen.color_refreshes,screen.night_refreshes,screen.animated_refreshes);
    state_file("power-cancel",true);
    if(power_worker>0)power_worker_stop(power_worker);
    if(power>=0)close(power);touch_close(&touch,engine);FlutterEngineShutdown(engine);engine=NULL;FlutterEngineCollectAOTData(aot);display_close(&screen);free(clipboard);
    fprintf(stderr,"[host] shutdown complete\n");
    while(tasks){Pending *p=tasks;tasks=p->next;free(p);}close(wake_pipe[0]);close(wake_pipe[1]);unlink(heartbeat_path);return 0;
}

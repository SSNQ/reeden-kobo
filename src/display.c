#include "kobo.h"
#include "refresh_policy.h"
#include "light_panel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <time.h>
/* Kernel HWTCON UAPI layout; verified against KOReader's mxcfb_kobo_h.lua. */
struct rect { uint32_t top,left,width,height; };
struct update { struct rect region; uint32_t waveform,mode,marker,flags; int dither; };
#define SEND_UPDATE 0x4024462eUL
#define WAIT_COMPLETE 0xc008462fUL
#define WAIT_SUBMISSION 0x40044637UL
_Static_assert(sizeof(struct update)==36,"HWTCON update ABI");
static int send_update(Display *d,struct update *u,TurnTrace *trace){
    uint64_t start=trace?monotonic_ns():0;
    int result=ioctl(d->fd,SEND_UPDATE,u),error=result<0?errno:0;
    if(result>=0)d->last_accepted_marker=u->marker;
    turn_trace_add(trace,"send",start,trace?monotonic_ns():0,
        (RefreshRegion){u->region.left,u->region.top,u->region.width,u->region.height},
        u->marker,u->waveform,u->mode,u->flags,result,error);
    if(result<0)errno=error;return result;
}
static int wait_marker(Display *d,const struct update *u,bool complete,TurnTrace *trace){
    uint32_t marker[2]={u->marker,0};uint64_t start=trace?monotonic_ns():0;
    int result=ioctl(d->fd,complete?WAIT_COMPLETE:WAIT_SUBMISSION,marker),error=result<0?errno:0;
    turn_trace_add(trace,complete?"wait_complete":"wait_submission",start,trace?monotonic_ns():0,
        (RefreshRegion){u->region.left,u->region.top,u->region.width,u->region.height},
        u->marker,u->waveform,u->mode,u->flags,result,error);
    if(result<0)errno=error;return result;
}
static int display_wait_traced(Display *,TurnTrace *);
uint64_t monotonic_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
int read_number(const char *p,int fallback){FILE *f=fopen(p,"r");int n=fallback;if(f){if(fscanf(f,"%d",&n)!=1)n=fallback;fclose(f);}return n;}
int write_number(const char *p,int n){FILE *f=fopen(p,"w");if(!f)return -1;int a=fprintf(f,"%d\n",n);int b=fclose(f);return a>0&&b==0?0:-1;}
static int display_command(const char *command){
    FILE *f=fopen("/proc/hwtcon/cmd","w");if(!f)return -1;
    int a=fputs(command,f),b=fclose(f);return a>=0&&b==0?0:-1;
}
static int display_inversion(Display *d,bool inverted){
    char path[4096];const char *root=getenv("REEDEN_KOBO_DIR");
    if(root){
        snprintf(path,sizeof(path),"%s/state/night-invert",root);
        /* Persist the reset obligation before touching the global knob. */
        if(inverted){FILE *f=fopen(path,"w");if(!f)return -1;if(fclose(f))return -1;}
    }
    if(display_command(inverted?"night_mode 4":"night_mode 0"))return -1;
    if(root&&!inverted)unlink(path);
    d->hw_inverted=inverted;d->hw_state_known=true;
    fprintf(stderr,"[display] hardware inversion=%u\n",inverted);
    return 0;
}
int display_open(Display *d){
    memset(d,0,sizeof(*d));d->fd=-1;pthread_mutex_init(&d->lock,NULL);strcpy(d->mode,"balanced");
    atomic_init(&d->turn_cancel,false);
    if(!turn_options_load(&d->turn_options))fprintf(stderr,"[page-turn] invalid experiment configuration; stable profile retained\n");
    if(!frame_evidence_options_load(&d->evidence_options))fprintf(stderr,"[frame-evidence] invalid configuration; diagnostics disabled\n");
    if(d->evidence_options.directory[0]||d->evidence_options.quality_after_turn||d->evidence_options.reagl_bands)d->turn_options.trace=true;
    const char *p=getenv("REEDEN_FB");if(!p)p="/dev/fb0";
    d->fd=open(p,O_RDONLY|O_CLOEXEC);if(d->fd<0)return -1;
    if(ioctl(d->fd,FBIOGET_VSCREENINFO,&d->v)||ioctl(d->fd,FBIOGET_FSCREENINFO,&d->f))return -1;
    if(strncmp(d->f.id,"hwtcon",6)||d->v.bits_per_pixel!=32||d->v.xres>4096||d->v.yres>4096||!d->v.xres||!d->v.yres){errno=ENOTSUP;return -1;}
    struct fb_bitfield fields[]={d->v.red,d->v.green,d->v.blue,d->v.transp};
    for(unsigned i=0;i<4;i++)if(fields[i].length>8||fields[i].offset+fields[i].length>32||fields[i].msb_right){errno=ENOTSUP;return -1;}
    d->length=d->f.smem_len;
    if((uint64_t)(d->v.yoffset+d->v.yres)*d->f.line_length>d->length || (d->v.xoffset+d->v.xres)*4>d->f.line_length){errno=EINVAL;return -1;}
    close(d->fd);d->fd=-1;
    d->pending_stride=d->v.xres*4;d->pending=malloc(d->pending_stride*d->v.yres);
    d->committed=malloc(d->pending_stride*d->v.yres);if(!d->pending||!d->committed)return -1;
    fprintf(stderr,"[display] %s %ux%u bpp=%u stride=%u rotation=%u RGB offsets=%u/%u/%u\n",d->f.id,d->v.xres,d->v.yres,d->v.bits_per_pixel,d->f.line_length,d->v.rotate,d->v.red.offset,d->v.green.offset,d->v.blue.offset);
    return 0;
}
int display_acquire(Display *d){
    const char *p=getenv("REEDEN_FB");if(!p)p="/dev/fb0";
    d->fd=open(p,O_RDWR|O_CLOEXEC);if(d->fd<0)return -1;
    struct fb_var_screeninfo v;struct fb_fix_screeninfo f;
    if(ioctl(d->fd,FBIOGET_VSCREENINFO,&v)||ioctl(d->fd,FBIOGET_FSCREENINFO,&f))return -1;
    if(v.xres!=d->v.xres||v.yres!=d->v.yres||v.bits_per_pixel!=32||v.rotate!=d->v.rotate||v.xoffset!=d->v.xoffset||v.yoffset!=d->v.yoffset||f.smem_len!=d->f.smem_len||f.line_length!=d->f.line_length||memcmp(&v.red,&d->v.red,sizeof(v.red))||memcmp(&v.green,&d->v.green,sizeof(v.green))||memcmp(&v.blue,&d->v.blue,sizeof(v.blue))){errno=ESTALE;return -1;}
    d->mem=mmap(NULL,d->length,PROT_READ|PROT_WRITE,MAP_SHARED,d->fd,0);if(d->mem==MAP_FAILED){d->mem=NULL;return -1;}
    return display_inversion(d,false);
}
bool display_present(Display *d,const void *pixels,size_t row,size_t height){
    if(!pixels||height!=d->v.yres||row<d->pending_stride){fprintf(stderr,"[display] rejected surface: row=%zu height=%zu\n",row,height);return false;}
    uint64_t arrived=d->turn_options.trace?monotonic_ns():0;
    pthread_mutex_lock(&d->lock);
    bool changed=!d->frames;
    for(unsigned y=0;y<height;y++){
        const uint8_t *src=(const uint8_t*)pixels+y*row;
        uint8_t *dst=d->pending+y*d->pending_stride;
        if(!d->frames||memcmp(dst,src,d->pending_stride)){memcpy(dst,src,d->pending_stride);changed=true;}
    }
    if(d->use_light_overlay&&light_panel_compose(d->pending,d->pending_stride,d->v.xres,d->v.yres))changed=true;
    if(changed){uint64_t now=monotonic_ns();if(!d->dirty)d->first_dirty=now;d->dirty=true;d->last_present=now;
        if(d->turn_options.trace&&d->page_turn_direction){if(!d->turn_first_frame_ns)d->turn_first_frame_ns=arrived;d->turn_last_frame_ns=arrived;}}
    d->frames++;
    if(d->frames==1){d->full=true;fprintf(stderr,"[display] FIRST_FRAME_RECEIVED\n");}
    pthread_mutex_unlock(&d->lock);return true;
}
static uint32_t field(unsigned c,struct fb_bitfield f){return f.length?((c>>(8-f.length))<<f.offset):0;}
void display_full(Display *d){pthread_mutex_lock(&d->lock);if(!d->dirty)d->first_dirty=monotonic_ns();d->full=true;d->dirty=true;pthread_mutex_unlock(&d->lock);}
void display_request_full(Display *d,unsigned delay_ms){
    uint64_t due=monotonic_ns()+(uint64_t)delay_ms*1000000ULL;
    pthread_mutex_lock(&d->lock);
    if(!d->request_clean_at||due<d->request_clean_at)d->request_clean_at=due;
    pthread_mutex_unlock(&d->lock);
}
bool display_arm_page_turn(Display *d,bool forward,bool vertical,unsigned steps){
    pthread_mutex_lock(&d->lock);
    bool ready=d->mem&&d->committed_valid&&steps>=2&&steps<=REFRESH_BANDS_MAX;
    if(ready){d->page_turn_direction=forward?1:-1;d->page_turn_vertical=vertical;d->page_turn_steps=steps;d->page_turn_until=monotonic_ns()+2500000000ULL;
        atomic_store(&d->turn_cancel,false);
        if(d->turn_options.trace){d->turn_input_ns=d->last_input_ns;d->turn_arm_ns=monotonic_ns();d->turn_first_frame_ns=d->turn_last_frame_ns=0;}}
    pthread_mutex_unlock(&d->lock);return ready;
}
void display_cancel_page_turn(Display *d){atomic_store(&d->turn_cancel,true);}
void display_note_input(Display *d,uint64_t at){
    pthread_mutex_lock(&d->lock);d->last_input_ns=at;pthread_mutex_unlock(&d->lock);
}
static unsigned next_marker(Display *d){if(++d->marker==0)++d->marker;return d->marker;}
/* Every region reads the same immutable final framebuffer. Fence all accepted
 * markers before another framebuffer/global inversion change, including on
 * a partial failure. Never wait on a marker rejected by SEND_UPDATE. */
static int submit_sequence(Display *d,struct update *updates,unsigned n,struct update *last,uint64_t gap_ns,
                           const unsigned *positions,unsigned span_pixels,TurnTrace *trace){
    struct update accepted[REFRESH_BANDS_MAX];unsigned count=0,done=0;bool failed=false,cancelled=false;
    uint64_t first_submit=0,last_submit=0,min_gap=UINT64_MAX;
    bool timed=positions!=NULL;uint64_t schedule_start=timed?monotonic_ns():0;
    if(!n||n>REFRESH_BANDS_MAX)return 0;
    for(unsigned i=0;i<n;i++){
        if(timed&&(app_quit||atomic_load(&d->turn_cancel))){cancelled=true;break;}
        if(timed&&d->turn_options.max_inflight&&count-done>=d->turn_options.max_inflight){
            if(wait_marker(d,&accepted[done],true,trace)<0){app_quit=1;return -1;}
            done++;
        }
        /* Submission acceptance can return before a visible panel change.
         * Keep animated bands separated even when the driver queues quickly;
         * normal mixed-color regions have no deliberate delay. */
        if(timed||(gap_ns&&last_submit)){
            uint64_t now=monotonic_ns(),due=timed?turn_due_ns(&d->turn_options,schedule_start,last_submit,positions[i],span_pixels):last_submit+gap_ns;
            if(now<due){
                struct timespec delay={(time_t)((due-now)/1000000000ULL),(long)((due-now)%1000000000ULL)};
                while(nanosleep(&delay,&delay)<0&&errno==EINTR){if(timed&&(app_quit||atomic_load(&d->turn_cancel)))break;}
            }
        }
        if(timed&&(app_quit||atomic_load(&d->turn_cancel))){cancelled=true;break;}
        struct update u=updates[i];u.marker=next_marker(d);
        uint64_t submitted_at=(gap_ns||timed)?monotonic_ns():0;
        if(send_update(d,&u,trace)<0){failed=true;break;}
        if(gap_ns||timed){
            if(!first_submit)first_submit=submitted_at;
            if(last_submit&&submitted_at-last_submit<min_gap)min_gap=submitted_at-last_submit;
            last_submit=submitted_at;
        }
        accepted[count++]=u;*last=u;
        if(wait_marker(d,&u,false,trace)<0){failed=true;break;}
    }
    for(unsigned i=done;i<count;i++){
        if(wait_marker(d,&accepted[i],true,trace)<0){
            fprintf(stderr,"[display-sequence] completion failed: %s\n",strerror(errno));app_quit=1;return -1;
        }
    }
    /* A rejected marker was never submitted and cannot be waited on. */
    if(count)d->completed_marker=accepted[count-1].marker;
    if(failed||cancelled){if(trace)trace->fallback=cancelled?"cancelled":"submission-error";fprintf(stderr,"[display-sequence] %s; restoring normal page update\n",cancelled?"cancelled":"submission failed");return 0;}
    if((gap_ns||timed)&&count>1)fprintf(stderr,"[page-turn] submissions=%u span_ms=%.1f min_gap_ms=%.1f\n",count,(last_submit-first_submit)/1000000.0,min_gap/1000000.0);
    return 1;
}
/* Color pages and explicit cleans never use the animation sweep. */
static int page_turn_bands(Display *d,bool forward,bool night,struct update *last,TurnTrace *trace){
    bool timed=d->turn_options.profile==TURN_TIMED;
    bool reagl=d->evidence_options.reagl_bands;
    unsigned steps=timed?d->turn_options.bands:d->page_turn_steps;
    struct update updates[REFRESH_BANDS_MAX];unsigned positions[REFRESH_BANDS_MAX];
    for(unsigned i=0;i<steps;i++){
        RefreshRegion r;
        unsigned w=d->page_turn_vertical?d->v.yres:d->v.xres,h=d->page_turn_vertical?d->v.xres:d->v.yres;
        if(!refresh_band(w,h,steps,i,forward,&r))return 0;
        positions[i]=forward?w-r.left-r.width:r.left;
        if(d->page_turn_vertical)r=(RefreshRegion){0,r.left,r.height,r.width};
        updates[i]=(struct update){.region={r.top,r.left,r.width,r.height},
            .waveform=night?HWTCON_RAW_NIGHT_BAND:reagl?REFRESH_GLR16:HWTCON_RAW_DAY_BAND,
            .mode=reagl?1:0,.flags=0x8000};
    }
    /* A narrower advancing edge rather than ten page-height coarse blocks.
     * The original speed setting controls both band count and pacing. */
    uint64_t gap_ns=steps<=16?6000000ULL:steps<=24?8000000ULL:10000000ULL;
    int result=submit_sequence(d,updates,steps,last,gap_ns,timed?positions:NULL,positions[steps-1],trace);if(result!=1)return result;
    d->animated_refreshes++;
    fprintf(stderr,"[page-turn] bands=%u direction=%s vertical=%u night=%u path=%s wave=%u mode=%u\n",steps,forward?"next":"prev",d->page_turn_vertical,night,reagl?"reagl":"stable",last->waveform,last->mode);
    return 1;
}
static int night_color_parts(Display *d,const RefreshPart *parts,unsigned n,struct update *last,TurnTrace *trace){
    struct update updates[REFRESH_PARTS_MAX];unsigned color_rows=0;
    for(unsigned i=0;i<n;i++){
        RefreshRegion r=parts[i].region;
        updates[i]=(struct update){.region={r.top,r.left,r.width,r.height},
            .waveform=parts[i].color?REFRESH_GLRC16:REFRESH_GLKW16,
            .mode=1,.flags=parts[i].color?REFRESH_CFA_G2:0};
        if(parts[i].color)color_rows+=r.height;
    }
    int result=submit_sequence(d,updates,n,last,0,NULL,0,trace);if(result!=1)return result;
    d->localized_color_refreshes++;
    fprintf(stderr,"[night-color] regions=%u cfa_rows=%u/%u\n",n,color_rows,d->v.yres);
    return 1;
}
static uint64_t deadline_locked(Display *d){
    if(!d->frames)return UINT64_MAX;
    uint64_t interval=!strcmp(d->mode,"fast")?80000000ULL:140000000ULL;
    uint64_t settled=d->last_present+40000000ULL,limit=d->first_dirty+400000000ULL;
    if(settled>limit)settled=limit;
    uint64_t allowed=d->last_refresh+interval;
    uint64_t pixels_due=d->dirty?(allowed>settled?allowed:settled):UINT64_MAX;
    return refresh_deadline(pixels_due,d->request_clean_at,allowed,monotonic_ns());
}
uint64_t display_deadline(Display *d){pthread_mutex_lock(&d->lock);uint64_t t=deadline_locked(d);pthread_mutex_unlock(&d->lock);return t;}
static int flush(Display *d,const uint8_t *cover){
    uint64_t now=monotonic_ns();pthread_mutex_lock(&d->lock);
    if(!cover&&now<deadline_locked(d)){pthread_mutex_unlock(&d->lock);return 0;}
    if(!cover&&d->request_clean_at&&now>=d->request_clean_at){
        d->request_clean_at=0;
        bool same=d->committed_valid&&!memcmp(d->pending,d->committed,d->pending_stride*d->v.yres);
        if(!d->full&&refresh_duplicate_clean(now,d->last_clean,same)){
            fprintf(stderr,"[refresh] coalesced duplicate cleanup for unchanged pixels\n");
        }else{
            if(!d->dirty)d->first_dirty=now;
            d->full=true;d->dirty=true;
        }
    }
    if(!cover&&!d->dirty){pthread_mutex_unlock(&d->lock);return 0;}
    const uint8_t *source=cover?cover:d->pending;
    RefreshPlan plan;
    if(!refresh_plan(d->committed_valid?d->committed:NULL,source,d->pending_stride,
                     d->v.xres,d->v.yres,cover||d->full,d->mode,&plan)){
        d->dirty=false;pthread_mutex_unlock(&d->lock);return cover?-1:0;
    }
    TurnTrace storage,*trace=NULL;
    if(d->turn_options.trace){
        turn_trace_init(&storage,true,++d->trace_serial);trace=&storage;
        trace->options=d->turn_options;
        trace->flush=now;
        if(d->page_turn_direction&&now<=d->page_turn_until){
            trace->input=d->turn_input_ns;trace->arm=d->turn_arm_ns;trace->first_frame=d->turn_first_frame_ns;trace->last_frame=d->turn_last_frame_ns;
        }
    }
    if(cover)plan.hw_invert=false;
    else{
        if(!plan.frame_night)d->direct_night=false;
        else if(plan.frame_color)d->direct_night=true;
        /* Keep the direct RGB path for this dark reading session after
         * seeing color, even on later gray pages. Avoid toggling a global
         * encoding flag every time one colored line enters/leaves view. */
        if(d->direct_night)plan.hw_invert=false;
    }
    bool inversion_change=!d->hw_state_known||d->hw_inverted!=plan.hw_invert;
    if(inversion_change){
        /* Every pixel's physical encoding changes with this GLOBAL flag. */
        bool clean=plan.clean,target_inverted=plan.hw_invert;
        refresh_plan(d->committed_valid?d->committed:NULL,source,d->pending_stride,
                     d->v.xres,d->v.yres,true,d->mode,&plan);
        /* Re-encode all pixels without inventing a flashing cleanup. In
         * particular, one colored word entering/leaving a dark page must
         * retain GLRC16 instead of being promoted to a GCC16 white flash. */
        if(!clean){
            plan.clean=false;
            plan.waveform=plan.color?REFRESH_GLRC16:plan.night?REFRESH_GLKW16:REFRESH_GLR16;
        }
        plan.hw_invert=target_inverted;
    }
    RefreshPart parts[REFRESH_PARTS_MAX];unsigned part_count=0;
    if(!cover&&!plan.clean&&plan.major&&plan.frame_night&&plan.color&&!plan.hw_invert)
        part_count=refresh_night_parts(d->committed_valid?d->committed:NULL,source,
                                     d->pending_stride,d->v.xres,d->v.yres,parts);
    bool semantic_turn=!cover&&d->page_turn_direction&&now<=d->page_turn_until&&plan.major;
    bool animate=semantic_turn&&!plan.clean&&!plan.color&&!plan.frame_color&&!inversion_change&&d->v.xres>=160;
    if(d->turn_options.profile==TURN_OFF)animate=false;
    if(d->turn_options.profile==TURN_TIMED&&
       (d->v.xres!=1072||d->v.yres!=1448||d->v.rotate!=3))animate=false;
    if(d->evidence_options.reagl_bands&&
       (d->v.xres!=1072||d->v.yres!=1448||d->v.rotate!=3))animate=false;
    bool forward=d->page_turn_direction>0;
    if(cover||plan.major||plan.clean||now>d->page_turn_until)d->page_turn_direction=0;
    if(animate||part_count)plan.region=(RefreshRegion){0,0,d->v.xres,d->v.yres};
    if(trace)trace->planned=monotonic_ns();
    /* Wake before either wait or submit, as in KOReader's MTK path. */
    if(display_command("fiti_power 1")){perror("display wake");app_quit=1;turn_trace_dump(trace,"wake-error",monotonic_ns());pthread_mutex_unlock(&d->lock);return -1;}
    if(display_wait_traced(d,trace)<0){fprintf(stderr,"[display] wait for prior update failed: %s\n",strerror(errno));app_quit=1;turn_trace_dump(trace,"prior-wait-error",monotonic_ns());pthread_mutex_unlock(&d->lock);return -1;}
    if(inversion_change&&display_inversion(d,plan.hw_invert)){perror("display inversion");app_quit=1;turn_trace_dump(trace,"invert-error",monotonic_ns());pthread_mutex_unlock(&d->lock);return -1;}
    FrameCapture capture={.fd=-1};unsigned quality_status=0;
    if(semantic_turn){
        FrameEvidenceInfo info={.width=d->v.xres,.height=d->v.yres,.logical_stride=d->pending_stride,
            .rotation=d->v.rotate,.fb_stride=d->f.line_length,.xoffset=d->v.xoffset,.yoffset=d->v.yoffset,
            .red=d->v.red,.green=d->v.green,.blue=d->v.blue,.alpha=d->v.transp,.plan=plan,
            .old_valid=d->committed_valid,.animated=animate,.trace_id=trace?trace->id:0,
            .band_path=d->evidence_options.reagl_bands?"reagl":"stable",
            .profile=d->turn_options.profile==TURN_OFF?"off":d->turn_options.profile==TURN_TIMED?"timed":"stable"};
        capture=frame_capture_begin(&d->evidence_options,&info,d->committed,source,d->mem,d->length);
    }
    unsigned endx=plan.region.left+plan.region.width,endy=plan.region.top+plan.region.height;
    uint32_t alpha=field(255,d->v.transp);
    bool native_rgb=d->v.red.length==8&&d->v.green.length==8&&d->v.blue.length==8&&
        d->v.red.offset==0&&d->v.green.offset==8&&d->v.blue.offset==16;
    if(trace)trace->copy_begin=monotonic_ns();
    for(unsigned y=plan.region.top;y<endy;y++){
        uint32_t *dst=(uint32_t*)(d->mem+(y+d->v.yoffset)*d->f.line_length+d->v.xoffset*4);
        const uint8_t *src=source+y*d->pending_stride;
        for(unsigned x=plan.region.left;x<endx;x++){
            const uint8_t *p=src+x*4;
            unsigned b=p[0],g=p[1],r=p[2];
            /* Dart already rendered the dark theme. Give the driver's
             * inversion the complementary grayscale input, keeping the
             * logical committed image unchanged. Real-color frames remain
             * in the direct CFA path: post-CFA inversion is not RGB inverse. */
            if(plan.hw_invert){b=255-b;g=255-g;r=255-r;}
            dst[x]=native_rgb?((uint32_t)b<<16)|((uint32_t)g<<8)|r|alpha:
                field(r,d->v.red)|field(g,d->v.green)|field(b,d->v.blue)|alpha;
        }
    }
    if(trace)trace->copied=monotonic_ns();
    frame_capture_pixels(&capture,"fb-converted.bin",d->mem,d->length);
    {
        /* REAGL and Kaleido both require UPDATE_MODE_FULL on this model;
         * that flag does not itself mean a full-screen flashing cleanup. */
        struct update u={0};
        int bands=animate?page_turn_bands(d,forward,plan.night,&u,trace):
                  part_count?night_color_parts(d,parts,part_count,&u,trace):0;
        if(bands<0){frame_capture_end(&capture,"sequence-wait-error",d->last_accepted_marker,0);turn_trace_dump(trace,"sequence-wait-error",monotonic_ns());pthread_mutex_unlock(&d->lock);return -1;}
        /* Diagnostic comparison only, default OFF. Reuse the ordinary gray
         * reading waveform/mode/flags after ALL strip markers have completed.
         * No forced GC16, no pixel changes, no claimed optical improvement.
         * Restrict to measured geometry; color, clean and fallback sequences
         * never acquire an extra update. quality_status: 0=off/skipped,
         * 1=completed, 2=send rejected, 3=completion failed. */
        if(bands==1&&animate&&d->evidence_options.quality_after_turn&&
           d->v.xres==1072&&d->v.yres==1448&&d->v.rotate==3){
            struct update q={.region={0,0,d->v.xres,d->v.yres},
                .waveform=plan.night?REFRESH_GLKW16:REFRESH_GLR16,.mode=1,.marker=next_marker(d)};
            if(send_update(d,&q,trace)<0){quality_status=2;fprintf(stderr,"[text-quality] optional update rejected: %s\n",strerror(errno));}
            else{
                if(wait_marker(d,&q,true,trace)<0){
                    app_quit=1;frame_capture_end(&capture,"quality-wait-error",q.marker,3);
                    turn_trace_dump(trace,"quality-wait-error",monotonic_ns());pthread_mutex_unlock(&d->lock);return -1;
                }
                d->completed_marker=q.marker;d->quality_refreshes++;quality_status=1;u=q;
                fprintf(stderr,"[text-quality] completed ordinary whole-page comparison wave=%u mode=1 flags=0 marker=%u\n",q.waveform,q.marker);
            }
        }
        if(!bands)u=(struct update){.region={plan.region.top,plan.region.left,plan.region.width,plan.region.height},.waveform=plan.waveform,.mode=1,.marker=next_marker(d),.flags=plan.flags};
        if(!bands&&send_update(d,&u,trace)<0){fprintf(stderr,"[display] HWTCON_SEND_UPDATE failed: %s\n",strerror(errno));d->full=true;d->failures++;if(d->failures>=3)app_quit=1;}
        else {
            /* Never call an unconfirmed target the committed image. This
             * error-path fix leaves successful profile timings/waves intact. */
            if(!bands){
                if(wait_marker(d,&u,true,trace)<0){app_quit=1;frame_capture_end(&capture,"complete-error",u.marker,quality_status);turn_trace_dump(trace,"complete-error",monotonic_ns());pthread_mutex_unlock(&d->lock);return -1;}
                d->completed_marker=u.marker;
            }
            for(unsigned y=plan.region.top;y<endy;y++)memcpy(
                d->committed+y*d->pending_stride+plan.region.left*4,
                source+y*d->pending_stride+plan.region.left*4,plan.region.width*4);
            d->committed_valid=true;
            d->full=false;d->failures=0;d->refreshes++;
            if(plan.clean){d->last_clean=monotonic_ns();d->clean_refreshes++;}
            if(plan.night&&!plan.color)d->night_refreshes++;
            if(plan.color)d->color_refreshes++;else d->gray_refreshes++;
            if(cover)d->request_clean_at=0;
            /* FULL REAGL/CFA transactions are completed before another
             * framebuffer or global inversion change, matching KOReader. */
            if(display_wait_traced(d,trace)<0){fprintf(stderr,"[display] wait for completion failed: %s\n",strerror(errno));app_quit=1;}
            if(d->refreshes==1)fprintf(stderr,"[display] FIRST_UPDATE_SUBMITTED marker=%u\n",u.marker);
            if(d->refreshes<=30||plan.clean||d->last_waveform!=u.waveform)fprintf(stderr,"[refresh] marker=%u wave=%u clean=%u color=%u night=%u hw_invert=%u flags=0x%x dither=%d transition=%u region=%u,%u %ux%u changed=%u work_ms=%.1f\n",
                u.marker,u.waveform,plan.clean,plan.color,plan.night,plan.hw_invert,u.flags,u.dither,plan.polarity_changed,plan.region.left,plan.region.top,plan.region.width,plan.region.height,plan.changed,(monotonic_ns()-now)/1000000.0);
            d->last_waveform=u.waveform;
        }
        d->last_refresh=monotonic_ns();
    }
    frame_capture_pixels(&capture,"fb-complete.bin",d->mem,d->length);
    frame_capture_end(&capture,app_quit?"quit-or-error":d->failures?"send-error":"complete",d->last_accepted_marker,quality_status);
    turn_trace_dump(trace,app_quit?"quit-or-error":d->failures?"send-error":"complete",monotonic_ns());
    d->dirty=d->failures>0;int result=(d->failures||app_quit)?-1:0;pthread_mutex_unlock(&d->lock);return result;
}
void display_flush(Display *d){flush(d,NULL);}
static int display_wait_traced(Display *d,TurnTrace *trace){
    if(!d->last_accepted_marker||d->completed_marker==d->last_accepted_marker)return 0;
    struct update u={.marker=d->last_accepted_marker};
    if(trace)for(unsigned i=trace->count;i>0;i--){
        const TurnEvent *e=&trace->event[i-1];
        if(e->marker==u.marker&&!strcmp(e->op,"send")&&e->result>=0){
            u.region=(struct rect){e->region.top,e->region.left,e->region.width,e->region.height};
            u.waveform=e->wave;u.mode=e->mode;u.flags=e->flags;break;
        }
    }
    int result=wait_marker(d,&u,true,trace);
    if(result>=0)d->completed_marker=u.marker;
    return result;
}
int display_wait(Display *d){return display_wait_traced(d,NULL);}
int display_cover(Display *d,const char *path,unsigned width,unsigned height,unsigned channels){
    if(!width||!height||width>4096||height>4096||channels<1||channels>4){errno=EINVAL;return -1;}
    FILE *f=fopen(path,"rb");if(!f)return -1;
    size_t total=(size_t)width*height*channels;
    uint8_t *raw=malloc(total),*bgra=malloc(d->pending_stride*d->v.yres);
    if(!raw||!bgra){free(raw);free(bgra);fclose(f);errno=ENOMEM;return -1;}
    bool valid=fread(raw,1,total,f)==total&&fgetc(f)==EOF;fclose(f);
    if(!valid){free(raw);free(bgra);errno=EINVAL;return -1;}
    if((channels==2||channels==4)&&d->committed_valid&&d->committed)
        memcpy(bgra,d->committed,d->pending_stride*d->v.yres);
    else memset(bgra,255,d->pending_stride*d->v.yres);
    int ox=((int)d->v.xres-(int)width)/2,oy=((int)d->v.yres-(int)height)/2;
    for(unsigned y=0;y<height;y++)for(unsigned x=0;x<width;x++){
        int dx=(int)x+ox,dy=(int)y+oy;if(dx<0||dy<0||dx>=(int)d->v.xres||dy>=(int)d->v.yres)continue;
        const uint8_t *px=raw+((size_t)y*width+x)*channels;
        uint8_t sr=px[0],sg=channels>=3?px[1]:px[0],sb=channels>=3?px[2]:px[0];
        uint8_t sa=channels==2?px[1]:(channels==4?px[3]:255);
        uint8_t *p=bgra+dy*d->pending_stride+dx*4;
        if(sa==255){p[0]=sb;p[1]=sg;p[2]=sr;p[3]=255;}
        else if(sa>0){
            unsigned inv=255u-sa;
            unsigned b=sb+((unsigned)p[0]*inv*0x8081u>>23);
            unsigned g=sg+((unsigned)p[1]*inv*0x8081u>>23);
            unsigned r=sr+((unsigned)p[2]*inv*0x8081u>>23);
            p[0]=b>255?255:(uint8_t)b;
            p[1]=g>255?255:(uint8_t)g;
            p[2]=r>255?255:(uint8_t)r;
            p[3]=255;
        }
    }
    int result=flush(d,bgra);free(raw);free(bgra);return result;
}
void display_close(Display *d){if(d->mem&&d->hw_state_known){display_wait(d);if(display_inversion(d,false))perror("reset display inversion");}if(d->mem)munmap(d->mem,d->length);if(d->fd>=0)close(d->fd);free(d->pending);free(d->committed);pthread_mutex_destroy(&d->lock);}

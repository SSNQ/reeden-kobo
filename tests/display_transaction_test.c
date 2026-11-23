#define _GNU_SOURCE
#include "kobo.h"
#include "refresh_policy.h"
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

volatile int app_quit;
bool light_panel_compose(uint8_t *p,size_t s,unsigned w,unsigned h){(void)p;(void)s;(void)w;(void)h;return false;}
static FILE *command_file;
static bool inverted,in_flight,fail_inversion;
static unsigned submitted,completed,last_wave,last_flags,commands;
static unsigned outstanding,band_count,fail_band_at,submission_waits;
static bool pending_markers[1024],needs_submission_wait;
static unsigned band_left[128];
static unsigned band_top[128];
static bool expect_parts;
static unsigned part_color_rows,part_gray_rows;
static uint64_t last_band_submit;
static unsigned cancel_at_band,fail_submission_at,peak_outstanding;
static bool fail_complete_once,fail_normal_once;
static unsigned fail_complete_marker;
static uint8_t *immutable_image;
static uint32_t last_first_pixel;
static Display *display;
static FILE *__real_fopen(const char *path,const char *mode){return fopen(path,mode);}
static int __real_fclose(FILE *f){return fclose(f);}
FILE *__wrap_fopen(const char *path,const char *mode){
    if(!strcmp(path,"/proc/hwtcon/cmd")){assert(!command_file);return command_file=tmpfile();}
    return __real_fopen(path,mode);
}
int __wrap_fclose(FILE *f){
    if(f==command_file){
        char command[64]={0};fflush(f);rewind(f);assert(fread(command,1,sizeof(command)-1,f)>0);
        command_file=NULL;commands++;
        if(!strcmp(command,"night_mode 4")){
            assert(!in_flight);
            if(fail_inversion){__real_fclose(f);errno=EIO;return EOF;}
            inverted=true;
        }else if(!strcmp(command,"night_mode 0")){assert(!in_flight);inverted=false;}
        else assert(!strcmp(command,"fiti_power 1"));
    }
    return __real_fclose(f);
}
struct update_record {uint32_t top,left,width,height,waveform,mode,marker,flags;int dither;};
int __wrap_ioctl(int fd,unsigned long request,...){
    /* Timestamp call entry, before the mock's multi-megabyte memcmp. On ARM
     * that validation cost varies enough to distort an internal timestamp. */
    uint64_t ioctl_entered=monotonic_ns();
    assert(fd==99);va_list args;va_start(args,request);void *p=va_arg(args,void *);va_end(args);
    if(outstanding)assert(immutable_image&&!memcmp(immutable_image,display->mem,display->length));
    if(request==0x4024462eUL){
        struct update_record *u=p;assert(u->dither==0&&!needs_submission_wait);
        assert(u->left%16==0&&u->top%16==0&&u->left+u->width<=display->v.xres&&u->top+u->height<=display->v.yres);
        if(u->flags==0x8000&&(u->width<display->v.xres||u->height<display->v.yres)){
            uint64_t now=ioctl_entered;
            uint64_t minimum=display->page_turn_steps<=16?4000000ULL:display->page_turn_steps<=24?6000000ULL:8000000ULL;
            if(outstanding&&display->turn_options.profile!=TURN_TIMED)assert(now-last_band_submit>=minimum);
            last_band_submit=now;
            band_count++;assert(u->flags==0x8000);
            if(display->evidence_options.reagl_bands)assert(u->mode==1&&(u->waveform==REFRESH_GLR16||u->waveform==REFRESH_GLKW16));
            else assert(u->mode==0&&(u->waveform==HWTCON_RAW_DAY_BAND||u->waveform==HWTCON_RAW_NIGHT_BAND));
            if(band_count==fail_band_at){fail_band_at=0;errno=EIO;return -1;}
            band_left[band_count%128]=u->left;band_top[band_count%128]=u->top;needs_submission_wait=true;
        }else if(expect_parts&&u->height<display->v.yres){
            assert(u->mode==1&&u->left==0&&u->width==display->v.xres&&u->top%48==0);
            if(u->flags==REFRESH_CFA_G2){assert(u->waveform==REFRESH_GLRC16);part_color_rows+=u->height;}
            else {assert(u->flags==0&&u->waveform==REFRESH_GLKW16);part_gray_rows+=u->height;}
            needs_submission_wait=true;
        }else {assert(u->mode==1&&!outstanding);if(fail_normal_once){fail_normal_once=false;errno=EIO;return -1;}}
        assert(u->marker<1024&&!pending_markers[u->marker]);
        if(!outstanding){immutable_image=malloc(display->length);assert(immutable_image);memcpy(immutable_image,display->mem,display->length);}
        pending_markers[u->marker]=true;outstanding++;if(outstanding>peak_outstanding)peak_outstanding=outstanding;
        submitted=u->marker;last_wave=u->waveform;last_flags=u->flags;
        last_first_pixel=((uint32_t*)display->mem)[0];in_flight=true;
    }else if(request==0x40044637UL){
        assert(*(uint32_t*)p==submitted&&needs_submission_wait);
        needs_submission_wait=false;submission_waits++;
        if(cancel_at_band&&band_count==cancel_at_band){cancel_at_band=0;display_cancel_page_turn(display);}
        if(fail_submission_at==submission_waits){fail_submission_at=0;errno=EIO;return -1;}
    }else{
        assert(request==0xc008462fUL);unsigned marker=*(uint32_t*)p;
        assert(marker<1024&&pending_markers[marker]&&outstanding);
        if(fail_complete_once||marker==fail_complete_marker){fail_complete_once=false;fail_complete_marker=0;errno=EIO;return -1;}
        pending_markers[marker]=false;outstanding--;
        if(!outstanding){free(immutable_image);immutable_image=NULL;}
        completed=marker;in_flight=outstanding>0;
    }
    return 0;
}
#define ioctl __wrap_ioctl
#define fopen __wrap_fopen
#define fclose __wrap_fclose
#include "../src/display.c"
#undef ioctl
#undef fopen
#undef fclose
static void flush_now(Display *d){d->last_refresh=d->last_present=d->first_dirty=0;display_flush(d);assert(!in_flight&&completed==submitted);}
static void check_page_turns(Display *original){
    Display d={.fd=99,.pending_stride=640*4,.length=640*64*4};display=&d;
    d.v.xres=640;d.v.yres=64;d.f.line_length=d.pending_stride;
    d.v.red=(struct fb_bitfield){0,8,0};d.v.green=(struct fb_bitfield){8,8,0};
    d.v.blue=(struct fb_bitfield){16,8,0};d.v.transp=(struct fb_bitfield){24,8,0};
    d.pending=malloc(d.length);d.committed=malloc(d.length);d.mem=calloc(1,d.length);
    assert(d.pending&&d.committed&&d.mem);pthread_mutex_init(&d.lock,NULL);strcpy(d.mode,"balanced");
    memset(d.pending,255,d.length);d.frames=1;d.dirty=d.full=true;flush_now(&d);
    assert(!d.animated_refreshes);
    assert(display_arm_page_turn(&d,true,false,10));
    for(unsigned y=8;y<56;y++)for(unsigned x=16;x<d.v.xres-16;x++){
        uint8_t *p=d.pending+y*d.pending_stride+x*4;p[0]=p[1]=p[2]=200;
    }
    d.dirty=true;unsigned at=band_count;flush_now(&d);
    assert(d.animated_refreshes==1&&band_count==at+10&&submission_waits>=10);
    for(unsigned i=1;i<10;i++)assert(band_left[(at+i)%128]>band_left[(at+i+1)%128]);
    assert(!memcmp(d.pending,d.committed,d.length));
    assert(display_arm_page_turn(&d,false,false,10));memset(d.pending,255,d.length);d.dirty=true;at=band_count;flush_now(&d);
    assert(d.animated_refreshes==2&&band_count==at+10);
    for(unsigned i=1;i<10;i++)assert(band_left[(at+i)%128]<band_left[(at+i+1)%128]);
    /* A failed third band still displays the complete new page, after all
     * accepted band markers finish. No half-page or unsubmitted marker wait. */
    assert(display_arm_page_turn(&d,true,false,10));memset(d.pending,200,d.length);d.dirty=true;
    fail_band_at=band_count+3;flush_now(&d);assert(d.animated_refreshes==2&&last_wave==REFRESH_GLR16);
    assert(!memcmp(d.pending,d.committed,d.length));
    assert(display_arm_page_turn(&d,true,false,10));display_full(&d);at=band_count;flush_now(&d);
    assert(band_count==at&&last_wave==REFRESH_GC16);
    assert(display_arm_page_turn(&d,true,true,2));memset(d.pending,150,d.length);d.dirty=true;
    at=band_count;flush_now(&d);assert(d.animated_refreshes==3&&band_count==at+2);
    assert(band_top[(at+1)%128]>band_top[(at+2)%128]);
    /* Actual color, even unchanged outside the dirty box, must never be sent
     * through grayscale strips. */
    d.pending[0]=250;d.pending[1]=20;d.pending[2]=30;d.dirty=true;flush_now(&d);
    assert(display_arm_page_turn(&d,true,false,10));
    for(unsigned y=8;y<56;y++)for(unsigned x=16;x<d.v.xres-16;x++){
        uint8_t *p=d.pending+y*d.pending_stride+x*4;p[0]=p[1]=p[2]=150;
    }
    d.dirty=true;at=band_count;flush_now(&d);assert(band_count==at);
    /* An old arm cannot animate an unrelated later page/menu. */
    memset(d.pending,255,d.length);d.dirty=true;flush_now(&d);
    assert(display_arm_page_turn(&d,true,false,10));d.page_turn_until=0;memset(d.pending,200,d.length);d.dirty=true;
    at=band_count;flush_now(&d);assert(band_count==at);
    memset(d.pending,0,d.length);display_full(&d);flush_now(&d);
    assert(display_arm_page_turn(&d,true,false,10));
    for(unsigned y=8;y<56;y++)for(unsigned x=16;x<d.v.xres-16;x++){
        uint8_t *p=d.pending+y*d.pending_stride+x*4;p[0]=p[1]=p[2]=80;
    }
    d.dirty=true;flush_now(&d);assert(d.animated_refreshes==4&&last_wave==REFRESH_GLKW16&&inverted);
    /* The original fast setting uses fewer paced bands while preserving
     * the same complete final image and completion fences. */
    assert(display_arm_page_turn(&d,true,false,6));
    memset(d.pending,60,d.length);d.dirty=true;at=band_count;flush_now(&d);
    assert(d.animated_refreshes==5&&band_count==at+6&&last_wave==REFRESH_GLKW16);
    assert(!memcmp(d.pending,d.committed,d.length));
    /* Fine bands cover the final image in either direction and retain all
     * completion barriers with the larger hardware transaction sequence. */
    for(unsigned steps=16;steps<=REFRESH_BANDS_MAX;steps+=8){
        assert(display_arm_page_turn(&d,false,false,steps));
        memset(d.pending,steps,d.length);d.dirty=true;
        at=band_count;unsigned before=d.animated_refreshes;flush_now(&d);
        assert(d.animated_refreshes==before+1&&band_count==at+steps);
        for(unsigned i=1;i<steps;i++)assert(band_left[(at+i)%128]<band_left[(at+i+1)%128]);
        assert(!memcmp(d.pending,d.committed,d.length));
    }
    assert(!display_inversion(&d,false));
    free(d.pending);free(d.committed);free(d.mem);pthread_mutex_destroy(&d.lock);display=original;
}
static void check_localized_color(Display *original){
    Display d={.fd=99,.pending_stride=192*4,.length=192*768*4};display=&d;
    d.v.xres=192;d.v.yres=768;d.f.line_length=d.pending_stride;
    d.v.red=(struct fb_bitfield){0,8,0};d.v.green=(struct fb_bitfield){8,8,0};
    d.v.blue=(struct fb_bitfield){16,8,0};d.v.transp=(struct fb_bitfield){24,8,0};
    d.pending=calloc(1,d.length);d.committed=malloc(d.length);d.mem=calloc(1,d.length);
    assert(d.pending&&d.committed&&d.mem);pthread_mutex_init(&d.lock,NULL);strcpy(d.mode,"balanced");
    d.frames=1;d.dirty=d.full=true;flush_now(&d);assert(inverted);
    for(unsigned y=8;y<760;y+=8)for(unsigned x=16;x<176;x++){
        uint8_t *p=d.pending+y*d.pending_stride+x*4;p[0]=p[1]=p[2]=80;
    }
    d.pending[300*d.pending_stride+20*4]=200;
    d.dirty=true;expect_parts=true;part_color_rows=part_gray_rows=0;flush_now(&d);
    assert(d.localized_color_refreshes==1&&d.clean_refreshes==1&&d.direct_night&&!inverted);
    assert(part_color_rows==144&&part_gray_rows==624);
    assert(!memcmp(d.pending,d.committed,d.length));
    d.pending[300*d.pending_stride+20*4]=0;
    for(unsigned y=8;y<760;y+=8)for(unsigned x=16;x<176;x++){
        uint8_t *p=d.pending+y*d.pending_stride+x*4;p[0]=p[1]=p[2]=90;
    }
    d.dirty=true;part_color_rows=part_gray_rows=0;flush_now(&d);
    assert(d.localized_color_refreshes==2&&part_color_rows==144&&!inverted);
    assert(d.direct_night&&d.clean_refreshes==1&&!memcmp(d.pending,d.committed,d.length));
    expect_parts=false;
    /* A real image takes the normal CFA path; explicit full clean is honored. */
    for(unsigned y=0;y<768;y++)for(unsigned x=0;x<192;x++){
        uint8_t *p=d.pending+y*d.pending_stride+x*4;p[0]=200;p[1]=p[2]=0;
    }
    d.dirty=true;flush_now(&d);assert(d.localized_color_refreshes==2&&last_wave==REFRESH_GLRC16);
    display_full(&d);flush_now(&d);assert(last_wave==REFRESH_GCC16&&d.clean_refreshes==2);
    assert(!display_inversion(&d,false));
    free(d.pending);free(d.committed);free(d.mem);pthread_mutex_destroy(&d.lock);display=original;
}
static void check_experimental_turns(Display *original){
    Display d={.fd=99,.pending_stride=1072*4,.length=1072*1448*4};display=&d;
    d.v.xres=1072;d.v.yres=1448;d.v.rotate=3;d.f.line_length=d.pending_stride;
    d.v.red=(struct fb_bitfield){0,8,0};d.v.green=(struct fb_bitfield){8,8,0};
    d.v.blue=(struct fb_bitfield){16,8,0};d.v.transp=(struct fb_bitfield){24,8,0};
    d.pending=malloc(d.length);d.committed=malloc(d.length);d.mem=calloc(1,d.length);
    assert(d.pending&&d.committed&&d.mem);pthread_mutex_init(&d.lock,NULL);strcpy(d.mode,"balanced");
    memset(d.pending,255,d.length);d.frames=1;d.dirty=d.full=true;flush_now(&d);
    d.turn_options=(TurnOptions){.profile=TURN_OFF,.trace=true};
    assert(display_arm_page_turn(&d,true,false,16));memset(d.pending,190,d.length);d.dirty=true;
    unsigned at=band_count;flush_now(&d);assert(band_count==at&&!d.animated_refreshes);
    d.turn_options=(TurnOptions){.profile=TURN_TIMED,.bands=16,.span_us=150000,.min_gap_us=2000,.trace=true};
    /* Several pending arm commands coalesce to the latest semantic direction;
     * this does not pretend to measure live input during the blocking loop. */
    for(unsigned turn=0;turn<6;turn++){
        assert(display_arm_page_turn(&d,true,false,16));
        assert(display_arm_page_turn(&d,turn%2==0,false,16));
        memset(d.pending,turn%2?190:210,d.length);d.dirty=true;at=band_count;
        flush_now(&d);assert(band_count==at+16&&d.animated_refreshes==turn+1);
        for(unsigned i=1;i<16;i++){
            unsigned a=band_left[(at+i)%128],b=band_left[(at+i+1)%128];
            assert(turn%2?a<b:a>b);
        }
        assert(!memcmp(d.pending,d.committed,d.length));
    }
    d.turn_options.max_inflight=4;peak_outstanding=0;
    assert(display_arm_page_turn(&d,true,false,16));memset(d.pending,180,d.length);d.dirty=true;flush_now(&d);
    assert(peak_outstanding<=4);
    d.evidence_options.reagl_bands=true;
    assert(display_arm_page_turn(&d,true,false,16));memset(d.pending,190,d.length);d.dirty=true;
    at=band_count;flush_now(&d);assert(band_count==at+16&&last_wave==REFRESH_GLR16&&last_flags==0x8000);
    assert(!memcmp(d.pending,d.committed,d.length));
    d.evidence_options.reagl_bands=false;
    /* One optional quality probe, after every accepted strip was fenced.
     * Same target pixels; no additional clean or black/white flash request. */
    d.evidence_options.quality_after_turn=true;
    unsigned clean_before=d.clean_refreshes;
    assert(display_arm_page_turn(&d,true,false,16));memset(d.pending,200,d.length);d.dirty=true;
    at=band_count;flush_now(&d);
    assert(band_count==at+16&&d.quality_refreshes==1&&last_wave==REFRESH_GLR16&&last_flags==0);
    assert(d.clean_refreshes==clean_before&&!memcmp(d.pending,d.committed,d.length));
    /* Rejection of the diagnostic update retains the completed primary image
     * and never waits for the rejected marker. */
    fail_normal_once=true;
    assert(display_arm_page_turn(&d,false,false,16));memset(d.pending,180,d.length);d.dirty=true;flush_now(&d);
    assert(d.quality_refreshes==1&&d.committed[0]==180&&display_wait(&d)==0);
    assert(display_arm_page_turn(&d,true,false,16));memset(d.pending,200,d.length);d.dirty=true;
    fail_complete_marker=d.marker+17; /* 16 successful bands, then quality fence fails. */
    d.last_refresh=d.last_present=d.first_dirty=0;display_flush(&d);
    assert(app_quit&&d.committed[0]==180&&d.quality_refreshes==1);
    assert(display_wait(&d)==0);app_quit=0;
    d.evidence_options.quality_after_turn=false;
    /* Cooperative cancellation after three accepted bands must fence all
     * accepted work, then restore a complete ordinary page with no seams. */
    d.turn_options.max_inflight=0;cancel_at_band=band_count+3;
    assert(display_arm_page_turn(&d,true,false,16));memset(d.pending,200,d.length);d.dirty=true;
    at=band_count;unsigned animations=d.animated_refreshes;flush_now(&d);
    assert(band_count==at+3&&d.animated_refreshes==animations&&!outstanding&&last_wave==REFRESH_GLR16);
    assert(!memcmp(d.pending,d.committed,d.length));
    fail_submission_at=submission_waits+2;
    assert(display_arm_page_turn(&d,false,false,16));memset(d.pending,180,d.length);d.dirty=true;
    flush_now(&d);assert(d.animated_refreshes==animations&&!outstanding);
    assert(!memcmp(d.pending,d.committed,d.length));
    /* Unknown rotation and real color must bypass the experiment. */
    d.evidence_options.quality_after_turn=true;
    d.v.rotate=0;assert(display_arm_page_turn(&d,true,false,16));
    memset(d.pending,200,d.length);d.dirty=true;at=band_count;flush_now(&d);assert(band_count==at);
    d.v.rotate=3;assert(display_arm_page_turn(&d,true,false,16));
    memset(d.pending,180,d.length);d.pending[0]=250;d.dirty=true;
    flush_now(&d);assert(band_count==at&&last_flags==REFRESH_CFA_G2);
    assert(d.quality_refreshes==1); /* No optional update on unknown geometry or CFA. */
    d.evidence_options.quality_after_turn=false;
    memset(d.pending,200,d.length);d.dirty=true;flush_now(&d); /* Erase old color safely. */
    /* Night, explicit clean and vertical geometry keep their original
     * waveforms/directions under the new timing policy. */
    assert(display_arm_page_turn(&d,true,false,16));
    memset(d.pending,0,d.length);display_full(&d);flush_now(&d);
    at=band_count;assert(last_wave==REFRESH_GCK16&&inverted);
    assert(display_arm_page_turn(&d,true,false,16));
    memset(d.pending,40,d.length);d.dirty=true;flush_now(&d);
    assert(band_count==at+16&&last_wave==REFRESH_GLKW16&&inverted);
    d.evidence_options.quality_after_turn=true;
    assert(display_arm_page_turn(&d,false,false,16));memset(d.pending,60,d.length);d.dirty=true;flush_now(&d);
    assert(d.quality_refreshes==2&&last_wave==REFRESH_GLKW16&&last_flags==0&&inverted);
    d.evidence_options.quality_after_turn=false;
    memset(d.pending,200,d.length);display_full(&d);flush_now(&d);
    assert(display_arm_page_turn(&d,true,true,16));
    memset(d.pending,180,d.length);d.dirty=true;at=band_count;flush_now(&d);
    assert(band_count==at+16&&!inverted);
    for(unsigned i=1;i<16;i++)assert(band_top[(at+i)%128]>band_top[(at+i+1)%128]);
    /* A rejected ordinary SEND must never become the next wait target.
     * Verify this fix also preserves stable-profile success behavior. */
    d.turn_options.profile=TURN_STABLE;
    memset(d.pending,200,d.length);d.dirty=true;fail_normal_once=true;
    d.last_refresh=d.last_present=d.first_dirty=0;display_flush(&d);
    assert(d.failures==1&&!outstanding&&d.committed[0]==180);
    assert(d.last_accepted_marker==d.completed_marker&&d.marker!=d.last_accepted_marker);
    assert(display_wait(&d)==0);
    flush_now(&d);assert(d.failures==0&&d.committed[0]==200);
    d.turn_options.profile=TURN_TIMED;
    /* A failed completion must not advance the logical committed image. */
    memset(d.pending,160,d.length);d.dirty=true;fail_complete_once=true;
    d.last_refresh=d.last_present=d.first_dirty=0;display_flush(&d);
    assert(app_quit&&d.committed[0]==200);
    assert(display_wait(&d)==0);app_quit=0; /* Mock a later kernel completion for cleanup. */
    assert(!outstanding&&!immutable_image);
    assert(!display_inversion(&d,false));
    free(d.pending);free(d.committed);free(d.mem);pthread_mutex_destroy(&d.lock);display=original;
    puts("PASS: off/timed profiles, repeated and reversed arms, queue cap, cancellation, submission failure, immutable framebuffer, unknown rotation/color fallback and failed-completion cache");
}
static void check_pixel_transfer(Display *original){
    /* Distinct input and framebuffer strides, nonzero offsets, every gray
     * level including lower-edge antialiasing. Padding is never a pixel. */
    Display d={.fd=99,.pending_stride=256,.length=288*66};display=&d;
    d.v.xres=d.v.yres=64;d.v.xoffset=2;d.v.yoffset=1;d.f.line_length=288;
    d.v.red=(struct fb_bitfield){0,8,0};d.v.green=(struct fb_bitfield){8,8,0};
    d.v.blue=(struct fb_bitfield){16,8,0};d.v.transp=(struct fb_bitfield){24,8,0};
    d.pending=malloc(256*64);d.committed=malloc(256*64);d.mem=malloc(d.length);
    assert(d.pending&&d.committed&&d.mem);memset(d.mem,0xa5,d.length);
    pthread_mutex_init(&d.lock,NULL);strcpy(d.mode,"balanced");
    for(unsigned night=0;night<2;night++){
        uint8_t src[272*64];memset(src,night?0:255,sizeof(src));
        for(unsigned y=8;y<12;y++)for(unsigned x=0;x<64;x++){
            uint8_t *p=src+y*272+x*4;p[0]=p[1]=p[2]=(uint8_t)((y-8)*64+x);p[3]=255;
        }
        assert(display_present(&d,src,272,64));display_full(&d);flush_now(&d);
        assert(inverted==!!night);
        for(unsigned y=0;y<64;y++)for(unsigned x=0;x<64;x++){
            const uint8_t *p=src+y*272+x*4;
            const uint8_t *q=d.mem+(y+1)*288+(x+2)*4;
            unsigned gray=night?255-p[0]:p[0];
            assert(q[0]==gray&&q[1]==gray&&q[2]==gray&&q[3]==255);
            assert(!memcmp(d.pending+y*256+x*4,p,4));
        }
        /* A full white/black background row stays a uniform background;
         * conversion cannot manufacture a gray line underneath it. */
        for(unsigned y=0;y<66;y++)for(unsigned x=0;x<288;x++){
            if(y<1||y>=65||x<8||x>=264)assert(d.mem[y*288+x]==0xa5);
        }
    }
    assert(!display_inversion(&d,false));
    free(d.pending);free(d.committed);free(d.mem);pthread_mutex_destroy(&d.lock);display=original;
    puts("PASS: all 256 antialias gray levels, padded input/FB strides, offsets, inversion round trip and untouched padding");
}
int main(void){
    char root[]="/tmp/reeden-display-test-XXXXXX";assert(mkdtemp(root));char path[512];
    snprintf(path,sizeof(path),"%s/state",root);assert(!mkdir(path,0700));assert(!setenv("REEDEN_KOBO_DIR",root,1));
    Display d={.fd=99,.pending_stride=256,.length=16384};display=&d;
    d.v.xres=d.v.yres=64;d.f.line_length=256;
    d.v.red=(struct fb_bitfield){0,8,0};d.v.green=(struct fb_bitfield){8,8,0};
    d.v.blue=(struct fb_bitfield){16,8,0};d.v.transp=(struct fb_bitfield){24,8,0};
    d.pending=malloc(d.length);d.committed=malloc(d.length);d.mem=calloc(1,d.length);assert(d.pending&&d.committed&&d.mem);
    pthread_mutex_init(&d.lock,NULL);strcpy(d.mode,"balanced");
    memset(d.pending,255,d.length);d.frames=1;d.dirty=d.full=true;flush_now(&d);
    assert(last_wave==REFRESH_GC16&&!inverted&&last_first_pixel==0xffffffff);
    memset(d.pending,0,d.length);display_full(&d);flush_now(&d);
    assert(last_wave==REFRESH_GCK16&&inverted&&last_first_pixel==0xffffffff);
    assert(d.committed[0]==0); /* Hardware encoding never poisons logical diff. */
    d.pending[32*256+32*4]=d.pending[32*256+32*4+1]=d.pending[32*256+32*4+2]=255;
    d.dirty=true;flush_now(&d);assert(last_wave==REFRESH_GLKW16&&last_flags==0&&inverted);
    /* A single colored glyph requires CFA and a safe global encoding switch. */
    unsigned cleans=d.clean_refreshes;
    d.pending[0]=200;d.dirty=true;flush_now(&d);
    assert(last_wave==REFRESH_GLRC16&&last_flags==REFRESH_CFA_G2&&!inverted&&last_first_pixel==0xffc80000);
    assert(d.clean_refreshes==cleans); /* Encoding changes are not clean requests. */
    d.pending[4]=180;d.dirty=true;flush_now(&d);
    assert(last_wave==REFRESH_GLRC16&&last_flags==REFRESH_CFA_G2&&!inverted);
    /* Erasing old color still uses CFA; subsequent gray pages retain the
     * session's direct encoding instead of toggling the global flag again. */
    d.pending[0]=d.pending[4]=0;d.dirty=true;flush_now(&d);
    assert(last_wave==REFRESH_GLRC16&&last_flags==REFRESH_CFA_G2&&!inverted);
    assert(d.clean_refreshes==cleans);
    d.pending[32*256+32*4]=d.pending[32*256+32*4+1]=d.pending[32*256+32*4+2]=0;
    d.dirty=true;flush_now(&d);assert(last_wave==REFRESH_GLKW16&&last_flags==0&&!inverted);
    snprintf(path,sizeof(path),"%s/cover.y8",root);FILE *cover=fopen(path,"wb");assert(cover);
    uint8_t white[4096];memset(white,255,sizeof(white));assert(fwrite(white,1,sizeof(white),cover)==sizeof(white));fclose(cover);
    unsigned before_cover=d.refreshes,confirmed=d.completed_marker;
    uint8_t old_pixel=d.committed[0];
    fail_normal_once=true;
    assert(display_cover(&d,path,64,64,1)<0);
    assert(!app_quit&&d.failures==1&&d.refreshes==before_cover&&d.completed_marker==confirmed);
    assert(d.committed[0]==old_pixel&&!outstanding);
    assert(!display_cover(&d,path,64,64,1));assert(!inverted&&last_wave==REFRESH_GC16);
    unlink(path);
    /* 1.43.2+ RGB (3-channel) and RGBA (4-channel transparent over committed page) sleep covers. */
    snprintf(path,sizeof(path),"%s/cover.rgb",root);cover=fopen(path,"wb");assert(cover);
    uint8_t rgb_cover[64*64*3];memset(rgb_cover,255,sizeof(rgb_cover));
    rgb_cover[0]=220;rgb_cover[1]=40;rgb_cover[2]=30;
    assert(fwrite(rgb_cover,1,sizeof(rgb_cover),cover)==sizeof(rgb_cover));fclose(cover);
    assert(!display_cover(&d,path,64,64,3));assert(!inverted&&last_wave==REFRESH_GCC16&&last_flags==REFRESH_CFA_G2);
    unlink(path);
    snprintf(path,sizeof(path),"%s/cover.rgba",root);cover=fopen(path,"wb");assert(cover);
    uint8_t rgba_cover[64*64*4];memset(rgba_cover,0,sizeof(rgba_cover)); /* fully transparent -> keeps committed */
    assert(fwrite(rgba_cover,1,sizeof(rgba_cover),cover)==sizeof(rgba_cover));fclose(cover);
    assert(!display_cover(&d,path,64,64,4));assert(d.committed[0]==30&&d.committed[1]==40&&d.committed[2]==220);
    unlink(path);snprintf(path,sizeof(path),"%s/state/night-invert",root);assert(access(path,F_OK)<0);
    check_page_turns(&d);
    check_localized_color(&d);
    check_experimental_turns(&d);
    check_pixel_transfer(&d);
    /* A failed global write must retain its recovery obligation and abort. */
    fail_inversion=true;d.direct_night=false;display_full(&d);flush_now(&d);assert(app_quit&&access(path,F_OK)==0);
    unlink(path);snprintf(path,sizeof(path),"%s/state",root);rmdir(path);rmdir(root);
    free(d.pending);free(d.committed);free(d.mem);pthread_mutex_destroy(&d.lock);
    assert(commands>0);
    puts("PASS: isolated display transactions, RGB packing, grayscale HW inversion, color/erase CFA, completion fences, cover reset and failed-inversion recovery marker; no hardware accessed");
    return 0;
}

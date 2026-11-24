#include "kobo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/input.h>
static bool (*pointer_filter)(FlutterPointerEvent *);
static void (*input_observer)(uint64_t);
void touch_set_observer(void (*observer)(uint64_t)){input_observer=observer;}
void touch_set_filter(bool (*filter)(FlutterPointerEvent *)){pointer_filter=filter;}
static void send_pointer(FlutterEngine e,Contact *c,FlutterPointerPhase phase,unsigned w,unsigned h,Touch *t){
    double nx=(double)(c->x-t->min_x)/(t->max_x-t->min_x),ny=(double)(c->y-t->min_y)/(t->max_y-t->min_y);
    double x=(1.0-ny)*(w-1),y=nx*(h-1);
    if(t->invert_x)x=w-1-x;
    if(t->invert_y)y=h-1-y;
    if(x<0)x=0;if(x>w-1)x=w-1;if(y<0)y=0;if(y>h-1)y=h-1;
    FlutterPointerEvent p={.struct_size=sizeof(p),.phase=phase,.timestamp=monotonic_ns()/1000,.x=x,.y=y,.device=c->id+1,.device_kind=kFlutterPointerDeviceKindTouch,.signal_kind=kFlutterPointerSignalKindNone};
    c->last_x=x;c->last_y=y;
    if(!pointer_filter||!pointer_filter(&p))FlutterEngineSendPointerEvent(e,&p,1);
}
int touch_open(Touch *t){
    memset(t,0,sizeof(*t));t->fd=-1;t->current.id=-1;
    const char *p=getenv("REEDEN_TOUCH");if(!p)p="/dev/input/event1";
    t->fd=open(p,O_RDONLY|O_NONBLOCK|O_CLOEXEC);if(t->fd<0)return -1;
    if(ioctl(t->fd,EVIOCGRAB,1)<0)goto fail;
    struct input_absinfo x={0},y={0},slot={0};
    if(ioctl(t->fd,EVIOCGABS(ABS_MT_POSITION_X),&x)||ioctl(t->fd,EVIOCGABS(ABS_MT_POSITION_Y),&y)||x.maximum<=x.minimum||y.maximum<=y.minimum)goto fail;
    unsigned long caps[(ABS_MAX+8*sizeof(long))/(8*sizeof(long))];memset(caps,0,sizeof(caps));ioctl(t->fd,EVIOCGBIT(EV_ABS,sizeof(caps)),caps);
    t->slots=(caps[ABS_MT_SLOT/(8*sizeof(long))]>>(ABS_MT_SLOT%(8*sizeof(long))))&1;
    if(t->slots)ioctl(t->fd,EVIOCGABS(ABS_MT_SLOT),&slot);
    t->min_x=x.minimum;t->max_x=x.maximum;t->min_y=y.minimum;t->max_y=y.maximum;
    t->invert_x=getenv("REEDEN_TOUCH_INVERT_X")!=NULL;
    t->invert_y=getenv("REEDEN_TOUCH_INVERT_Y")!=NULL;
    for(int i=0;i<16;i++)t->contacts[i].id=-1;
    fprintf(stderr,"[touch] %s protocol=%s X=%d..%d Y=%d..%d transform=swap+mirrorX\n",p,t->slots?"B":"A",t->min_x,t->max_x,t->min_y,t->max_y);
    return 0;
fail:
    {int saved=errno;close(t->fd);t->fd=-1;errno=saved;return -1;}
}
static void cancel_all(Touch *t,FlutterEngine e,unsigned w,unsigned h){for(int i=0;i<16;i++){Contact *c=&t->contacts[i];if(c->down){send_pointer(e,c,kCancel,w,h,t);send_pointer(e,c,kRemove,w,h,t);}memset(c,0,sizeof(*c));c->id=-1;}}
static void sync_contacts(Touch *t,FlutterEngine e,unsigned w,unsigned h){
    for(int i=0;i<16;i++)t->contacts[i].seen=false;
    for(unsigned j=0;j<t->report_count;j++){
        Contact *r=&t->report[j],*c=NULL;if(!r->active||r->id<0)continue;
        for(int i=0;i<16;i++)if(t->contacts[i].id==r->id){c=&t->contacts[i];break;}
        if(!c)for(int i=0;i<16;i++)if(t->contacts[i].id<0){c=&t->contacts[i];*c=*r;c->down=false;break;}
        if(!c)continue;c->x=r->x;c->y=r->y;c->seen=true;
        if(!c->down){send_pointer(e,c,kAdd,w,h,t);send_pointer(e,c,kDown,w,h,t);c->down=true;}else send_pointer(e,c,kMove,w,h,t);
    }
    for(int i=0;i<16;i++){Contact *c=&t->contacts[i];if(c->id>=0&&!c->seen){if(c->down){send_pointer(e,c,kUp,w,h,t);send_pointer(e,c,kRemove,w,h,t);}memset(c,0,sizeof(*c));c->id=-1;}}
}
void touch_process(Touch *t,FlutterEngine engine,unsigned w,unsigned h){
    struct input_event events[64];ssize_t n;
    while((n=read(t->fd,events,sizeof(events)))>0){
      if(input_observer)input_observer(monotonic_ns());
      for(size_t i=0;i<(size_t)n/sizeof(*events);i++){
        struct input_event ev=events[i];
        if(ev.type==EV_SYN&&ev.code==SYN_DROPPED){cancel_all(t,engine,w,h);memset(t->report,0,sizeof(t->report));t->dropped=true;continue;}
        if(t->dropped){if(ev.type==EV_SYN&&ev.code==SYN_REPORT){t->dropped=false;t->report_count=0;t->current=(Contact){.id=-1};}continue;}
        if(t->slots){
            if(ev.type==EV_ABS){
                if(ev.code==ABS_MT_SLOT){t->slot=ev.value;continue;}if(t->slot<0||t->slot>=16)continue;
                Contact *c=&t->report[t->slot];
                if(ev.code==ABS_MT_TRACKING_ID){c->id=ev.value;c->active=ev.value>=0;}
                else if(ev.code==ABS_MT_POSITION_X)c->x=ev.value;else if(ev.code==ABS_MT_POSITION_Y)c->y=ev.value;
            }else if(ev.type==EV_SYN&&ev.code==SYN_REPORT){t->report_count=16;sync_contacts(t,engine,w,h);}
        }else{
            if(ev.type==EV_ABS){if(ev.code==ABS_MT_TRACKING_ID){t->current.id=ev.value;t->current.active=ev.value>=0;}else if(ev.code==ABS_MT_POSITION_X)t->current.x=ev.value;else if(ev.code==ABS_MT_POSITION_Y)t->current.y=ev.value;}
            else if(ev.type==EV_SYN&&ev.code==SYN_MT_REPORT){if(t->current.active&&t->report_count<16)t->report[t->report_count++]=t->current;t->current=(Contact){.id=-1};}
            else if(ev.type==EV_SYN&&ev.code==SYN_REPORT){if(t->current.active&&t->report_count<16)t->report[t->report_count++]=t->current;sync_contacts(t,engine,w,h);t->report_count=0;t->current=(Contact){.id=-1};}
        }
      }
    }
}
void touch_close(Touch *t,FlutterEngine e){
    if(e)cancel_all(t,e,1072,1448);
    int fd=t->fd;t->fd=-1;
    if(fd>=0)close(fd);
}

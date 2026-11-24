#include "turn_timing.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool number(const char *key,unsigned low,unsigned high,unsigned *out){
    const char *p=getenv(key);if(!p)return true;
    if(!*p)return false;
    for(const char *q=p;*q;q++)if(*q<'0'||*q>'9')return false;
    errno=0;char *end=NULL;unsigned long value=strtoul(p,&end,10);
    if(errno||!end||*end||value<low||value>high)return false;
    *out=(unsigned)value;return true;
}
bool turn_options_load(TurnOptions *out){
    TurnOptions o={.profile=TURN_STABLE,.bands=16,.span_us=150000,.min_gap_us=2000};
    const char *p=getenv("REEDEN_TURN_PROFILE");
    if(p&&strcmp(p,"stable")){
        if(!strcmp(p,"off"))o.profile=TURN_OFF;
        else if(!strcmp(p,"timed"))o.profile=TURN_TIMED;
        else goto invalid;
    }
    unsigned trace=0;
    if(!number("REEDEN_TURN_TRACE",0,1,&trace))goto invalid;
    o.trace=trace;
    if(o.profile==TURN_TIMED&&
       (!number("REEDEN_TURN_BANDS",4,32,&o.bands)||
        !number("REEDEN_TURN_SPAN_US",40000,350000,&o.span_us)||
        !number("REEDEN_TURN_MIN_GAP_US",1000,20000,&o.min_gap_us)||
        !number("REEDEN_TURN_MAX_INFLIGHT",0,8,&o.max_inflight)))goto invalid;
    /* A legal range can still describe an impossible schedule. */
    if(o.profile==TURN_TIMED&&o.min_gap_us*(o.bands-1)>o.span_us)goto invalid;
    *out=o;return true;
invalid:
    *out=(TurnOptions){.profile=TURN_STABLE};return false;
}
bool color_options_load(ColorRefreshProfile *out){
    const char *p=getenv("REEDEN_COLOR_REFRESH");
    if(!p||!strcmp(p,"smooth")){*out=COLOR_REFRESH_SMOOTH;return true;}
    if(!strcmp(p,"legacy")){*out=COLOR_REFRESH_LEGACY;return true;}
    *out=COLOR_REFRESH_SMOOTH;return false;
}
static uint64_t plus(uint64_t a,uint64_t b){return UINT64_MAX-a<b?UINT64_MAX:a+b;}
uint64_t turn_due_ns(const TurnOptions *o,uint64_t start,uint64_t last,
                     unsigned pixels,unsigned span_pixels){
    if(!span_pixels||pixels>span_pixels)return UINT64_MAX;
    uint64_t offset=(uint64_t)o->span_us*1000*pixels/span_pixels;
    uint64_t due=plus(start,offset);
    if(last){uint64_t minimum=plus(last,(uint64_t)o->min_gap_us*1000);if(due<minimum)due=minimum;}
    return due;
}
void turn_trace_init(TurnTrace *t,bool enabled,uint64_t id){
    t->enabled=enabled;
    if(enabled){memset(t,0,sizeof(*t));t->enabled=true;t->id=id;}
}
void turn_trace_add(TurnTrace *t,const char *op,uint64_t begin,uint64_t end,RefreshRegion region,
                    uint32_t marker,uint32_t wave,uint32_t mode,uint32_t flags,int result,int error){
    if(!t||!t->enabled||t->count==TURN_TRACE_MAX)return;
    t->event[t->count++]=(TurnEvent){op,begin,end,region,marker,wave,mode,flags,result,error};
}
void turn_trace_dump(const TurnTrace *t,const char *outcome,uint64_t end){
    if(!t||!t->enabled)return;
    for(unsigned i=0;i<t->count;i++){
        const TurnEvent *e=&t->event[i];
        fprintf(stderr,"[turn-trace] {\"id\":%llu,\"op\":\"%s\",\"begin_ns\":%llu,\"end_ns\":%llu,\"marker\":%u,\"region\":[%u,%u,%u,%u],\"wave\":%u,\"mode\":%u,\"flags\":%u,\"rc\":%d,\"errno\":%d}\n",
            (unsigned long long)t->id,e->op,(unsigned long long)e->begin,(unsigned long long)e->end,e->marker,
            e->region.left,e->region.top,e->region.width,e->region.height,e->wave,e->mode,e->flags,e->result,e->error);
    }
    const TurnOptions *o=&t->options;
    const char *profile=o->profile==TURN_TIMED?"timed":o->profile==TURN_OFF?"off":"stable";
    fprintf(stderr,"[turn-trace] {\"id\":%llu,\"op\":\"summary\",\"outcome\":\"%s\",\"fallback\":\"%s\",\"profile\":\"%s\",\"requested_bands\":%u,\"span_us\":%u,\"min_gap_us\":%u,\"max_inflight\":%u,\"input_read_ns\":%llu,\"arm_ns\":%llu,\"first_frame_ns\":%llu,\"last_frame_ns\":%llu,\"flush_ns\":%llu,\"planned_ns\":%llu,\"copy_begin_ns\":%llu,\"copied_ns\":%llu,\"complete_ns\":%llu,\"events\":%u}\n",
        (unsigned long long)t->id,outcome,t->fallback?t->fallback:"none",profile,o->profile==TURN_TIMED?o->bands:0,o->span_us,o->min_gap_us,o->max_inflight,
        (unsigned long long)t->input,(unsigned long long)t->arm,
        (unsigned long long)t->first_frame,(unsigned long long)t->last_frame,(unsigned long long)t->flush,
        (unsigned long long)t->planned,(unsigned long long)t->copy_begin,(unsigned long long)t->copied,(unsigned long long)end,t->count);
}

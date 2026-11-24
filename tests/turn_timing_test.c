#include "turn_timing.h"
#undef NDEBUG
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

static const char *keys[]={"REEDEN_TURN_PROFILE","REEDEN_TURN_TRACE","REEDEN_TURN_BANDS",
    "REEDEN_TURN_SPAN_US","REEDEN_TURN_MIN_GAP_US","REEDEN_TURN_MAX_INFLIGHT"};
static void clear(void){for(unsigned i=0;i<sizeof(keys)/sizeof(*keys);i++)unsetenv(keys[i]);}
int main(void){
    TurnOptions o;clear();assert(turn_options_load(&o)&&o.profile==TURN_STABLE&&!o.trace);
    setenv(keys[0],"off",1);assert(turn_options_load(&o)&&o.profile==TURN_OFF);
    setenv(keys[0],"timed",1);assert(turn_options_load(&o)&&o.profile==TURN_TIMED&&o.bands==16);
    const char *bad[]={"","-1","+16","16x"," 16","9999999999999999999999999999999"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++){
        setenv(keys[2],bad[i],1);assert(!turn_options_load(&o)&&o.profile==TURN_STABLE&&!o.trace);
    }
    clear();setenv(keys[0],"timed",1);setenv(keys[2],"32",1);setenv(keys[4],"20000",1);
    assert(!turn_options_load(&o)); /* Individually valid, jointly impossible. */
    clear();setenv(keys[0],"timed",1);setenv(keys[5],"4",1);
    assert(turn_options_load(&o)&&o.max_inflight==4);
    o=(TurnOptions){.profile=TURN_TIMED,.span_us=150000,.min_gap_us=2000};
    const uint64_t start=1000000000ULL;
    /* Distances 64 and 80 must consume proportionate time, rather than
     * giving unequal-width rectangles the same nominal velocity. */
    assert(turn_due_ns(&o,start,0,0,1008)==start);
    uint64_t a=turn_due_ns(&o,start,start,64,1008);
    uint64_t b=turn_due_ns(&o,start,a,144,1008);
    assert(b-a>a-start);
    assert(turn_due_ns(&o,start,b,1008,1008)==start+150000000ULL);
    /* A delayed driver never causes a rapid burst to catch an old deadline. */
    assert(turn_due_ns(&o,start,start+200000000ULL,64,1008)==start+202000000ULL);
    assert(turn_due_ns(&o,start,0,1009,1008)==UINT64_MAX);
    assert(turn_due_ns(&o,UINT64_MAX-10,0,1008,1008)==UINT64_MAX);
    /* All supported geometries have monotone deadlines and exact coverage. */
    for(unsigned bands=4;bands<=32;bands++)for(unsigned direction=0;direction<2;direction++){
        RefreshRegion last;assert(refresh_band(1072,1448,bands,bands-1,direction,&last));
        unsigned total=direction?1072-last.left-last.width:last.left;
        uint64_t previous=0;
        for(unsigned i=0;i<bands;i++){
            RefreshRegion r;assert(refresh_band(1072,1448,bands,i,direction,&r));
            unsigned advance=direction?1072-r.left-r.width:r.left;
            uint64_t due=turn_due_ns(&o,start,previous,advance,total);
            assert(!i||due>=previous+2000000ULL);previous=due;
        }
        assert(previous==start+150000000ULL);
    }
    ColorRefreshProfile cp;
    unsetenv("REEDEN_COLOR_REFRESH");assert(color_options_load(&cp)&&cp==COLOR_REFRESH_SMOOTH);
    setenv("REEDEN_COLOR_REFRESH","smooth",1);assert(color_options_load(&cp)&&cp==COLOR_REFRESH_SMOOTH);
    setenv("REEDEN_COLOR_REFRESH","legacy",1);assert(color_options_load(&cp)&&cp==COLOR_REFRESH_LEGACY);
    setenv("REEDEN_COLOR_REFRESH","bogus",1);assert(!color_options_load(&cp)&&cp==COLOR_REFRESH_SMOOTH);
    unsetenv("REEDEN_COLOR_REFRESH");
    clear();puts("PASS: stable default, invalid config fallback, spatial timing, no catch-up bursts, overflow and both directions");
}

#pragma once
#include "refresh_policy.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum { TURN_STABLE, TURN_OFF, TURN_TIMED } TurnProfile;
typedef struct {
    TurnProfile profile;
    unsigned bands,span_us,min_gap_us,max_inflight;
    bool trace;
} TurnOptions;

/* Invalid explicit settings fail closed to the unchanged stable profile. */
bool turn_options_load(TurnOptions *out);

typedef enum { COLOR_REFRESH_SMOOTH, COLOR_REFRESH_LEGACY } ColorRefreshProfile;
/* Invalid explicit settings fail closed to the smooth default profile. */
bool color_options_load(ColorRefreshProfile *out);
/* Uniform spatial progression, with a minimum actual gap to avoid catch-up
 * bursts. span_pixels is the distance between first and final band starts. */
uint64_t turn_due_ns(const TurnOptions *,uint64_t start,uint64_t last,
                     unsigned pixels,unsigned span_pixels);

enum { TURN_TRACE_MAX=104 };
typedef struct {
    const char *op;
    uint64_t begin,end;
    RefreshRegion region;
    uint32_t marker,wave,mode,flags;
    int result,error;
} TurnEvent;
typedef struct {
    bool enabled;
    uint64_t id,input,arm,first_frame,last_frame,flush,planned,copy_begin,copied;
    const char *fallback;
    TurnOptions options;
    unsigned count;
    TurnEvent event[TURN_TRACE_MAX];
} TurnTrace;
void turn_trace_init(TurnTrace *,bool,uint64_t id);
void turn_trace_add(TurnTrace *,const char *,uint64_t,uint64_t,RefreshRegion,
                    uint32_t marker,uint32_t wave,uint32_t mode,uint32_t flags,
                    int result,int error);
void turn_trace_dump(const TurnTrace *,const char *outcome,uint64_t end);

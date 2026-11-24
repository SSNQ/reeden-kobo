#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct { unsigned left,top,width,height; } RefreshRegion;
enum { REFRESH_BANDS_MAX=32 };
typedef struct {
    RefreshRegion region;
    uint32_t waveform,flags;
    unsigned changed;
    bool clean,major,color,night,polarity_changed,hw_invert,frame_color,frame_night;
} RefreshPlan;

enum { REFRESH_GC16=2, REFRESH_GL16=3, REFRESH_GLR16=4, REFRESH_GCK16=8,
       REFRESH_GLKW16=9, REFRESH_GCC16=10, REFRESH_GLRC16=11,
       REFRESH_CFA_NATIVE=0, REFRESH_CFA_G2=0x600 };

/* Raw IDs, NOT libkobo names. Despite NickelDissolve ABOUT.md calling raw 8
 * daytime glkw16, this Clara Colour's live waveform_modes maps 8 -> gck16
 * and 9 -> glkw16; Kobo's published HWTCON source agrees. Use neutral band
 * aliases to preserve baseline behavior without assuming pulse semantics. */
enum { HWTCON_RAW_DAY_BAND=8, HWTCON_RAW_NIGHT_BAND=9 };

/* The old image is a RAM copy of the last successfully submitted BGRA frame.
 * A NULL old image forces initialization. Alpha changes alone are invisible. */
bool refresh_plan(const uint8_t *old,const uint8_t *next,size_t stride,
                  unsigned width,unsigned height,bool requested_clean,
                  const char *mode,RefreshPlan *out);
/* Reeden owns auto/manual cleanup cadence. The backend only coalesces a
 * nearby cleanup with pixels waiting to be submitted. Zero means no request. */
uint64_t refresh_deadline(uint64_t pixels_due,uint64_t clean_due,
                          uint64_t next_allowed,uint64_t now);
bool refresh_duplicate_clean(uint64_t now,uint64_t last_clean,bool same_pixels);
/* Non-overlapping, aligned bands; forward reveals right to left. */
bool refresh_band(unsigned width,unsigned height,unsigned steps,unsigned index,
                  bool forward,RefreshRegion *out);
RefreshRegion refresh_align_region(RefreshRegion r,unsigned width,unsigned height,
                                   unsigned align);
enum { REFRESH_PARTS_MAX=17 };
typedef struct {RefreshRegion region;bool color;} RefreshPart;
/* Sparse colored text on a dark page: keep CFA confined to aligned row
 * groups. Dense color/photos return zero and retain the normal CFA path. */
unsigned refresh_night_parts(const uint8_t *old,const uint8_t *next,size_t stride,
                             unsigned width,unsigned height,RefreshPart *out);

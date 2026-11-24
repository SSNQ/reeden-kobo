#include "refresh_policy.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WIDTH=1072, HEIGHT=1448, STRIDE=WIDTH*4 };
static void pixel(uint8_t *p,unsigned x,unsigned y,unsigned b,unsigned g,unsigned r){
    p+=y*STRIDE+x*4;p[0]=b;p[1]=g;p[2]=r;p[3]=255;
}
static void contains(const RefreshPlan *p,unsigned x,unsigned y){
    assert(p->region.left<=x&&p->region.top<=y);
    assert(p->region.left+p->region.width>x&&p->region.top+p->region.height>y);
    assert(p->region.left+p->region.width<=WIDTH&&p->region.top+p->region.height<=HEIGHT);
    assert(p->region.left%16==0&&p->region.top%16==0);
}
int main(void){
    size_t bytes=(size_t)STRIDE*HEIGHT;
    uint8_t *old=malloc(bytes),*next=malloc(bytes);assert(old&&next);
    memset(old,255,bytes);memcpy(next,old,bytes);RefreshPlan p;
    assert(!refresh_plan(old,next,STRIDE,WIDTH,HEIGHT,false,"balanced",&p));
    assert(refresh_plan(NULL,next,STRIDE,WIDTH,HEIGHT,false,"balanced",&p));
    assert(p.clean&&p.waveform==2&&!p.color&&p.region.width==WIDTH&&p.region.height==HEIGHT);
    /* A one-pixel change on every edge/boundary must remain covered. */
    unsigned xs[]={0,1,15,16,17,WIDTH-2,WIDTH-1},ys[]={0,1,15,16,17,HEIGHT-2,HEIGHT-1};
    for(unsigned i=0;i<sizeof(xs)/sizeof(*xs);i++)for(unsigned j=0;j<sizeof(ys)/sizeof(*ys);j++){
        memcpy(next,old,bytes);pixel(next,xs[i],ys[j],0,0,0);
        assert(refresh_plan(old,next,STRIDE,WIDTH,HEIGHT,false,"balanced",&p));
        assert(!p.clean&&!p.major&&!p.color&&p.waveform==4&&p.flags==0&&p.changed==1);
        contains(&p,xs[i],ys[j]);
    }
    /* A stale colored pixel being erased still needs the color pipeline. */
    memset(old,255,bytes);pixel(old,100,100,255,0,0);memset(next,255,bytes);
    assert(refresh_plan(old,next,STRIDE,WIDTH,HEIGHT,false,"balanced",&p));
    assert(p.color&&p.waveform==11&&p.flags==0x600);contains(&p,100,100);
    memset(old,255,bytes);pixel(next,100,100,255,0,0);
    assert(refresh_plan(old,next,STRIDE,WIDTH,HEIGHT,true,"balanced",&p));
    assert(p.color&&p.clean&&p.waveform==10);
    /* Backend must not add a second cleanup cadence over app scheduling. */
    memcpy(next,old,bytes);
    for(unsigned y=30;y<HEIGHT-30;y+=8)for(unsigned x=30;x<WIDTH-30;x+=8)pixel(next,x,y,0,0,0);
    assert(refresh_plan(old,next,STRIDE,WIDTH,HEIGHT,false,"balanced",&p));
    assert(p.major&&!p.clean&&p.waveform==4);
    assert(refresh_plan(old,next,STRIDE,WIDTH,HEIGHT,false,"balanced",&p));
    assert(p.major&&!p.clean&&p.waveform==4);
    /* Explicit clean and HD remain available, even for unchanged pixels. */
    assert(refresh_plan(old,old,STRIDE,WIDTH,HEIGHT,true,"balanced",&p)&&p.clean);
    assert(refresh_plan(old,old,STRIDE,WIDTH,HEIGHT,false,"hd",&p)&&p.clean);
    memcpy(next,old,bytes);next[3]=0;
    assert(!refresh_plan(old,next,STRIDE,WIDTH,HEIGHT,false,"balanced",&p));
    assert(!refresh_plan(old,next,1,WIDTH,HEIGHT,false,"balanced",&p));
    /* Pure black/gray backgrounds need Eclipse, not daytime waveforms. */
    memset(old,0,bytes);memcpy(next,old,bytes);pixel(next,400,400,255,255,255);
    assert(refresh_plan(old,next,STRIDE,WIDTH,HEIGHT,false,"balanced",&p));
    assert(p.night&&!p.color&&!p.clean&&p.waveform==9&&p.flags==0&&p.hw_invert);
    assert(refresh_plan(old,next,STRIDE,WIDTH,HEIGHT,true,"balanced",&p));
    assert(p.night&&p.clean&&p.waveform==8);
    memset(old,40,bytes);memcpy(next,old,bytes);pixel(next,400,400,200,200,200);
    assert(refresh_plan(old,next,STRIDE,WIDTH,HEIGHT,false,"balanced",&p)&&p.night&&p.waveform==9);
    /* Theme polarity changes clean once, using the destination's waveform. */
    memset(old,255,bytes);memset(next,0,bytes);
    assert(refresh_plan(old,next,STRIDE,WIDTH,HEIGHT,false,"balanced",&p));
    assert(p.clean&&p.polarity_changed&&p.night&&p.waveform==8);
    assert(refresh_plan(next,old,STRIDE,WIDTH,HEIGHT,false,"balanced",&p));
    assert(p.clean&&p.polarity_changed&&!p.night&&p.waveform==2);
    /* A white popup should not use a night regional waveform. */
    memset(old,0,bytes);memcpy(next,old,bytes);
    for(unsigned y=300;y<540;y++)for(unsigned x=200;x<520;x++)pixel(next,x,y,255,255,255);
    assert(refresh_plan(old,next,STRIDE,WIDTH,HEIGHT,false,"balanced",&p));
    assert(!p.clean&&!p.night&&p.waveform==4);
    /* Real color retains Kaleido, including in a dark frame. */
    memset(old,0,bytes);memcpy(next,old,bytes);pixel(next,400,400,200,0,0);
    assert(refresh_plan(old,next,STRIDE,WIDTH,HEIGHT,false,"balanced",&p));
    assert(p.color&&p.waveform==11&&p.flags==0x600);
    assert(refresh_plan(old,next,STRIDE,WIDTH,HEIGHT,true,"balanced",&p)&&p.waveform==10);
    assert(!p.hw_invert);
    /* 16-level grayscale is not binary, and tiny RGB rounding is not color. */
    memset(old,255,bytes);memcpy(next,old,bytes);pixel(next,400,400,126,127,128);
    assert(refresh_plan(old,next,STRIDE,WIDTH,HEIGHT,false,"balanced",&p));
    assert(!p.color&&p.waveform==REFRESH_GLR16);
    pixel(next,400,400,126,127,129);
    assert(refresh_plan(old,next,STRIDE,WIDTH,HEIGHT,false,"balanced",&p)&&p.color);
    /* Color outside a gray dirty region still prevents global HW inversion. */
    memset(old,0,bytes);pixel(old,10,10,200,0,0);memcpy(next,old,bytes);pixel(next,400,400,255,255,255);
    assert(refresh_plan(old,next,STRIDE,WIDTH,HEIGHT,false,"balanced",&p));
    assert(!p.color&&!p.hw_invert&&p.night&&p.frame_color);
    /* Animation bands cover each pixel exactly once in either direction. */
    for(unsigned w=160;w<=1080;w+=8)for(unsigned steps=2;steps<=REFRESH_BANDS_MAX&&steps<=w/16;steps++){
        unsigned coverage[1080]={0},previous=w;
        for(unsigned i=0;i<steps;i++){
            RefreshRegion band;
            assert(refresh_band(w,1448,steps,i,true,&band));
            assert(band.left%16==0&&band.width>0&&band.left+band.width==previous);
            previous=band.left;
            for(unsigned x=band.left;x<band.left+band.width;x++)coverage[x]++;
        }
        assert(previous==0);
        for(unsigned x=0;x<w;x++)assert(coverage[x]==1);
        unsigned left=0;
        for(unsigned i=0;i<steps;i++){
            RefreshRegion band;assert(refresh_band(w,1448,steps,i,false,&band));
            assert(band.left==left);left+=band.width;
        }
        assert(left==w);
    }
    RefreshRegion band;
    assert(!refresh_band(64,64,10,0,true,&band));
    assert(!refresh_band(1072,1448,10,10,true,&band));
    /* Sparse current and erased colored words get localized CFA row groups;
     * every pixel belongs to exactly one disjoint region with a stable CFA phase. */
    memset(old,0,bytes);memset(next,0,bytes);
    pixel(next,400,400,200,0,0);pixel(old,300,900,0,200,0);
    RefreshPart parts[REFRESH_PARTS_MAX];
    unsigned n=refresh_night_parts(old,next,STRIDE,WIDTH,HEIGHT,parts);
    assert(n>1);unsigned yend=0,color_rows=0;
    bool new_color=false,old_color=false;
    for(unsigned i=0;i<n;i++){
        RefreshRegion r=parts[i].region;
        assert(r.left==0&&r.width==WIDTH&&r.top==yend&&r.top%48==0&&r.height>0);
        yend=r.top+r.height;
        if(parts[i].color){
            color_rows+=r.height;
            if(r.top<=400&&yend>400)new_color=true;
            if(r.top<=900&&yend>900)old_color=true;
        }
    }
    assert(yend==HEIGHT&&color_rows<HEIGHT/2&&new_color&&old_color);
    for(unsigned y=0;y<HEIGHT;y++)for(unsigned x=0;x<WIDTH;x++)pixel(next,x,y,200,0,0);
    assert(!refresh_night_parts(old,next,STRIDE,WIDTH,HEIGHT,parts));
    /* Merge nearby cleanup with pixels, while honoring long delays. */
    const uint64_t ms=1000000ULL;
    assert(refresh_deadline(40*ms,80*ms,0,0)==80*ms);
    assert(refresh_deadline(120*ms,80*ms,0,0)==120*ms);
    assert(refresh_deadline(40*ms,1000*ms,0,0)==40*ms);
    assert(refresh_deadline(UINT64_MAX,80*ms,140*ms,0)==140*ms);
    assert(refresh_deadline(UINT64_MAX,0,0,0)==UINT64_MAX);
    assert(refresh_deadline(40*ms,0,0,0)==40*ms);
    assert(refresh_duplicate_clean(500*ms,100*ms,true));
    assert(!refresh_duplicate_clean(700*ms,100*ms,true));
    assert(!refresh_duplicate_clean(500*ms,100*ms,false));
    assert(!refresh_duplicate_clean(500*ms,0,true));
    RefreshRegion ar=refresh_align_region((RefreshRegion){50,100,200,300},1072,1448,48);
    assert(ar.left==48&&ar.top==96&&ar.left+ar.width>=250&&ar.top+ar.height>=400);
    assert(ar.width%48==0&&ar.height%48==0);
    RefreshRegion edge=refresh_align_region((RefreshRegion){1000,1400,72,48},1072,1448,48);
    assert(edge.left%48==0&&edge.top%48==0&&edge.left+edge.width==1072&&edge.top+edge.height==1448);
    /* Single-pass diff/color edge cases: unchanged color inside halo, unchanged
     * color outside dirty box, erased color, and unchanged B&W early return. */
    memset(old,255,bytes);pixel(old,17,17,200,0,0);memcpy(next,old,bytes);pixel(next,18,18,0,0,0);
    assert(refresh_plan(old,next,STRIDE,WIDTH,HEIGHT,false,"balanced",&p));
    assert(p.color&&p.frame_color&&p.changed==1);
    memset(old,255,bytes);pixel(old,10,10,200,0,0);memcpy(next,old,bytes);pixel(next,400,400,0,0,0);
    assert(refresh_plan(old,next,STRIDE,WIDTH,HEIGHT,false,"balanced",&p));
    assert(!p.color&&p.frame_color&&p.changed==1);
    memset(old,255,bytes);pixel(old,400,400,200,0,0);memset(next,255,bytes);
    assert(refresh_plan(old,next,STRIDE,WIDTH,HEIGHT,false,"balanced",&p));
    assert(p.color&&!p.frame_color&&p.changed==1);
    assert(refresh_plan(NULL,old,STRIDE,WIDTH,HEIGHT,false,"balanced",&p));
    assert(p.color&&p.frame_color&&p.clean);
    memset(old,255,bytes);memcpy(next,old,bytes);
    assert(!refresh_plan(old,next,STRIDE,WIDTH,HEIGHT,false,"balanced",&p));
    free(old);free(next);
    puts("PASS: day/night/gray/color, polarity changes and white popups, 49 edge regions, frontend cleanup ownership, 48px alignment and timing coalescence");
    return 0;
}

#include "frame_evidence.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

extern uint64_t monotonic_ns(void);
bool frame_evidence_options_load(FrameEvidenceOptions *out){
    FrameEvidenceOptions o={.limit=2,.reagl_bands=true};
    const char *dir=getenv("REEDEN_FRAME_EVIDENCE_DIR");
    const char *limit=getenv("REEDEN_FRAME_EVIDENCE_LIMIT");
    const char *quality=getenv("REEDEN_TEXT_QUALITY_AFTER_TURN");
    const char *band=getenv("REEDEN_TEXT_BAND_PATH");
    if(dir){
        if(dir[0]!='/'||strlen(dir)>=sizeof(o.directory))goto invalid;
        strcpy(o.directory,dir);
    }
    if(limit){
        if(strlen(limit)!=1||*limit<'1'||*limit>'4')goto invalid;
        o.limit=(unsigned)(*limit-'0');
    }
    if(quality){
        if(strcmp(quality,"0")&&strcmp(quality,"1"))goto invalid;
        o.quality_after_turn=!strcmp(quality,"1");
    }
    if(band){
        if(strcmp(band,"stable")&&strcmp(band,"reagl"))goto invalid;
        o.reagl_bands=!strcmp(band,"reagl");
    }
    *out=o;return true;
invalid:
    memset(out,0,sizeof(*out));out->reagl_bands=true;return false;
}
/* Exclusive files and a new per-transaction directory prevent overwrites and
 * reject terminal symlinks. Parent directory is explicitly chosen by operator. */
static FILE *new_file(FrameCapture *c,const char *name){
    int fd=openat(c->fd,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0){c->ok=false;return NULL;}
    FILE *f=fdopen(fd,"wb");if(!f){close(fd);c->ok=false;}return f;
}
void frame_capture_pixels(FrameCapture *c,const char *name,const uint8_t *p,size_t n){
    if(!c->active||!c->ok)return;
    FILE *f=new_file(c,name);if(!f)return;
    bool ok=fwrite(p,1,n,f)==n;if(fclose(f))ok=false;
    if(!ok)c->ok=false;
}
FrameCapture frame_capture_begin(FrameEvidenceOptions *o,const FrameEvidenceInfo *i,
                                const uint8_t *old,const uint8_t *target,const uint8_t *fb,size_t bytes){
    FrameCapture c={.fd=-1};
    if(!o->directory[0]||o->used>=o->limit)return c;
    unsigned serial=++o->used;char name[64];
    snprintf(name,sizeof(name),"frame-%ld-%u",(long)getpid(),serial);
    int parent=open(o->directory,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(parent<0)goto failed;
    if(mkdirat(parent,name,0700)){close(parent);goto failed;}
    c.fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);close(parent);
    if(c.fd<0)goto failed;
    c.active=c.ok=true;c.begin_ns=monotonic_ns();
    FILE *f=new_file(&c,"info.json");
    if(f){
        int rc=fprintf(f,"{\"schema\":1,\"width\":%u,\"height\":%u,\"logical_stride\":%u,\"rotation\":%u,"
          "\"fb_stride\":%u,\"xoffset\":%u,\"yoffset\":%u,\"fb_bytes\":%zu,"
          "\"red\":[%u,%u],\"green\":[%u,%u],\"blue\":[%u,%u],\"alpha\":[%u,%u],"
          "\"old_valid\":%s,\"hw_inverted\":%s,\"profile\":\"%s\",\"band_path\":\"%s\",\"animated\":%s,"
          "\"clean\":%s,\"color\":%s,\"region\":[%u,%u,%u,%u],\"ordinary_wave\":%u,\"ordinary_flags\":%u,"
          "\"trace_id\":%llu,\"capture_begin_ns\":%llu}\n",
          i->width,i->height,i->logical_stride,i->rotation,i->fb_stride,i->xoffset,i->yoffset,bytes,
          i->red.offset,i->red.length,i->green.offset,i->green.length,i->blue.offset,i->blue.length,i->alpha.offset,i->alpha.length,
          i->old_valid?"true":"false",i->plan.hw_invert?"true":"false",i->profile,i->band_path?i->band_path:"stable",i->animated?"true":"false",
          i->plan.clean?"true":"false",i->plan.color?"true":"false",i->plan.region.left,i->plan.region.top,
          i->plan.region.width,i->plan.region.height,i->plan.waveform,i->plan.flags,
          (unsigned long long)i->trace_id,(unsigned long long)c.begin_ns);
        bool ok=rc>0;if(fclose(f))ok=false;if(!ok)c.ok=false;
    }
    size_t logical=(size_t)i->logical_stride*i->height;
    if(i->old_valid)frame_capture_pixels(&c,"old.bgra",old,logical);
    frame_capture_pixels(&c,"target.bgra",target,logical);
    frame_capture_pixels(&c,"fb-before.bin",fb,bytes);
    return c;
failed:
    fprintf(stderr,"[frame-evidence] capture unavailable: %s\n",strerror(errno));return c;
}
void frame_capture_end(FrameCapture *c,const char *outcome,uint32_t marker,unsigned quality){
    if(!c->active)return;
    bool pixels_ok=c->ok;
    FILE *f=new_file(c,"result.json");
    if(f){
        int rc=fprintf(f,"{\"outcome\":\"%s\",\"pixels_complete\":%s,\"last_marker\":%u,"
          "\"quality_status\":%u,\"end_ns\":%llu}\n",outcome,pixels_ok?"true":"false",marker,quality,
          (unsigned long long)monotonic_ns());
        bool ok=rc>0;if(fclose(f))ok=false;if(!ok)c->ok=false;
    }
    fprintf(stderr,"[frame-evidence] outcome=%s capture_ok=%u diagnostic I/O included; not a latency benchmark\n",outcome,c->ok);
    close(c->fd);c->fd=-1;c->active=false;
}

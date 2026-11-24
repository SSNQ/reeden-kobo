#define _GNU_SOURCE
#include "frame_evidence.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

uint64_t monotonic_ns(void){static uint64_t t;return ++t;}
static void exact_file(const char *root,const char *name,const uint8_t *expected,size_t n){
    char path[1200];snprintf(path,sizeof(path),"%s/frame-%ld-1/%s",root,(long)getpid(),name);
    FILE *f=fopen(path,"rb");assert(f);uint8_t bytes[128];assert(n<=sizeof(bytes));
    assert(fread(bytes,1,n,f)==n&&fgetc(f)==EOF&&!memcmp(bytes,expected,n));fclose(f);
}
int main(void){
    unsetenv("REEDEN_FRAME_EVIDENCE_DIR");unsetenv("REEDEN_FRAME_EVIDENCE_LIMIT");unsetenv("REEDEN_TEXT_QUALITY_AFTER_TURN");
    unsetenv("REEDEN_TEXT_BAND_PATH");
    FrameEvidenceOptions options;assert(frame_evidence_options_load(&options));
    assert(!options.directory[0]&&!options.quality_after_turn&&options.reagl_bands&&options.limit==2);
    assert(!setenv("REEDEN_TEXT_BAND_PATH","stable",1));assert(frame_evidence_options_load(&options)&&!options.reagl_bands);
    assert(!setenv("REEDEN_TEXT_BAND_PATH","reagl",1));assert(frame_evidence_options_load(&options)&&options.reagl_bands);
    assert(!setenv("REEDEN_TEXT_BAND_PATH","unknown",1));assert(!frame_evidence_options_load(&options)&&options.reagl_bands);
    unsetenv("REEDEN_TEXT_BAND_PATH");
    FrameEvidenceInfo info={.width=4,.height=4,.logical_stride=16,.fb_stride=24,
        .xoffset=1,.yoffset=1,.old_valid=true,.profile="stable",.animated=true,
        .red={0,8,0},.green={8,8,0},.blue={16,8,0},.alpha={24,8,0}};
    uint8_t old[64],target[64],fb[120];memset(old,0,sizeof(old));memset(target,200,sizeof(target));memset(fb,80,sizeof(fb));
    assert(!frame_capture_begin(&options,&info,old,target,fb,sizeof(fb)).active);
    assert(!setenv("REEDEN_TEXT_QUALITY_AFTER_TURN","yes",1));assert(!frame_evidence_options_load(&options));
    assert(!options.directory[0]&&!options.quality_after_turn);
    assert(!setenv("REEDEN_TEXT_QUALITY_AFTER_TURN","1",1));assert(frame_evidence_options_load(&options)&&options.quality_after_turn);
    assert(!setenv("REEDEN_FRAME_EVIDENCE_DIR","relative",1));assert(!frame_evidence_options_load(&options)&&!options.quality_after_turn);
    char root[]="/tmp/reeden-frame-evidence-XXXXXX";assert(mkdtemp(root));
    assert(!setenv("REEDEN_FRAME_EVIDENCE_DIR",root,1));
    assert(!setenv("REEDEN_FRAME_EVIDENCE_LIMIT","5",1));assert(!frame_evidence_options_load(&options));
    assert(!setenv("REEDEN_FRAME_EVIDENCE_LIMIT","1",1));assert(frame_evidence_options_load(&options));
    FrameCapture c=frame_capture_begin(&options,&info,old,target,fb,sizeof(fb));assert(c.active&&c.ok);
    frame_capture_pixels(&c,"fb-converted.bin",fb,sizeof(fb));
    frame_capture_pixels(&c,"fb-complete.bin",fb,sizeof(fb));frame_capture_end(&c,"complete",17,1);assert(!c.active);
    exact_file(root,"old.bgra",old,sizeof(old));exact_file(root,"target.bgra",target,sizeof(target));
    exact_file(root,"fb-before.bin",fb,sizeof(fb));exact_file(root,"fb-converted.bin",fb,sizeof(fb));exact_file(root,"fb-complete.bin",fb,sizeof(fb));
    assert(!frame_capture_begin(&options,&info,old,target,fb,sizeof(fb)).active);
    options.used=0;assert(!frame_capture_begin(&options,&info,old,target,fb,sizeof(fb)).active); /* exclusive directory */
    char link[1200];snprintf(link,sizeof(link),"%s-link",root);assert(!symlink(root,link));
    strcpy(options.directory,link);options.used=0;
    assert(!frame_capture_begin(&options,&info,old,target,fb,sizeof(fb)).active);assert(!unlink(link));
    const char *names[]={"old.bgra","target.bgra","fb-before.bin","fb-converted.bin","fb-complete.bin","info.json","result.json"};
    char path[1200];for(unsigned i=0;i<sizeof(names)/sizeof(*names);i++){
        snprintf(path,sizeof(path),"%s/frame-%ld-1/%s",root,(long)getpid(),names[i]);assert(!unlink(path));
    }
    snprintf(path,sizeof(path),"%s/frame-%ld-1",root,(long)getpid());assert(!rmdir(path));assert(!rmdir(root));
    puts("PASS: default-off capture/quality, invalid options fail closed, byte-exact snapshots, bounded capture, no overwrite or terminal symlinks; no hardware accessed");
}

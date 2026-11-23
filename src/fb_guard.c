/* Statically linked musl utility, independent of Kobo/private libc. --inspect is read-only. */
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
struct snapshot { uint32_t magic,w,h,stride,rotate; };
struct update { uint32_t top,left,width,height,waveform,mode,marker,flags; int dither; };
/* A POSIX record lock survives exec, not fork or the owner's death. Native UI
 * children cannot keep a dead recovery worker's lock alive. Never unlink it. */
static int recover_session(const char *app,const char *owner){
    if(chdir(app)<0){perror("recovery directory");return 1;}
    int fd=open("state/recovery.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_NONBLOCK,0600);
    struct stat st;
    if(fd<0||fstat(fd,&st)<0){perror("recovery lock");return 1;}
    if(!S_ISREG(st.st_mode)||st.st_nlink!=1){fprintf(stderr,"Invalid recovery lock file\n");return 1;}
    struct flock lock={.l_type=F_WRLCK,.l_whence=SEEK_SET};
    if(fcntl(fd,F_SETLK,&lock)<0){
        if(errno==EACCES||errno==EAGAIN)return 75;
        perror("recovery lock");return 1;
    }
    execl("/bin/sh","sh","./recover.sh","--locked",owner,(char*)NULL);
    perror("recovery shell");return 1;
}
int main(int argc,char **argv){
    if(argc==4&&!strcmp(argv[1],"--recover-session"))return recover_session(argv[2],argv[3]);
    int inspect=argc==2&&!strcmp(argv[1],"--inspect"),save=argc==3&&!strcmp(argv[1],"--snapshot"),restore=argc==3&&!strcmp(argv[1],"--restore");
    if(!inspect&&!save&&!restore)return 64;
    int fd=open("/dev/fb0",restore?O_RDWR:O_RDONLY);struct fb_var_screeninfo v;struct fb_fix_screeninfo f;
    if(fd<0||ioctl(fd,FBIOGET_VSCREENINFO,&v)||ioctl(fd,FBIOGET_FSCREENINFO,&f)){perror("framebuffer query");return 1;}
    printf("framebuffer=%s size=%ux%u bpp=%u stride=%u rotate=%u RGB=%u:%u/%u:%u/%u:%u\n",f.id,v.xres,v.yres,v.bits_per_pixel,f.line_length,v.rotate,v.red.offset,v.red.length,v.green.offset,v.green.length,v.blue.offset,v.blue.length);
    if(strncmp(f.id,"hwtcon",6)||v.xres!=1072||v.yres!=1448||v.rotate!=3||v.bits_per_pixel!=32||v.xoffset||v.yoffset||f.line_length!=4288)return 2;
    if(inspect){close(fd);return 0;}
    size_t n=(size_t)f.line_length*v.yres;if(n>f.smem_len)return 3;
    void *mem=mmap(NULL,n,restore?(PROT_READ|PROT_WRITE):PROT_READ,MAP_SHARED,fd,0);if(mem==MAP_FAILED)return 4;
    FILE *fp=fopen(argv[2],save?"wb":"rb");if(!fp)return 5;
    struct snapshot expected={0x524b4642,v.xres,v.yres,f.line_length,v.rotate},actual;
    if(save){if(fwrite(&expected,sizeof(expected),1,fp)!=1||fwrite(mem,1,n,fp)!=n)return 6;}
    else {if(fread(&actual,sizeof(actual),1,fp)!=1||memcmp(&actual,&expected,sizeof(actual)))return 7;uint8_t *copy=malloc(n);if(!copy||fread(copy,1,n,fp)!=n)return 8;memcpy(mem,copy,n);free(copy);
        FILE *power=fopen("/proc/hwtcon/cmd","w");if(power){fputs("fiti_power 1",power);fclose(power);}
        struct update u={0,0,v.xres,v.yres,10,1,0x524b,0x600,0};if(ioctl(fd,0x4024462eUL,&u)<0){perror("restore refresh");return 9;}
    }
    fclose(fp);munmap(mem,n);close(fd);return 0;
}

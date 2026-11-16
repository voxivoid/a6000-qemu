/* mdlscan_so.c - route (b) RE: discover which model/CAMERA params are readable
 * in-process and with which getModelInfo selector (TRACKB_INPROCESS §16).
 *
 * getModelInfo("model/CAMERA", sel, out) fills `out` with group `sel`'s params;
 * ParamList::get(out, id) returns that param (value at +12) or NULL. sel is a
 * small int group number that differs per param (§16.2), so we scan sel x id and
 * log every (sel,id)->value that resolves. Pure reads (no mutation) -> safe.
 *
 * Self-gates to im.elf; logs to /setting/a5k/mdlscan.log. Build (a5k-cross):
 *   arm-linux-gnueabi-gcc -std=gnu99 -O2 -fPIC -shared -fno-stack-protector \
 *     -o mdlscan.so tools/mdlscan_so.c -lpthread
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/mman.h>

#define GETMI 0x39485CUL   /* libObj viewManagerIf::getModelInfo */
#define PLC   0x70912EUL   /* libObj ParamList::ParamList */
#define PLGET 0x0EA4B0UL   /* viewUnified2 ParamList::get */
#define AIF_PHYS 0xF3440000UL
static unsigned long gl, gv;

static void lg(const char *s){int fd=open("/setting/a5k/mdlscan.log",O_WRONLY|O_CREAT|O_APPEND,0644);if(fd>=0){(void)!write(fd,s,strlen(s));close(fd);}}
static unsigned long fb(const char*n){int fd=open("/proc/self/maps",O_RDONLY);if(fd<0)return 0;static char b[1<<20];int o=0,r;while(o<(int)sizeof b-1&&(r=read(fd,b+o,sizeof b-1-o))>0)o+=r;close(fd);if(o<=0)return 0;b[o]=0;unsigned long base=0;for(char*p=b;p;){char*nl=strchr(p,'\n');if(nl)*nl=0;if(strstr(p,n)&&strstr(p,"r-xp")){unsigned long s=strtoul(p,0,16);if(!base||s<base)base=s;}if(!nl)break;p=nl+1;}return base;}
static int osd_on(void){int fd=open("/dev/mem",O_RDONLY);if(fd<0)return -1;volatile uint32_t*r=mmap(0,0x1000,PROT_READ,MAP_SHARED,fd,AIF_PHYS);close(fd);if(r==MAP_FAILED)return -1;uint32_t v=r[0x144/4];munmap((void*)r,0x1000);return (v&2)?1:0;}

typedef void*(*ctor_fn)(void*);
typedef int(*gmi_fn)(const char*,unsigned long,void*,void*,void*);
typedef void*(*get_fn)(void*,unsigned int);

static void scan_once(void){
  ctor_fn ctor=(ctor_fn)(gl+PLC+1); gmi_fn g=(gmi_fn)(gl+GETMI+1); get_fn pg=(get_fn)(gv+PLGET+1);
  for(unsigned sel=0; sel<=40; sel++){
    unsigned char st[0x20]; memset(st,0,sizeof st); ctor(st);
    int ret=g("model/CAMERA",sel,st,0,0);
    if(ret==0) continue;
    int hits=0; char line[160];
    snprintf(line,sizeof line,"--- sel %u (ret=%d) ---\n",sel,ret); lg(line);
    for(unsigned id=0x100; id<=0x2A0; id++){
      void*p=pg(st,id);
      if(!p) continue;
      int v=*(int*)((char*)p+12);
      snprintf(line,sizeof line,"  id 0x%03x = %d\n",id,v); lg(line);
      if(++hits>64){ lg("  ...(>64, truncated)\n"); break; }
    }
    if(!hits) lg("  (no ids in 0x100..0x2A0)\n");
  }
}

static void *w(void*a){(void)a;
  for(int i=0;i<1200;i++){int s=osd_on();if(s==1)break;if(s<0&&i>100)break;struct timespec t={0,100*1000000L};nanosleep(&t,0);}
  gl=fb("libObj.so"); gv=fb("viewUnified2.so");
  if(!gl||!gv){lg("mdlscan: base NOT found\n");return 0;}
  char m[80];snprintf(m,sizeof m,"mdlscan: libObj=0x%lx vu=0x%lx\n",gl,gv);lg(m);
  lg("=== PASS 1 (initial state) ===\n"); scan_once();
  /* gap: host injects sendkey (half-press etc.) to change engine state */
  struct timespec g2={30,0}; nanosleep(&g2,0);
  lg("=== PASS 2 (after host sendkey) ===\n"); scan_once();
  lg("mdlscan: done\n"); return 0;}

static void comm(char*o,int n){o[0]=0;int fd=open("/proc/self/comm",O_RDONLY);if(fd<0)return;int r=read(fd,o,n-1);close(fd);if(r<0)r=0;while(r&&(o[r-1]=='\n'||o[r-1]==' '))r--;o[r]=0;}
__attribute__((constructor)) static void c(void){char cm[32];comm(cm,sizeof cm);if(strcmp(cm,"im.elf"))return;pthread_t t;if(pthread_create(&t,0,w,0)==0)pthread_detach(t);}

/* grpcmd_so.c - route (b): interactive CmnViewProcessDataMgr group prober for
 * empirically mapping option<->internal by STOCK OSD (TRACKB §23 follow-up).
 *
 * A resident worker in im.elf polls /setting/a5k/cmd.txt (~0.5s) and executes:
 *   "G V"      -> cvpdm group G write internal V, read-back -> cmd.out
 *   "R G"      -> read group G only
 *   "MODE E"   -> requestModelExecute("model/CAMERA",0x10,{0x12A=E}) (P=1,A=2,S=3,M=4,SCN=6)
 * It does NOT suppress the stock UI, so the stock OSD renders the live setting and
 * the host can screendump after each write to read the semantic (which option an
 * internal value is). Guarded derefs (abort out-of-range). RAM-only, reverts nothing
 * (host drives reverts). Self-gates to im.elf. Install via env.txt LD_PRELOAD with
 * A5K_UI_NOSUPPRESS semantics implicit (this .so never patches the draw gate).
 *
 * Build (a5k-cross):
 *   arm-linux-gnueabi-gcc -std=gnu99 -O2 -fPIC -shared -fno-stack-protector \
 *     -o grpcmd.so emulator/tools/grpcmd_so.c -lpthread
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

#define GUARD_OFF 0x47FFF4UL
#define OBJ_OFF   0x47FFF8UL
#define RD_VOFF   12
#define WR_VOFF   56
#define IP_CTOR   0x0EA468UL
#define PLGET     0x0EA4B0UL
#define GETMI     0x39485CUL
#define REQ       0x3948E0UL
#define PLC       0x70912EUL
#define PLADD     0x7095D6UL
#define AIF_PHYS  0xF3440000UL
#define CMD  "/setting/a5k/cmd.txt"
#define OUT  "/setting/a5k/cmd.out"

static unsigned long gl, gv, gv_end;
static void out(const char*s){int fd=open(OUT,O_WRONLY|O_CREAT|O_APPEND,0644);if(fd>=0){(void)!write(fd,s,strlen(s));close(fd);}}
static void outf(const char*f,...){char b[160];__builtin_va_list a;__builtin_va_start(a,f);vsnprintf(b,sizeof b,f,a);__builtin_va_end(a);out(b);}
static unsigned long fb(const char*n,unsigned long*endp){int fd=open("/proc/self/maps",O_RDONLY);if(fd<0)return 0;static char b[1<<20];int o=0,r;while(o<(int)sizeof b-1&&(r=read(fd,b+o,sizeof b-1-o))>0)o+=r;close(fd);if(o<=0)return 0;b[o]=0;unsigned long base=0,end=0;for(char*p=b;p;){char*nl=strchr(p,'\n');if(nl)*nl=0;if(strstr(p,n)){unsigned long s=strtoul(p,0,16);char*d=strchr(p,'-');unsigned long e=d?strtoul(d+1,0,16):0;if(!base||s<base)base=s;if(e>end)end=e;}if(!nl)break;p=nl+1;}if(endp)*endp=end;return base;}
static int osd_on(void){int fd=open("/dev/mem",O_RDONLY);if(fd<0)return -1;volatile uint32_t*r=mmap(0,0x1000,PROT_READ,MAP_SHARED,fd,AIF_PHYS);close(fd);if(r==MAP_FAILED)return -1;uint32_t v=r[0x144/4];munmap((void*)r,0x1000);return (v&2)?1:0;}
static int in_vu(unsigned long a){return a>=gv&&a<gv_end;}
static void nap(int ms){struct timespec t={ms/1000,(ms%1000)*1000000L};nanosleep(&t,0);}

typedef int(*rd_fn)(void*,int,int*);
typedef int(*wr_fn)(void*,int,int);
static int grp_read(int g,int*o){void**ob=(void**)(gv+OBJ_OFF);unsigned long vt=(unsigned long)*ob;if(!in_vu(vt))return -777;unsigned long m=*(unsigned long*)(vt+RD_VOFF);if(!in_vu(m&~1UL))return -777;int v=-1;int r=((rd_fn)m)(ob,g,&v);if(o)*o=v;return r;}
static int grp_write(int g,int val){void**ob=(void**)(gv+OBJ_OFF);unsigned long vt=(unsigned long)*ob;if(!in_vu(vt))return -777;unsigned long m=*(unsigned long*)(vt+WR_VOFF);if(!in_vu(m&~1UL))return -777;return ((wr_fn)m)(ob,g,val);}
static int model_set1(unsigned long cmd,unsigned int pid,int val){
  typedef void*(*plc)(void*);typedef void*(*ipc)(void*,int);typedef void(*pla)(void*,unsigned long,void*);typedef int(*req)(const char*,unsigned long,void*);
  plc c=(plc)((gl+PLC)|1);pla a=(pla)((gl+PLADD)|1);ipc i=(ipc)((gv+IP_CTOR)|1);req rq=(req)((gl+REQ)|1);
  void*list=malloc(8);void*p=malloc(0x10);if(!list||!p)return -1;c(list);i(p,val);a(list,pid,p);return rq("model/CAMERA",cmd,list);}
static int read_mode(void){
  typedef void*(*plc)(void*);typedef int(*gmi)(const char*,unsigned long,void*,void*,void*);typedef void*(*plg)(void*,unsigned int);
  plc c=(plc)((gl+PLC)|1);gmi g=(gmi)((gl+GETMI)|1);plg pg=(plg)((gv+PLGET)|1);
  unsigned char st[0x20];memset(st,0,sizeof st);c(st);int ret=g("model/CAMERA",15,st,0,0);if(ret==0)return -1;void*p=pg(st,0x12C);if(!p)return -1;return *(int*)((char*)p+12);}

static void handle(char*line){
  while(*line==' ')line++;
  if(line[0]=='M'){int e=atoi(line+5);int r=model_set1(0x10,0x12A,e);nap(300);int md=-1;for(int i=0;i<30;i++){nap(200);md=read_mode();if(md==e)break;}outf("MODE %d ret=%d modeD=%d\n",e,r,md);return;}
  if(line[0]=='R'){int g=atoi(line+2);int v=-1;int r=grp_read(g,&v);outf("R g%d ret=%d v=%d\n",g,r,v);return;}
  int g=0,val=0;if(sscanf(line,"%d %d",&g,&val)==2){int rw=grp_write(g,val);nap(400);int v=-1;int rr=grp_read(g,&v);outf("W g%d=%d wret=%d -> read(ret=%d)=%d\n",g,val,rw,rr,v);}
}

static void comm(char*o,int n){o[0]=0;int fd=open("/proc/self/comm",O_RDONLY);if(fd<0)return;int r=read(fd,o,n-1);close(fd);if(r<0)r=0;while(r&&(o[r-1]=='\n'||o[r-1]==' '))r--;o[r]=0;}
static void*w(void*a){(void)a;
  for(int i=0;i<1500;i++){int s=osd_on();if(s==1)break;if(s<0&&i>100)break;nap(100);}
  nap(3000);
  gl=fb("libObj.so",0); gv=fb("viewUnified2.so",&gv_end);
  if(!gl||!gv){out("grpcmd: base NOT found\n");return 0;}
  unsigned int guard=*(unsigned int*)(gv+GUARD_OFF);
  outf("grpcmd: ready libObj=0x%lx vu=0x%lx guard=0x%x\n",gl,gv,guard);
  if(!(guard&1)){out("grpcmd: global NOT constructed\n");return 0;}
  for(;;){
    int fd=open(CMD,O_RDONLY);
    if(fd>=0){char b[128];int n=read(fd,b,sizeof b-1);close(fd);
      if(n>0){b[n]=0;char*nl=strchr(b,'\n');if(nl)*nl=0;
        if(b[0]){ unlink(CMD); outf(">> %s\n",b); handle(b); }
      }
    }
    nap(500);
  }
  return 0;}
__attribute__((constructor)) static void c(void){char cm[32];comm(cm,sizeof cm);if(strcmp(cm,"im.elf"))return;pthread_t t;if(pthread_create(&t,0,w,0)==0)pthread_detach(t);}

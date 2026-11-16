/* grpscan2_so.c - route (b): verify CmnViewProcessDataMgr per-GROUP SET round-trip
 * for settings that are INHIBITED in Scene mode. Pass 1 of grpscan_so.c showed
 * g2(image-quality)/g8(flash-mode) round-trip in the stock boot mode (SCN), but
 * drive/focus/dro/metering/picture-effect/object-track/smile/steadyshot returned
 * ret=0 -- the stock engine inhibits them in SCN (same as the stock menu greys
 * them out: WrapperXxx::isInhOn). So this tool first switches the camera to P
 * (program-auto) via the proven requestModelExecute("model/CAMERA",0x10,{0x12A=1})
 * (TRACKB §19 / camera_inproc setSetting), waits for the pump to apply, then does
 * the group round-trips. Reverts each value and restores SCN at the end. RAM-only,
 * guarded, self-gates to im.elf.
 *
 * Build (a5k-cross):
 *   arm-linux-gnueabi-gcc -std=gnu99 -O2 -fPIC -shared -fno-stack-protector \
 *     -o grpscan2.so emulator/tools/grpscan2_so.c -lpthread
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

/* viewUnified2 */
#define GUARD_OFF 0x47FFF4UL
#define OBJ_OFF   0x47FFF8UL
#define RD_VOFF   12
#define WR_VOFF   56
#define IP_CTOR   0x0EA468UL   /* IntParam ctor(obj,val) */
#define PLGET     0x0EA4B0UL
/* libObj */
#define GETMI     0x39485CUL
#define REQ       0x3948E0UL   /* requestModelExecute(name,cmd,ParamList*) */
#define PLC       0x70912EUL   /* ParamList::ParamList */
#define PLADD     0x7095D6UL   /* ParamList::add(id,ParamBase*) */
#define AIF_PHYS  0xF3440000UL

static unsigned long gl, gv, gv_end;
static void lg(const char*s){int fd=open("/setting/a5k/grpscan2.log",O_WRONLY|O_CREAT|O_APPEND,0644);if(fd>=0){(void)!write(fd,s,strlen(s));close(fd);}}
static void lgf(const char*f,...){char b[160];__builtin_va_list a;__builtin_va_start(a,f);vsnprintf(b,sizeof b,f,a);__builtin_va_end(a);lg(b);}
static unsigned long fb(const char*n,unsigned long*endp){int fd=open("/proc/self/maps",O_RDONLY);if(fd<0)return 0;static char b[1<<20];int o=0,r;while(o<(int)sizeof b-1&&(r=read(fd,b+o,sizeof b-1-o))>0)o+=r;close(fd);if(o<=0)return 0;b[o]=0;unsigned long base=0,end=0;for(char*p=b;p;){char*nl=strchr(p,'\n');if(nl)*nl=0;if(strstr(p,n)){unsigned long s=strtoul(p,0,16);char*d=strchr(p,'-');unsigned long e=d?strtoul(d+1,0,16):0;if(!base||s<base)base=s;if(e>end)end=e;}if(!nl)break;p=nl+1;}if(endp)*endp=end;return base;}
static int osd_on(void){int fd=open("/dev/mem",O_RDONLY);if(fd<0)return -1;volatile uint32_t*r=mmap(0,0x1000,PROT_READ,MAP_SHARED,fd,AIF_PHYS);close(fd);if(r==MAP_FAILED)return -1;uint32_t v=r[0x144/4];munmap((void*)r,0x1000);return (v&2)?1:0;}
static int in_vu(unsigned long a){return a>=gv&&a<gv_end;}
static void nap(int ms){struct timespec t={ms/1000,(ms%1000)*1000000L};nanosleep(&t,0);}

typedef int(*rd_fn)(void*,int,int*);
typedef int(*wr_fn)(void*,int,int);
static int grp_read(int g,int*out){void**obj=(void**)(gv+OBJ_OFF);unsigned long vt=(unsigned long)*obj;if(!in_vu(vt))return -777;unsigned long m=*(unsigned long*)(vt+RD_VOFF);if(!in_vu(m&~1UL))return -777;int v=-1;int r=((rd_fn)m)(obj,g,&v);if(out)*out=v;return r;}
static int grp_write(int g,int val){void**obj=(void**)(gv+OBJ_OFF);unsigned long vt=(unsigned long)*obj;if(!in_vu(vt))return -777;unsigned long m=*(unsigned long*)(vt+WR_VOFF);if(!in_vu(m&~1UL))return -777;return ((wr_fn)m)(obj,g,val);}

/* model/CAMERA single-int SET via the proven requestModelExecute ParamList ABI. */
static int model_set1(const char*model,unsigned long cmd,unsigned int pid,int val){
  typedef void*(*plc)(void*);typedef void*(*ipc)(void*,int);typedef void(*pla)(void*,unsigned long,void*);typedef int(*req)(const char*,unsigned long,void*);
  plc c=(plc)((gl+PLC)|1);pla a=(pla)((gl+PLADD)|1);ipc i=(ipc)((gv+IP_CTOR)|1);req rq=(req)((gl+REQ)|1);
  void*list=malloc(8);void*p=malloc(0x10);if(!list||!p)return -1;c(list);i(p,val);a(list,pid,p);return rq(model,cmd,list);}
/* read shooting mode engine D value (§16, getModelInfo sel 15 -> 0x12C). */
static int read_mode(void){
  typedef void*(*plc)(void*);typedef int(*gmi)(const char*,unsigned long,void*,void*,void*);typedef void*(*plg)(void*,unsigned int);
  plc c=(plc)((gl+PLC)|1);gmi g=(gmi)((gl+GETMI)|1);plg pg=(plg)((gv+PLGET)|1);
  unsigned char st[0x20];memset(st,0,sizeof st);c(st);int ret=g("model/CAMERA",15,st,0,0);if(ret==0)return -1;void*p=pg(st,0x12C);if(!p)return -1;return *(int*)((char*)p+12);}

struct G{int group;const char*name;int alt;};
static struct G T[]={
  {  7,"drive-mode",      2},
  {  9,"flash-comp",     -1},  /* int range; alt = v0+1 handled below */
  { 13,"focus-mode",      0},  /* valid incl 0(AF),2,3,4,6 */
  { 20,"exposure-comp",  -1},  /* int range */
  { 22,"metering-mode",   4},  /* valid {0,1,4} */
  { 24,"dro-hdr",         0},  /* valid {0,1,..} */
  { 26,"picture-effect",  1},  /* art-filter */
  { 29,"object-tracking", 1},
  { 31,"smile-face",      1},
  { 33,"auto-portrait-fr",1},
  {208,"steadyshot",      0},
};

static void roundtrip(int modeTag){
  int n=sizeof T/sizeof T[0];
  for(int i=0;i<n;i++){
    int v0=-1;int r0=grp_read(T[i].group,&v0);
    if(r0==-777){lgf("[m%d] g%-3d %-16s READ guard-fail\n",modeTag,T[i].group,T[i].name);continue;}
    if(r0==0){lgf("[m%d] g%-3d %-16s READ ret=0 (inhibited/invalid)\n",modeTag,T[i].group,T[i].name);continue;}
    int alt=T[i].alt; if(alt<0) alt=v0+1; if(alt==v0) alt=(v0==0)?1:0;
    int rw=grp_write(T[i].group,alt); nap(500);
    int v1=-1;int r1=grp_read(T[i].group,&v1);
    int rv=grp_write(T[i].group,v0); nap(300);
    int v2=-1; grp_read(T[i].group,&v2);
    const char*verdict=(r1!=0&&v1==alt)?"ROUNDTRIP-VERIFIED":(rw==0)?"write-rejected(inhibited) [?]":(r1!=0&&v1==v0)?"write-ignored [?]":"unexpected [?]";
    lgf("[m%d] g%-3d %-16s v0=%d write%d(ret=%d)->v1=%d revert(ret=%d)->v2=%d  %s\n",
        modeTag,T[i].group,T[i].name,v0,alt,rw,v1,rv,v2,verdict);
    nap(200);
  }
}

static void comm(char*o,int n){o[0]=0;int fd=open("/proc/self/comm",O_RDONLY);if(fd<0)return;int r=read(fd,o,n-1);close(fd);if(r<0)r=0;while(r&&(o[r-1]=='\n'||o[r-1]==' '))r--;o[r]=0;}
static void*w(void*a){(void)a;
  for(int i=0;i<1500;i++){int s=osd_on();if(s==1)break;if(s<0&&i>100)break;nap(100);}
  nap(4000);
  gl=fb("libObj.so",0); gv=fb("viewUnified2.so",&gv_end);
  if(!gl||!gv){lg("grpscan2: base NOT found\n");return 0;}
  unsigned int guard=*(unsigned int*)(gv+GUARD_OFF);
  lgf("grpscan2: libObj=0x%lx vu=0x%lx..0x%lx guard=0x%x\n",gl,gv,gv_end,guard);
  if(!(guard&1)){lg("grpscan2: global NOT constructed -> abort\n");return 0;}
  int m0=read_mode();
  lgf("grpscan2: boot shooting-mode D=%d\n",m0);
  lg("=== PASS A (boot mode, baseline) ===\n"); roundtrip(0);
  /* switch to P (program-auto): engine enum 1 via 0x12A (RE_MODE_DIAL). */
  int rp=model_set1("model/CAMERA",0x10,0x12A,1);
  int md=-1; for(int i=0;i<30;i++){nap(200);md=read_mode();if(md==1)break;}
  lgf("grpscan2: set P ret=%d, mode D now=%d\n",rp,md);
  nap(1500);
  lg("=== PASS B (program-auto P) ===\n"); roundtrip(1);
  /* restore scene mode (enum 6 -> boot landscape path). */
  int rr=model_set1("model/CAMERA",0x10,0x12A,6);
  nap(2000); int mr=read_mode();
  lgf("grpscan2: restore SCN ret=%d, mode D now=%d\n",rr,mr);
  lg("grpscan2: done\n");return 0;}
__attribute__((constructor)) static void c(void){char cm[32];comm(cm,sizeof cm);if(strcmp(cm,"im.elf"))return;pthread_t t;if(pthread_create(&t,0,w,0)==0)pthread_detach(t);}

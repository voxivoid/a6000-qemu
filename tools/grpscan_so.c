/* grpscan_so.c - route (b) RE: verify the CmnViewProcessDataMgr singleton as a
 * generic per-GROUP setting read/write slot (TRACKB_INPROCESS §18 generalised).
 *
 * §18 proved image-aspect = group 1: READ  = vtable[3]  (off 12) m(obj,1,&out),
 *                                     WRITE = vtable[14] (off 56) m(obj,1,val).
 * The stock Wrapper setters (viewUnified2) all call the SAME global singleton
 * (&unk_47FFF8, resolved by sub_11BB1C) with vtable[14](obj, GROUP, encodedVal),
 * GROUP differing per setting (aspect 1, image-quality 2, drive 7, flash 8,
 * flash-comp 9, focus 13, exp-comp 20, metering 22, dro 24, art-filter 26,
 * object-track 29, smile 31, auto-portrait 33, auto 35, scene 36, audio-lvl 44,
 * rec-media 147, focus-area 187/188, steadyshot 208, nd 225 -- extracted from
 * the setXxx bodies). This tool verifies, per group, that:
 *   (a) READ  via vtable[3] with that group returns a bound value (ret!=0), and
 *   (b) for the enum groups with known-valid alternates, WRITE via vtable[14]
 *       then READ-back reflects the new value (round-trip), then reverts.
 * Everything reverts; pure reads for groups without a safe alternate. RAM-only,
 * guarded derefs (abort out-of-range, no forced pointers). Self-gates to im.elf.
 *
 * Build (a5k-cross):
 *   docker run --rm -v "$PWD":/w a5k-cross arm-linux-gnueabi-gcc -std=gnu99 -O2 \
 *     -fPIC -shared -fno-stack-protector -o grpscan.so /w/emulator/tools/grpscan_so.c -lpthread
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
#define RD_VOFF   12   /* vtable[3]  read  m(obj, group, &out) */
#define WR_VOFF   56   /* vtable[14] write m(obj, group, val)  */
#define AIF_PHYS  0xF3440000UL

static unsigned long gv, gv_end;

static void lg(const char *s){int fd=open("/setting/a5k/grpscan.log",O_WRONLY|O_CREAT|O_APPEND,0644);if(fd>=0){(void)!write(fd,s,strlen(s));close(fd);}}
static void lgf(const char *f,...){char b[160];__builtin_va_list a;__builtin_va_start(a,f);vsnprintf(b,sizeof b,f,a);__builtin_va_end(a);lg(b);}
static unsigned long find_base(const char*n,unsigned long*endp){
  int fd=open("/proc/self/maps",O_RDONLY);if(fd<0)return 0;static char b[1<<20];int o=0,r;
  while(o<(int)sizeof b-1&&(r=read(fd,b+o,sizeof b-1-o))>0)o+=r;close(fd);if(o<=0)return 0;b[o]=0;
  unsigned long base=0,end=0;
  for(char*p=b;p;){char*nl=strchr(p,'\n');if(nl)*nl=0;
    if(strstr(p,n)){unsigned long s=strtoul(p,0,16);char*d=strchr(p,'-');unsigned long e=d?strtoul(d+1,0,16):0;
      if(!base||s<base)base=s; if(e>end)end=e;}
    if(!nl)break;p=nl+1;}
  if(endp)*endp=end; return base;}
static int osd_on(void){int fd=open("/dev/mem",O_RDONLY);if(fd<0)return -1;volatile uint32_t*r=mmap(0,0x1000,PROT_READ,MAP_SHARED,fd,AIF_PHYS);close(fd);if(r==MAP_FAILED)return -1;uint32_t v=r[0x144/4];munmap((void*)r,0x1000);return (v&2)?1:0;}
static int in_vu(unsigned long a){return a>=gv && a<gv_end;}

typedef int(*rd_fn)(void*,int,int*);
typedef int(*wr_fn)(void*,int,int);

/* read group -> *out (internal value). returns ret of the vtable call, or -777 on guard fail. */
static int grp_read(int group,int*out){
  void**obj=(void**)(gv+OBJ_OFF);
  unsigned long vt=(unsigned long)*obj;
  if(!in_vu(vt))return -777;
  unsigned long m=*(unsigned long*)(vt+RD_VOFF);
  if(!in_vu(m&~1UL))return -777;
  int v=-1;int r=((rd_fn)m)(obj,group,&v);
  if(out)*out=v;
  return r;
}
static int grp_write(int group,int val){
  void**obj=(void**)(gv+OBJ_OFF);
  unsigned long vt=(unsigned long)*obj;
  if(!in_vu(vt))return -777;
  unsigned long m=*(unsigned long*)(vt+WR_VOFF);
  if(!in_vu(m&~1UL))return -777;
  return ((wr_fn)m)(obj,group,val);
}
static void nap(int ms){struct timespec t={ms/1000,(ms%1000)*1000000L};nanosleep(&t,0);}

/* group table. wr=1 -> attempt a round-trip write of `alt` (a known-valid internal
 * value for that setting); wr=0 -> read only (range/complex encoders). */
struct G{int group;const char*name;int wr;int alt;};
static struct G T[]={
  {  1,"image-aspect",     1, 3},  /* valid {2,3,4}; alt chosen vs current */
  {  2,"image-quality",    1, 2},  /* valid {0..4} */
  {  7,"drive-mode",       1, 0},  /* valid {0,1,2,..} */
  {  8,"flash-mode",       1, 0},  /* valid {0,1,2,3,5,6} */
  {  9,"flash-comp",       0, 0},  /* int range */
  { 13,"focus-mode",       1, 0},  /* valid {0,6,2,3,4} + 1 */
  { 20,"exposure-comp",    0, 0},  /* int range */
  { 22,"metering-mode",    1, 0},  /* valid {0,1,2} */
  { 24,"dro-hdr",          1, 0},  /* valid {0,1,2,..} */
  { 26,"picture-effect",   1, 0},  /* art-filter valid {0,1,..} */
  { 29,"object-tracking",  1, 0},
  { 31,"smile-face",       1, 0},  /* valid {0,1,..6} */
  { 33,"auto-portrait-fr", 1, 0},
  { 35,"auto-mode",        0, 0},
  { 36,"scene-mode",       0, 0},  /* scene enum; read only */
  { 44,"audio-rec-level",  0, 0},
  {147,"recording-media",  0, 0},
  {187,"focus-area(187)",  0, 0},
  {188,"focus-area(188)",  0, 0},
  {208,"steadyshot",       1, 0},
  {225,"nd-filter",        0, 0},
};

static void scan(void){
  int n=sizeof T/sizeof T[0];
  for(int i=0;i<n;i++){
    int v0=-1;int r0=grp_read(T[i].group,&v0);
    if(r0==-777){lgf("g%-3d %-18s READ guard-fail\n",T[i].group,T[i].name);continue;}
    if(r0==0){lgf("g%-3d %-18s READ ret=0 (not bound/invalid group)  [?]\n",T[i].group,T[i].name);continue;}
    if(!T[i].wr){lgf("g%-3d %-18s READ v=%d  [read-verified]\n",T[i].group,T[i].name,v0);continue;}
    int alt=T[i].alt; if(alt==v0) alt=(v0==0)?1:0; if(T[i].group==1) alt=(v0==2)?4:2;
    int rw=grp_write(T[i].group,alt); nap(400);
    int v1=-1;int r1=grp_read(T[i].group,&v1);
    int rv=grp_write(T[i].group,v0); nap(300);
    int v2=-1; grp_read(T[i].group,&v2);
    const char*verdict = (r1!=0 && v1==alt) ? "ROUNDTRIP-VERIFIED"
                        : (r1!=0 && v1==v0)  ? "write-ignored(read-only?) [?]"
                        : "unexpected [?]";
    lgf("g%-3d %-18s v0=%d write%d(ret=%d)->v1=%d revert->v2=%d  %s\n",
        T[i].group,T[i].name,v0,alt,rw,v1,v2,verdict);
    nap(300);
  }
}

static void comm(char*o,int n){o[0]=0;int fd=open("/proc/self/comm",O_RDONLY);if(fd<0)return;int r=read(fd,o,n-1);close(fd);if(r<0)r=0;while(r&&(o[r-1]=='\n'||o[r-1]==' '))r--;o[r]=0;}
static void*w(void*a){(void)a;
  for(int i=0;i<1500;i++){int s=osd_on();if(s==1)break;if(s<0&&i>100)break;nap(100);}
  nap(3000);  /* let the engine fully bind the global to the top view */
  gv=find_base("viewUnified2.so",&gv_end);
  if(!gv){lg("grpscan: vu base NOT found\n");return 0;}
  unsigned int guard=*(unsigned int*)(gv+GUARD_OFF);
  lgf("grpscan: vu=0x%lx..0x%lx guard=0x%x\n",gv,gv_end,guard);
  if(!(guard&1)){lg("grpscan: global NOT constructed -> abort\n");return 0;}
  lg("=== grpscan round-trip ===\n"); scan();
  lg("grpscan: done\n");return 0;}
__attribute__((constructor)) static void c(void){char cm[32];comm(cm,sizeof cm);if(strcmp(cm,"im.elf"))return;pthread_t t;if(pthread_create(&t,0,w,0)==0)pthread_detach(t);}

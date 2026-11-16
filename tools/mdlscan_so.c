/* mdlscan_so.c - route (b) RE: discover the live OSD state sources in-process.
 *
 * Two namespaces are dumped, both pure reads (no mutation -> safe):
 *   (A) model/CAMERA engine params: getModelInfo("model/CAMERA", sel, out) fills
 *       `out` with group `sel`'s params; ParamList::get(out, id) returns that
 *       param (value at +12) or NULL. sel is a small int group number that
 *       differs per param (TRACKB_INPROCESS §16.2), so we scan sel x id and log
 *       every (sel,id)->value that resolves. 0x12C (sel 15) = s_exposureMode D.
 *   (B) CmnViewProcessDataMgr groups: the viewUnified2 Wrapper getters all read
 *       the SAME global singleton (&unk_47FFF8) via vtable[3](obj, GROUP, &out),
 *       GROUP differing per setting (§23: aspect 1, image-quality 2, drive 7,
 *       flash 8, flash-comp 9, focus 13, exp-comp 20, metering 22, dro 24,
 *       art 26, obj-track 29, smile/face 31, scene 36, focus-area 187/188,
 *       steadyshot 208, nd 225, ...). This is the UI-side "internal value" the
 *       stock OSD renders. We read groups 0..255 and log every bound one.
 *
 * Purpose: identify, per OSD field, the live (namespace, selector/group, id,
 * internal value) so camera_inproc.cpp readState() can read the real value
 * instead of a placeholder. PASS 1 = boot state, PASS 2 = after a host-driven
 * stock-menu change (toggle-diff) -> the field whose value moved is confirmed.
 *
 * Self-gates to im.elf; logs to /setting/a5k/mdlscan.log. Build (a5k-cross):
 *   docker run --rm -v "$PWD":/w a5k-cross arm-linux-gnueabi-gcc -std=gnu99 -O2 \
 *     -fPIC -shared -fno-stack-protector -o mdlscan.so /w/emulator/tools/mdlscan_so.c -lpthread
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

/* CmnViewProcessDataMgr global singleton (viewUnified2), §18/§23. */
#define CVPDM_GUARD 0x47FFF4UL  /* _cxa_guard: bit0 = constructed */
#define CVPDM_OBJ   0x47FFF8UL  /* obj pointer (its *obj = vtable)  */
#define CVPDM_RD    12          /* vtable[3]: read  m(obj, group, &out) */
#define CVPDM_WR    56          /* vtable[14]: write m(obj, group, val) */

static unsigned long gl, gv, gv_end;

static void lg(const char *s){int fd=open("/setting/a5k/mdlscan.log",O_WRONLY|O_CREAT|O_APPEND,0644);if(fd>=0){(void)!write(fd,s,strlen(s));close(fd);}}
static void lgf(const char*f,...){char b[200];__builtin_va_list a;__builtin_va_start(a,f);vsnprintf(b,sizeof b,f,a);__builtin_va_end(a);lg(b);}
static unsigned long fb(const char*n,unsigned long*endp){int fd=open("/proc/self/maps",O_RDONLY);if(fd<0)return 0;static char b[1<<20];int o=0,r;while(o<(int)sizeof b-1&&(r=read(fd,b+o,sizeof b-1-o))>0)o+=r;close(fd);if(o<=0)return 0;b[o]=0;unsigned long base=0,end=0;for(char*p=b;p;){char*nl=strchr(p,'\n');if(nl)*nl=0;if(strstr(p,n)){if(strstr(p,"r-xp")){unsigned long s=strtoul(p,0,16);if(!base||s<base)base=s;}char*d=strchr(p,'-');unsigned long e=d?strtoul(d+1,0,16):0;if(e>end)end=e;}if(!nl)break;p=nl+1;}if(endp)*endp=end;return base;}
static int osd_on(void){int fd=open("/dev/mem",O_RDONLY);if(fd<0)return -1;volatile uint32_t*r=mmap(0,0x1000,PROT_READ,MAP_SHARED,fd,AIF_PHYS);close(fd);if(r==MAP_FAILED)return -1;uint32_t v=r[0x144/4];munmap((void*)r,0x1000);return (v&2)?1:0;}
static int in_vu(unsigned long a){return a>=gv && a<gv_end;}

typedef void*(*ctor_fn)(void*);
typedef int(*gmi_fn)(const char*,unsigned long,void*,void*,void*);
typedef void*(*get_fn)(void*,unsigned int);
typedef int(*rd_fn)(void*,int,int*);

/* (A) model/CAMERA selector x id scan ------------------------------------- */
static void scan_model(void){
  ctor_fn ctor=(ctor_fn)(gl+PLC+1); gmi_fn g=(gmi_fn)(gl+GETMI+1); get_fn pg=(get_fn)(gv+PLGET+1);
  lg("-- (A) model/CAMERA getModelInfo(sel) x ParamList::get(id) --\n");
  for(unsigned sel=0; sel<=40; sel++){
    unsigned char st[0x20]; memset(st,0,sizeof st); ctor(st);
    int ret=g("model/CAMERA",sel,st,0,0);
    if(ret==0) continue;
    int hits=0; char line[160];
    snprintf(line,sizeof line,"sel %u (ret=%d):\n",sel,ret); lg(line);
    for(unsigned id=0x100; id<=0x2A0; id++){
      void*p=pg(st,id);
      if(!p) continue;
      int v=*(int*)((char*)p+12);
      snprintf(line,sizeof line,"  id 0x%03x = %d\n",id,v); lg(line);
      if(++hits>96){ lg("  ...(>96, truncated)\n"); break; }
    }
    if(!hits) lg("  (no ids in 0x100..0x2A0)\n");
  }
}

/* (B) CmnViewProcessDataMgr group read scan -------------------------------- */
/* group -> label (for the known OSD-relevant groups; others logged numerically) */
static const char* grp_name(int g){
  switch(g){
    case 1:return"image-aspect"; case 2:return"image-quality"; case 7:return"drive-mode";
    case 8:return"flash-mode"; case 9:return"flash-comp"; case 13:return"focus-mode";
    case 20:return"exposure-comp"; case 22:return"metering-mode"; case 24:return"dro-hdr";
    case 26:return"picture-effect"; case 29:return"object-tracking"; case 31:return"smile-face";
    case 33:return"auto-portrait-fr"; case 35:return"auto-mode"; case 36:return"scene-mode";
    case 44:return"audio-rec-level"; case 147:return"recording-media"; case 187:return"focus-area(187)";
    case 188:return"focus-area(188)"; case 208:return"steadyshot"; case 225:return"nd-filter";
    default:return"";
  }
}
static int grp_read(int group,int*out){
  if(!(*(unsigned int*)(gv+CVPDM_GUARD)&1)) return -777;
  void**obj=(void**)(gv+CVPDM_OBJ);
  unsigned long vt=(unsigned long)*obj; if(!in_vu(vt)) return -777;
  unsigned long m=*(unsigned long*)(vt+CVPDM_RD); if(!in_vu(m&~1UL)) return -777;
  int v=-1; int r=((rd_fn)m)(obj,group,&v); if(out)*out=v; return r;
}
static void scan_groups(void){
  lg("-- (B) CmnViewProcessDataMgr vtable[3] group read (0..255) --\n");
  if(!(*(unsigned int*)(gv+CVPDM_GUARD)&1)){ lg("  (singleton NOT constructed -> skip)\n"); return; }
  int bound=0;
  for(int g=0; g<=255; g++){
    int v=-1; int r=grp_read(g,&v);
    if(r==-777){ lg("  guard-fail -> abort group scan\n"); return; }
    if(r==0) continue;            /* unbound / invalid group */
    const char*nm=grp_name(g);
    lgf("  g%-3d %-18s = %d\n", g, nm[0]?nm:"(unnamed)", v);
    bound++;
  }
  if(!bound) lg("  (no bound groups)\n");
}

static int grp_write(int group,int val){
  if(!(*(unsigned int*)(gv+CVPDM_GUARD)&1)) return -777;
  void**obj=(void**)(gv+CVPDM_OBJ);
  unsigned long vt=(unsigned long)*obj; if(!in_vu(vt)) return -777;
  unsigned long m=*(unsigned long*)(vt+CVPDM_WR); if(!in_vu(m&~1UL)) return -777;
  typedef int(*wr_fn)(void*,int,int); return ((wr_fn)m)(obj,group,val);
}

static void scan_once(void){ scan_model(); scan_groups(); }

/* Self-contained toggle-diff (engine set, allowed per task): change storage-fmt
 * (g2, round-trip VERIFIED §23.2/§23.3; OSD-decoded 0=RAW 1=RAW+J 2=JPEG) to a
 * different internal value, read back (confirms the read path reflects a real
 * change), then REVERT. Safe/reversible, RAM-only. */
static void toggle_diff(void){
  int v0=-1,r0=grp_read(2,&v0);
  if(r0<=0){ lgf("toggle: g2 read baseline failed r=%d\n",r0); return; }
  int alt=(v0==2)?1:2;
  int rw=grp_write(2,alt); struct timespec t={0,400*1000000L}; nanosleep(&t,0);
  int v1=-1; grp_read(2,&v1);
  int rr=grp_write(2,v0); nanosleep(&t,0);
  int v2=-1; grp_read(2,&v2);
  lgf("toggle: g2 storage-fmt v0=%d write(%d)ret=%d -> read=%d  revert(%d)ret=%d -> read=%d  %s\n",
      v0,alt,rw,v1,v0,rr,v2, (v1==alt&&v2==v0)?"TOGGLE-DIFF VERIFIED":"[?]");
}

static void *w(void*a){(void)a;
  for(int i=0;i<1500;i++){int s=osd_on();if(s==1)break;if(s<0&&i>100)break;struct timespec t={0,100*1000000L};nanosleep(&t,0);}
  gl=fb("libObj.so",0); gv=fb("viewUnified2.so",&gv_end);
  if(!gl||!gv){lg("mdlscan: base NOT found\n");return 0;}
  struct timespec s3={3,0}; nanosleep(&s3,0);  /* let the engine bind the global to the top view */
  lgf("mdlscan: libObj=0x%lx vu=0x%lx..0x%lx guard=0x%x\n",gl,gv,gv_end,*(unsigned int*)(gv+CVPDM_GUARD));
  lg("=== PASS 1 (boot state) ===\n"); scan_once();
  struct timespec g2={8,0}; nanosleep(&g2,0);
  lg("=== TOGGLE-DIFF (engine set g2, reverted) ===\n"); toggle_diff();
  struct timespec g3={2,0}; nanosleep(&g3,0);
  lg("=== PASS 2 (after revert; should equal PASS 1) ===\n"); scan_once();
  lg("mdlscan: done\n"); return 0;}

static void comm(char*o,int n){o[0]=0;int fd=open("/proc/self/comm",O_RDONLY);if(fd<0)return;int r=read(fd,o,n-1);close(fd);if(r<0)r=0;while(r&&(o[r-1]=='\n'||o[r-1]==' '))r--;o[r]=0;}
__attribute__((constructor)) static void c(void){char cm[32];comm(cm,sizeof cm);if(strcmp(cm,"im.elf"))return;pthread_t t;if(pthread_create(&t,0,w,0)==0)pthread_detach(t);}

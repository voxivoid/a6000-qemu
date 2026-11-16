/* aspect_so.c - §18 OSD-visible SET: toggle image aspect ratio via the global
 * CmnViewProcessDataMgr (viewUnified2 unk_47FFF8), which stock keeps bound to
 * the current view. Aspect read = vtable[3] (off 12): m(obj,1,&out);
 * write = vtable[14] (off 56): m(obj,1,val). Valid internal values {2,3,4}
 * (sub_21F832). Guards every deref; aborts (logs) on anything unexpected -- no
 * ViewManager walk, no forced pointers. RAM-only. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#define GUARD_OFF 0x47FFF4UL
#define OBJ_OFF   0x47FFF8UL
#define RD_VOFF   12
#define WR_VOFF   56
static unsigned long gv, gv_end;
static void lg(const char*s){int fd=open("/android/a5k/aspect.log",O_WRONLY|O_CREAT|O_APPEND,0644);if(fd>=0){(void)!write(fd,s,strlen(s));close(fd);}}
/* return base; also set *endp to end of that r-xp..rw-p file range (rough) */
static unsigned long find_base(const char*name,unsigned long*endp){
  int fd=open("/proc/self/maps",O_RDONLY);if(fd<0)return 0;static char b[1<<20];int o=0,r;
  while(o<(int)sizeof b-1&&(r=read(fd,b+o,sizeof b-1-o))>0)o+=r;close(fd);if(o<=0)return 0;b[o]=0;
  unsigned long base=0,end=0;
  for(char*p=b;p;){char*nl=strchr(p,'\n');if(nl)*nl=0;
    if(strstr(p,name)){unsigned long s=strtoul(p,0,16);char*d=strchr(p,'-');unsigned long e=d?strtoul(d+1,0,16):0;
      if(!base||s<base)base=s; if(e>end)end=e;}
    if(!nl)break;p=nl+1;}
  if(endp)*endp=end; return base;}
static int in_vu(unsigned long a){return a>=gv && a<gv_end;}
typedef int(*rd_fn)(void*,int,int*);
typedef int(*wr_fn)(void*,int,int);
static int aspect_read(void){
  void**obj=(void**)(gv+OBJ_OFF);
  unsigned long vt=(unsigned long)*obj;
  if(!in_vu(vt)){lg("aspect: vtable ptr out of range\n");return -777;}
  unsigned long m=*(unsigned long*)(vt+RD_VOFF);
  if(!in_vu(m&~1UL)){lg("aspect: read method out of range\n");return -777;}
  rd_fn f=(rd_fn)m; int out=-1; int r=f(obj,1,&out);
  char msg[80];snprintf(msg,sizeof msg,"aspect: READ ret=%d aspect=%d\n",r,out);lg(msg);return out;}
static void aspect_write(int val){
  void**obj=(void**)(gv+OBJ_OFF);
  unsigned long vt=(unsigned long)*obj;
  if(!in_vu(vt)){lg("aspect: (w)vtable out of range\n");return;}
  unsigned long m=*(unsigned long*)(vt+WR_VOFF);
  if(!in_vu(m&~1UL)){lg("aspect: write method out of range\n");return;}
  wr_fn f=(wr_fn)m; int r=f(obj,1,val);
  char msg[80];snprintf(msg,sizeof msg,"aspect: WRITE %d ret=%d\n",val,r);lg(msg);}
static void nap(int s){struct timespec t={s,0};nanosleep(&t,0);}
static void*w(void*a){(void)a;
  gv=find_base("viewUnified2.so",&gv_end);
  if(!gv){lg("aspect: vu base NOT found\n");return 0;}
  unsigned int guard=*(unsigned int*)(gv+GUARD_OFF);
  char m[96];snprintf(m,sizeof m,"aspect: vu=0x%lx..0x%lx guard=0x%x\n",gv,gv_end,guard);lg(m);
  if(!(guard&1)){lg("aspect: global NOT constructed -> abort\n");return 0;}
  lg("aspect: --- before (dump) ---\n"); int cur=aspect_read(); nap(3);
  if(cur<-100){lg("aspect: read failed -> abort\n");return 0;}
  int nv = (cur==2)?4:2;   /* toggle to a different valid aspect {2,3,4} */
  char m2[48];snprintf(m2,sizeof m2,"aspect: toggle %d -> %d\n",cur,nv);lg(m2);
  lg("aspect: --- WRITE newval (dump) ---\n"); aspect_write(nv); nap(1); aspect_read(); nap(4);
  lg("aspect: --- revert (dump) ---\n"); aspect_write(cur); nap(1); aspect_read(); nap(4);
  lg("aspect: done\n");return 0;}
__attribute__((constructor)) static void c(void){lg("aspect: ctor\n");pthread_t t;if(pthread_create(&t,0,w,0)==0)pthread_detach(t);}

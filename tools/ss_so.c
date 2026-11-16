/* ss_so.c - §17 observable SET via model/STILL_REC 4-param (sub_189ED4).
 * requestModelExecute("model/STILL_REC", 0x15, [0x12C=mode,0x12D,0x12F,0x27E])
 * Exact stock-valid self-shooting tuple (11,1,1,1); toggle 0x27E (self-shoot
 * flag) 1<->0. Read-back 0x12C via getModelInfo(model/CAMERA,15). RAM-only. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#define GETMI 0x39485CUL
#define PLC   0x70912EUL
#define PLADD 0x7095D6UL
#define REQ   0x3948E0UL
#define INTP  0x0EA468UL
#define PLGET 0x0EA4B0UL
static unsigned long gl,gv;
static void lg(const char*s){int fd=open("/android/a5k/ss.log",O_WRONLY|O_CREAT|O_APPEND,0644);if(fd>=0){(void)!write(fd,s,strlen(s));close(fd);}}
static unsigned long fb(const char*n){int fd=open("/proc/self/maps",O_RDONLY);if(fd<0)return 0;static char b[1<<20];int o=0,r;while(o<(int)sizeof b-1&&(r=read(fd,b+o,sizeof b-1-o))>0)o+=r;close(fd);if(o<=0)return 0;b[o]=0;unsigned long base=0;for(char*p=b;p;){char*nl=strchr(p,'\n');if(nl)*nl=0;if(strstr(p,n)&&strstr(p,"r-xp")){unsigned long s=strtoul(p,0,16);if(!base||s<base)base=s;}if(!nl)break;p=nl+1;}return base;}
typedef void*(*ctor_fn)(void*);
typedef void(*add_fn)(void*,unsigned long,void*);
typedef void*(*intp_fn)(void*,int);
typedef int(*req_fn)(const char*,unsigned long,void*);
typedef int(*gmi_fn)(const char*,unsigned long,void*,void*,void*);
typedef void*(*get_fn)(void*,unsigned int);
static void add_ip(void*list,unsigned long id,int val){
  intp_fn ip=(intp_fn)(gv+INTP+1); add_fn add=(add_fn)(gl+PLADD+1);
  void*p=malloc(0x10); ip(p,val); add(list,id,p);}
static void set_mode(int m,int d,int f,int ss){
  ctor_fn c=(ctor_fn)(gl+PLC+1); req_fn rq=(req_fn)(gl+REQ+1);
  void*list=malloc(8); c(list);
  add_ip(list,0x12C,m); add_ip(list,0x12D,d); add_ip(list,0x12F,f); add_ip(list,0x27E,ss);
  int r=rq("model/STILL_REC",0x15,list);
  char msg[96];snprintf(msg,sizeof msg,"ss: SET STILL_REC 0x15 [12C=%d 12D=%d 12F=%d 27E=%d] ret=%d\n",m,d,f,ss,r);lg(msg);}
static int rd12c(void){
  ctor_fn c=(ctor_fn)(gl+PLC+1); gmi_fn g=(gmi_fn)(gl+GETMI+1); get_fn pg=(get_fn)(gv+PLGET+1);
  void*o=malloc(0x20); c(o); int r=g("model/CAMERA",15,o,0,0); void*p=pg(o,0x12C);
  int v=p?*(int*)((char*)p+12):-888; char m[80];snprintf(m,sizeof m,"ss: read 0x12C ret=%d val=%d\n",r,v);lg(m);return v;}
static void nap(int s){struct timespec t={s,0};nanosleep(&t,0);}
static void*w(void*a){(void)a;gl=fb("libObj.so");gv=fb("viewUnified2.so");if(!gl||!gv){lg("ss: base NOT found\n");return 0;}
  char m[80];snprintf(m,sizeof m,"ss: libObj=0x%lx vu=0x%lx\n",gl,gv);lg(m);
  lg("ss: --- before ---\n"); rd12c(); nap(3);
  lg("ss: --- self-shoot ON (27E=1) ---\n"); set_mode(11,1,1,1); nap(1); rd12c(); nap(4);
  lg("ss: --- self-shoot OFF (27E=0) ---\n"); set_mode(11,1,1,0); nap(1); rd12c(); nap(4);
  lg("ss: done\n");return 0;}
__attribute__((constructor)) static void c(void){lg("ss: ctor\n");pthread_t t;if(pthread_create(&t,0,w,0)==0)pthread_detach(t);}

/* rd_so.c - §15.1 in-process engine-param READ: getModelInfo(model/CAMERA,15)
 * -> param 0x12C (shooting mode). selector is a small int (not PrmNumberList,
 * resolving §13.4 blocker1; disasm of sub_17F798: movs r1,#15). */
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
#define PLGET 0x0EA4B0UL
static unsigned long gl,gv;
static void lg(const char*s){int fd=open("/android/a5k/rd.log",O_WRONLY|O_CREAT|O_APPEND,0644);if(fd>=0){(void)!write(fd,s,strlen(s));close(fd);}}
static unsigned long fb(const char*n){int fd=open("/proc/self/maps",O_RDONLY);if(fd<0)return 0;static char b[1<<20];int o=0,r;while(o<(int)sizeof b-1&&(r=read(fd,b+o,sizeof b-1-o))>0)o+=r;close(fd);if(o<=0)return 0;b[o]=0;unsigned long base=0;for(char*p=b;p;){char*nl=strchr(p,'\n');if(nl)*nl=0;if(strstr(p,n)&&strstr(p,"r-xp")){unsigned long s=strtoul(p,0,16);if(!base||s<base)base=s;}if(!nl)break;p=nl+1;}return base;}
typedef void*(*ctor_fn)(void*);
typedef int(*gmi_fn)(const char*,unsigned long,void*,void*,void*);
typedef void*(*get_fn)(void*,unsigned int);
static int rdparam(unsigned long sel,unsigned int id){
  ctor_fn c=(ctor_fn)(gl+PLC+1);gmi_fn g=(gmi_fn)(gl+GETMI+1);get_fn pg=(get_fn)(gv+PLGET+1);
  void*o=malloc(0x20);c(o);int r=g("model/CAMERA",sel,o,0,0);void*p=pg(o,id);
  int v=p?*(int*)((char*)p+12):-888;char m[96];
  snprintf(m,sizeof m,"rd: getModelInfo(model/CAMERA,%lu) ret=%d param0x%x=%d\n",sel,r,id,v);lg(m);return v;}
static void*w(void*a){(void)a;gl=fb("libObj.so");gv=fb("viewUnified2.so");if(!gl||!gv){lg("rd: base NOT found\n");return 0;}
  char m[80];snprintf(m,sizeof m,"rd: libObj=0x%lx vu=0x%lx\n",gl,gv);lg(m);
  struct timespec t={1,0};
  for(int i=0;i<15;i++){rdparam(15,0x12C);nanosleep(&t,0);}  /* shooting mode */
  lg("rd: done\n");return 0;}
__attribute__((constructor)) static void c(void){lg("rd: ctor\n");pthread_t t;if(pthread_create(&t,0,w,0)==0)pthread_detach(t);}

/* keyfeed_so.c - EMULATOR-ONLY reproduction of real-device native key input.
 * The emulator has no native createEvent key-input model (TRACKB §19.4), so to
 * reproduce real-device input we call the (hooked) InputService::createEvent with
 * real-device ids. If a5kinproc is already injected, createEvent's entry is our
 * my_createEvent, so this feeds our hook/ring EXACTLY as the real sub-CPU would.
 * Interactive: worker loops reading /android/a5k/keycmd (space/newline-separated
 * hex ids); sends each via createEvent(0,id,4), logs, truncates the file. Write an
 * id (e.g. "0x15010111 0x15010112" = MENU press+release) to drive it.
 * RAM-only, reboot-recoverable. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <pthread.h>
#include <time.h>
#define OFF_CREATEEVENT 0x37F854UL
typedef void* (*ce_fn)(void*, unsigned int, unsigned char);
static unsigned long find_base(const char*n){int fd=open("/proc/self/maps",O_RDONLY);if(fd<0)return 0;static char b[1<<20];int o=0,r;while(o<(int)sizeof b-1&&(r=read(fd,b+o,sizeof b-1-o))>0)o+=r;close(fd);b[o]=0;unsigned long base=0;for(char*p=b;p;){char*nl=strchr(p,'\n');if(nl)*nl=0;if(strstr(p,n)&&strstr(p,"r-xp")){unsigned long s=strtoul(p,0,16);if(!base||s<base)base=s;}if(!nl)break;p=nl+1;}return base;}
static void lg(const char*s){int fd=open("/android/a5k/keyfeed.log",O_WRONLY|O_CREAT|O_APPEND,0644);if(fd>=0){(void)!write(fd,s,strlen(s));close(fd);}}
static void*worker(void*a){(void)a;
  unsigned long lib=find_base("libObj.so"); char m[160];
  snprintf(m,sizeof m,"keyfeed: libObj=0x%lx createEvent=0x%lx\n",lib,lib+OFF_CREATEEVENT); lg(m);
  if(!lib){lg("keyfeed: no libObj\n");return 0;}
  ce_fn ce=(ce_fn)((lib+OFF_CREATEEVENT)|1UL);
  for(;;){
    int fd=open("/android/a5k/keycmd",O_RDONLY);
    if(fd>=0){char buf[512];int n=read(fd,buf,sizeof buf-1);close(fd);
      if(n>0){buf[n]=0;
        /* parse + clear first so we don't resend */
        int tfd=open("/android/a5k/keycmd",O_WRONLY|O_TRUNC);if(tfd>=0)close(tfd);
        char*p=buf;while(*p){while(*p==' '||*p=='\n'||*p=='\t'||*p=='\r')p++;if(!*p)break;
          unsigned long id=strtoul(p,&p,0);
          if(id){ void*rv=ce((void*)0,(unsigned int)id,4);
            snprintf(m,sizeof m,"keyfeed: send id=0x%08lx -> ret=%s (%p)\n",id, rv?"PASS-TO-STOCK":"CONSUMED-NULL", rv); lg(m); }
          struct timespec d={0,120*1000000L};nanosleep(&d,0);
        }
      }
    }
    struct timespec t={0,100*1000000L};nanosleep(&t,0);
  }
  return 0;}
__attribute__((constructor)) static void ctor(void){pthread_t th;if(pthread_create(&th,0,worker,0)==0)pthread_detach(th);}

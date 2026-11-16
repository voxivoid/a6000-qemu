/* key_so.c - §21 live key reception in-process. In Diadem, event0 gets no keys;
 * the native UI receives them as view Events via InputService::createEvent
 * (libObj 0x37F854): e=operator new(0x10); Event::Event(e,id,type,1). We
 * REIMPLEMENT createEvent in our stub (record id + do the same) and patch its
 * entry with an absolute Thumb jump (LDR.W PC,[PC,#0]+target). Non-destructive:
 * stock input keeps working; we see every input event id. RAM-only, reversible. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <pthread.h>
#include <time.h>
#define CREATEEVENT_OFF 0x37F854UL
#define EVENTCTOR_OFF   0x7042D4UL
static unsigned long g_lib;
static unsigned long g_evctor;
static volatile unsigned g_ring[512];
static volatile unsigned g_head;
static unsigned char g_save[8];
static unsigned long g_target;
static int g_patched;
static void lg(const char*s){int fd=open("/android/a5k/key.log",O_WRONLY|O_CREAT|O_APPEND,0644);if(fd>=0){(void)!write(fd,s,strlen(s));close(fd);}}

typedef void (*evctor_fn)(void*, unsigned int, unsigned char, unsigned char);

/* our replacement for InputService::createEvent(this, id, type) */
__attribute__((used))
static void* my_createEvent(void* self, unsigned int id, unsigned char type){
  (void)self;
  g_ring[g_head++ & 511] = id;                 /* record every event id */
  void* e = malloc(0x10);
  evctor_fn ctor = (evctor_fn)(g_evctor);
  ctor(e, id, type, 1);
  return e;
}

static unsigned long find_base(const char*n){int fd=open("/proc/self/maps",O_RDONLY);if(fd<0)return 0;static char b[1<<20];int o=0,r;while(o<(int)sizeof b-1&&(r=read(fd,b+o,sizeof b-1-o))>0)o+=r;close(fd);if(o<=0)return 0;b[o]=0;unsigned long base=0;for(char*p=b;p;){char*nl=strchr(p,'\n');if(nl)*nl=0;if(strstr(p,n)&&strstr(p,"r-xp")){unsigned long s=strtoul(p,0,16);if(!base||s<base)base=s;}if(!nl)break;p=nl+1;}return base;}

static int patch(void){
  g_target = g_lib + CREATEEVENT_OFF;
  unsigned long page = g_target & ~0xfffUL;
  if (mprotect((void*)page, 0x2000, PROT_READ|PROT_WRITE|PROT_EXEC)) { lg("key: mprotect FAIL\n"); return -1; }
  memcpy(g_save, (void*)g_target, 8);
  unsigned char *t = (unsigned char*)g_target;
  /* LDR.W PC,[PC,#0] = F8DF F000 -> bytes DF F8 00 F0 ; then target|1 */
  t[0]=0xDF; t[1]=0xF8; t[2]=0x00; t[3]=0xF0;
  unsigned long tgt = ((unsigned long)&my_createEvent) | 1UL;
  memcpy(t+4, &tgt, 4);
  __builtin___clear_cache((char*)g_target, (char*)g_target+8);
  mprotect((void*)page, 0x2000, PROT_READ|PROT_EXEC);
  g_patched=1;
  char m[96]; snprintf(m,sizeof m,"key: patched createEvent @0x%lx -> my=0x%lx (evctor=0x%lx)\n", g_target, tgt, g_evctor); lg(m);
  return 0;
}
static void unpatch(void){
  if(!g_patched)return; unsigned long page=g_target&~0xfffUL;
  if(mprotect((void*)page,0x2000,PROT_READ|PROT_WRITE|PROT_EXEC)==0){
    memcpy((void*)g_target,g_save,8);
    __builtin___clear_cache((char*)g_target,(char*)g_target+8);
    mprotect((void*)page,0x2000,PROT_READ|PROT_EXEC);
    g_patched=0; lg("key: unpatched\n");
  }
}
static void nap_ms(long ms){struct timespec t={ms/1000,(ms%1000)*1000000L};nanosleep(&t,0);}
static void*worker(void*a){(void)a;
  g_lib=find_base("libObj.so"); if(!g_lib){lg("key: libObj NOT found\n");return 0;}
  g_evctor = (g_lib + EVENTCTOR_OFF) | 1UL;   /* Thumb */
  char m[80]; snprintf(m,sizeof m,"key: libObj=0x%lx\n",g_lib); lg(m);
  if(patch()!=0) return 0;
  unsigned tail=g_head;
  for(int i=0;i<300;i++){            /* ~30s: flush new ids */
    unsigned h=g_head;
    while(tail!=h){ unsigned id=g_ring[tail&511]; tail++;
      char b[48]; snprintf(b,sizeof b,"key: createEvent id=%u (0x%x)\n",id,id); lg(b);
    }
    nap_ms(100);
  }
  unpatch();
  lg("key: worker done\n");
  return 0;
}
__attribute__((constructor)) static void c(void){lg("key: ctor\n");pthread_t t;if(pthread_create(&t,0,worker,0)==0)pthread_detach(t);}

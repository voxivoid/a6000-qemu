/* ui_so.c - §21 live-operated in-process UI. Hooks InputService::createEvent
 * (libObj 0x37F854) to RECEIVE native key events (Diadem path; event0 is empty
 * in-process), suppresses stock draw (gate 2b: sub_66D442 0x66D442 -> return 0,
 * detach-reversible), and renders a key-driven menu overlay into the OSD double
 * buffer. MENU toggles the menu; UP/DOWN move the highlight; CENTER confirms.
 * key id = 0x15010000 | (code<<4) | edge(1=press). codes: CENTER 6 UP 7 DOWN 8
 * RIGHT 9 LEFT a MENU 0x11. RAM-only, reversible. */
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
#define DIRTY_OFF       0x66D442UL
#define OSD_PHYS 0x8f758000UL
#define STRIDE   0x500U
#define FB_BYTES (STRIDE*480U)
#define NBUF 2U
#define NITEMS 5
static unsigned long g_lib,g_evctor;
static volatile int g_menu, g_hl, g_confirm;
static unsigned char g_sv_ce[8], g_sv_dirty[4];
static unsigned long g_ce_t, g_dirty_t; static int g_ce_p,g_dirty_p;
static void lg(const char*s){int fd=open("/android/a5k/ui.log",O_WRONLY|O_CREAT|O_APPEND,0644);if(fd>=0){(void)!write(fd,s,strlen(s));close(fd);}}
typedef void(*evctor_fn)(void*,unsigned int,unsigned char,unsigned char);
__attribute__((used)) static void* my_createEvent(void* self,unsigned int id,unsigned char type){
  (void)self;
  if((id&0xFFFF0000u)==0x15010000u && (id&0xf)==1){     /* key press */
    unsigned code=(id>>4)&0xff;
    if(code==0x11) g_menu=!g_menu;                        /* MENU toggle */
    else if(g_menu && code==0x07){ if(g_hl>0)g_hl--; }    /* UP */
    else if(g_menu && code==0x08){ if(g_hl<NITEMS-1)g_hl++; } /* DOWN */
    else if(g_menu && code==0x06){ g_confirm=8; }         /* CENTER confirm (flash) */
  }
  void* e=malloc(0x10); ((evctor_fn)g_evctor)(e,id,type,1); return e;
}
static unsigned long find_base(const char*n){int fd=open("/proc/self/maps",O_RDONLY);if(fd<0)return 0;static char b[1<<20];int o=0,r;while(o<(int)sizeof b-1&&(r=read(fd,b+o,sizeof b-1-o))>0)o+=r;close(fd);if(o<=0)return 0;b[o]=0;unsigned long base=0;for(char*p=b;p;){char*nl=strchr(p,'\n');if(nl)*nl=0;if(strstr(p,n)&&strstr(p,"r-xp")){unsigned long s=strtoul(p,0,16);if(!base||s<base)base=s;}if(!nl)break;p=nl+1;}return base;}
static int patch_bytes(unsigned long addr,const unsigned char*nw,int n,unsigned char*save){
  unsigned long pg=addr&~0xfffUL;
  if(mprotect((void*)pg,0x2000,PROT_READ|PROT_WRITE|PROT_EXEC))return -1;
  memcpy(save,(void*)addr,n); memcpy((void*)addr,nw,n);
  __builtin___clear_cache((char*)addr,(char*)addr+n);
  mprotect((void*)pg,0x2000,PROT_READ|PROT_EXEC); return 0;
}
static void restore_bytes(unsigned long addr,const unsigned char*save,int n){
  unsigned long pg=addr&~0xfffUL;
  if(mprotect((void*)pg,0x2000,PROT_READ|PROT_WRITE|PROT_EXEC)==0){
    memcpy((void*)addr,save,n);__builtin___clear_cache((char*)addr,(char*)addr+n);
    mprotect((void*)pg,0x2000,PROT_READ|PROT_EXEC);}
}
static void fillrect(volatile uint8_t*fb,unsigned x,unsigned y,unsigned w,unsigned h,uint16_t c){
  for(unsigned b=0;b<NBUF;b++){volatile uint8_t*buf=fb+(unsigned long)b*FB_BYTES;
    for(unsigned yy=y;yy<y+h;yy++){volatile uint16_t*row=(volatile uint16_t*)(buf+yy*STRIDE+x*2);
      for(unsigned xx=0;xx<w;xx++)row[xx]=c;}}
}
static void render(volatile uint8_t*fb){
  /* clear a work area (black opaque) */
  fillrect(fb,80,60,360,360,0x000F);
  /* title bar: magenta when menu open, dim when closed */
  fillrect(fb,100,70,320,24, g_menu?0xF0FF:0x303F);
  if(g_menu){
    for(int i=0;i<NITEMS;i++){
      uint16_t col = (i==g_hl)?(g_confirm?0x0FFF:0xFFFF):0x00AF; /* hl=white/cyan else dim blue */
      col = (i==g_hl)?(g_confirm?0x0FFF:0xFF0F):0x00AF; /* hl: confirm=cyan else yellow; else dim blue */
      fillrect(fb,110,110+i*50,300,40,col);
    }
  }
}
static void nap_ms(long ms){struct timespec t={ms/1000,(ms%1000)*1000000L};nanosleep(&t,0);}
static void*worker(void*a){(void)a;
  g_lib=find_base("libObj.so"); if(!g_lib){lg("ui: libObj NOT found\n");return 0;}
  g_evctor=(g_lib+EVENTCTOR_OFF)|1UL;
  int fd=open("/dev/mem",O_RDWR); if(fd<0){lg("ui: /dev/mem FAIL\n");return 0;}
  unsigned long len=(NBUF*FB_BYTES+0xfffUL)&~0xfffUL;
  volatile uint8_t*fb=mmap(0,len,PROT_READ|PROT_WRITE,MAP_SHARED,fd,OSD_PHYS);
  if(fb==MAP_FAILED){lg("ui: mmap FAIL\n");close(fd);return 0;}
  /* key hook */
  g_ce_t=g_lib+CREATEEVENT_OFF; unsigned char jb[8]={0xDF,0xF8,0x00,0xF0,0,0,0,0};
  unsigned long tgt=((unsigned long)&my_createEvent)|1UL; memcpy(jb+4,&tgt,4);
  if(patch_bytes(g_ce_t,jb,8,g_sv_ce)==0)g_ce_p=1; else {lg("ui: ce patch FAIL\n");}
  /* stock draw suppress (gate 2b) */
  g_dirty_t=g_lib+DIRTY_OFF; unsigned char gb[4]={0x00,0x20,0x70,0x47}; /* movs r0,#0;bx lr */
  if(patch_bytes(g_dirty_t,gb,4,g_sv_dirty)==0)g_dirty_p=1; else {lg("ui: gate patch FAIL\n");}
  lg("ui: hooks installed; render loop\n");
  for(int i=0;i<600;i++){ if(g_confirm>0)g_confirm--; render(fb); nap_ms(50); }  /* ~30s */
  if(g_ce_p)restore_bytes(g_ce_t,g_sv_ce,8);
  if(g_dirty_p)restore_bytes(g_dirty_t,g_sv_dirty,4);
  lg("ui: hooks removed\n"); munmap((void*)fb,len); close(fd);
  return 0;}
__attribute__((constructor)) static void c(void){lg("ui: ctor\n");pthread_t t;if(pthread_create(&t,0,worker,0)==0)pthread_detach(t);}

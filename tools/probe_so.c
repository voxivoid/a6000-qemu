/* probe_so.c - LD_PRELOAD probe: log which boot process loaded us. */
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
static void w(const char*s,int n){int fd=open("/setting/a5k/probe.log",O_WRONLY|O_CREAT|O_APPEND,0644);if(fd>=0){(void)!write(fd,s,n);close(fd);}}
__attribute__((constructor)) static void c(void){
  char b[256]; int n=0;
  const char*p="LOADED pid="; while(*p)b[n++]=*p++;
  int pid=getpid(); char t[12]; int ti=0; if(pid==0)t[ti++]='0'; while(pid){t[ti++]='0'+pid%10;pid/=10;} while(ti)b[n++]=t[--ti];
  b[n++]=' '; p="comm="; while(*p)b[n++]=*p++;
  int fd=open("/proc/self/comm",O_RDONLY); if(fd>=0){int r=read(fd,b+n,64); if(r>0)n+=r; close(fd);} 
  b[n++]='\n'; w(b,n);
}

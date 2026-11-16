/*
 * auto_pay_so.c - Track B route (b) autostart *payload*.
 *
 * dlopen()'d into the running im.elf by a5kauto_so.c (the boot hook), with no
 * manual ptrace step.  Its constructor spawns a worker thread (so the hijacked
 * im.elf thread leaves dlopen at once and never blocks the engine) that maps
 * the panel-OSD double buffer through /dev/mem and keeps a solid YELLOW
 * rectangle refilled for ~40 s.  Same pixel path as tools/inj_so.c, but a
 * distinct colour + position + marker log prove the draw came from the
 * cold-boot autostart hook rather than a manual injection.
 *
 * Marker: writes "AUTOSTART payload drawing in im.elf pid=<pid>\n" to
 * /android/a5k/auto_pay.log and /setting/a5k/auto_pay.log.
 *
 * RAM-only: the mapping and draw touch no flash.  (The /setting log line is the
 * reversible rw tier, there only as persistent proof.)
 *
 * Build (glibc ARM, soft-float, docker image a5k-cross):
 *   arm-linux-gnueabi-gcc -std=gnu99 -O2 -fPIC -shared -fno-stack-protector \
 *     -o pay.so tools/auto_pay_so.c -lpthread
 */
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <pthread.h>
#include <time.h>

#define OSD_PHYS   0x8f758000UL      /* AIF ch53 +0x20 base, buffer 0        */
#define STRIDE     0x500U            /* 1280 bytes/line                      */
#define FB_BYTES   (STRIDE * 480U)   /* 0x96000                              */
#define NBUF       2U
#define RX 120U
#define RY 180U
#define RW 200U
#define RH 120U
#define PIX_YELLOW 0xFF0FU           /* RGBA4444 Rf Gf B0 Af -> (255,255,0)  */
#define FRAMES     4000U             /* ~40 s at 10 ms                        */

static void marker(void)
{
    char b[96]; snprintf(b, sizeof b, "AUTOSTART payload drawing in im.elf pid=%d\n", (int)getpid());
    const char *paths[] = { "/android/a5k/auto_pay.log", "/setting/a5k/auto_pay.log" };
    for (int i = 0; i < 2; i++) {
        int fd = open(paths[i], O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) { (void)!write(fd, b, strlen(b)); close(fd); }
    }
}

static void *worker(void *arg)
{
    (void)arg;
    marker();
    int fd = open("/dev/mem", O_RDWR);
    if (fd < 0) return 0;
    unsigned long len = (NBUF * FB_BYTES + 0xfffUL) & ~0xfffUL;
    volatile uint8_t *fb = mmap(0, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, OSD_PHYS);
    if (fb == MAP_FAILED) { close(fd); return 0; }

    struct timespec ts = { 0, 10 * 1000000L };
    for (unsigned f = 0; f < FRAMES; f++) {
        for (unsigned b = 0; b < NBUF; b++) {
            volatile uint8_t *buf = fb + (unsigned long)b * FB_BYTES;
            for (unsigned y = RY; y < RY + RH; y++) {
                volatile uint16_t *row = (volatile uint16_t *)(buf + y * STRIDE + RX * 2);
                for (unsigned x = 0; x < RW; x++) row[x] = PIX_YELLOW;
            }
        }
        nanosleep(&ts, 0);
    }
    munmap((void *)fb, len);
    close(fd);
    return 0;
}

__attribute__((constructor))
static void pay_ctor(void)
{
    pthread_t t;
    if (pthread_create(&t, 0, worker, 0) == 0) pthread_detach(t);
}

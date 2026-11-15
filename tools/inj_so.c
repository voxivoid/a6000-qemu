/*
 * inj_so.c - Track B route (b) payload: in-process OSD draw from inside im.elf.
 *
 * Built as a glibc ARM shared object and loaded into the running im.elf by the
 * ptrace injector (a5kinject) via __libc_dlopen_mode().  Its constructor runs
 * INSIDE im.elf's address space; it spawns a worker thread (so the hijacked
 * im.elf thread returns from dlopen at once, never blocking the engine) that
 * maps the panel-OSD double buffer through /dev/mem and blinks a solid BLUE
 * rectangle for ~9 s.  Same pixel path as tools/osdpoke.c, but the code now
 * executes in im.elf, proving the ptrace->dlopen->in-process-draw chain.
 *
 * Blue (0x00FF -> (0,0,255)) and a distinct position mark it apart from the
 * osdpoke (red/green) PoC.  RAM-only; the mapping and draw touch no flash.
 *
 * dlopen resolves open/mmap/pthread/nanosleep against im.elf's loaded libc +
 * libpthread, so no freestanding tricks are needed.
 *
 * Build (glibc ARM, soft-float, docker image a5k-cross):
 *   arm-linux-gnueabi-gcc -std=gnu99 -O2 -fPIC -shared -fno-stack-protector \
 *     -o inj.so tools/inj_so.c -lpthread
 * (needs only GLIBC_2.4 symbols -> loads on the guest's glibc 2.11.2)
 * Load it into im.elf with tools/a5kinject.c.
 */
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <pthread.h>
#include <time.h>

#define OSD_PHYS   0x8f758000UL      /* AIF ch53 +0x20 base, buffer 0        */
#define STRIDE     0x500U            /* 1280 bytes/line                      */
#define FB_BYTES   (STRIDE * 480U)   /* 0x96000                              */
#define NBUF       2U                /* buf0 0x8f758000, buf1 0x8f7ee000     */
#define RX 340U
#define RY 300U
#define RW 180U
#define RH 110U
#define PIX_BLUE   0x00FFU           /* RGBA4444 R0 G0 B F A F -> (0,0,255)  */
#define PIX_CYAN   0x0FFFU           /* R0 Gf Bf Af -> (0,255,255)           */
#define FRAMES     900U

static void logline(const char *s, unsigned n)
{
    int fd = open("/android/a5k/inj.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) { (void)!write(fd, s, n); close(fd); }
}

static void *worker(void *arg)
{
    (void)arg;
    int fd = open("/dev/mem", O_RDWR);
    if (fd < 0) { logline("inj: open /dev/mem FAIL\n", 24); return 0; }
    unsigned long len = (NBUF * FB_BYTES + 0xfffUL) & ~0xfffUL;
    volatile uint8_t *fb = mmap(0, len, PROT_READ | PROT_WRITE, MAP_SHARED,
                               fd, OSD_PHYS);
    if (fb == MAP_FAILED) { logline("inj: mmap FAIL\n", 15); close(fd); return 0; }
    logline("inj: worker mapped, drawing\n", 28);

    struct timespec ts = { 0, 10 * 1000000L };
    for (unsigned f = 0; f < FRAMES; f++) {
        uint16_t col = (f & 16) ? PIX_CYAN : PIX_BLUE;
        for (unsigned b = 0; b < NBUF; b++) {
            volatile uint8_t *buf = fb + (unsigned long)b * FB_BYTES;
            for (unsigned y = RY; y < RY + RH; y++) {
                volatile uint16_t *row =
                    (volatile uint16_t *)(buf + y * STRIDE + RX * 2);
                for (unsigned x = 0; x < RW; x++)
                    row[x] = col;
            }
        }
        nanosleep(&ts, 0);
    }
    logline("inj: worker done\n", 17);
    munmap((void *)fb, len);
    close(fd);
    return 0;
}

__attribute__((constructor))
static void inj_ctor(void)
{
    logline("inj: ctor in im.elf, spawning worker\n", 37);
    pthread_t t;
    if (pthread_create(&t, 0, worker, 0) == 0)
        pthread_detach(t);
    else
        logline("inj: pthread_create FAIL\n", 25);
    /* return immediately: the hijacked im.elf thread leaves dlopen at once */
}

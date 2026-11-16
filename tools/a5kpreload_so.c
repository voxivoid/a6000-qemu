/*
 * a5kpreload_so.c - Track B route (b) autostart hook, LD_PRELOAD variant.
 *
 * The lowest-risk boot-time persistence for route (b): NO ptrace.  Installed
 * exactly like a5kauto_so.c (two reversible /setting files, see that header):
 *
 *   /setting/mode/dmode   present (benign) -> init calls set_oreore_env()
 *   /setting/env.txt      LD_PRELOAD=/setting/a5k/a5kpreload.so
 *
 * EMPIRICAL FINDING (this session): the LD_PRELOAD that init putenv()s from
 * env.txt propagates all the way into im.elf itself -- im.elf's own glibc
 * loader maps this .so and runs our constructor (proof: probe.so appears in
 * /proc/<im.elf>/maps).  So we do not need to re-inject with ptrace at all:
 * the stock loader loads us into im.elf for free, every cold boot.
 *
 * The .so is preloaded into *every* init child (mount, busybox, bootin, ...),
 * so the constructor self-gates: it does nothing unless it is running inside
 * im.elf (comm == "im.elf").  Inside im.elf it spawns a detached worker that
 * waits for the panel OSD layer to come on, then keeps our rectangle refilled.
 * The engine critical path is untouched (the worker is a side thread; main
 * proceeds), so boot time stays within stock noise.
 *
 * Reversible: delete the two /setting files (and /setting/a5k) -> pure stock.
 * nflasha1/nflasha2 FAT, /usr, /system, rootfs, kernel untouched -> the USB
 * updater path stays reachable (RECOVERY_MAP.md invariant preserved).
 *
 * Build (glibc ARM, soft-float, docker image a5k-cross):
 *   arm-linux-gnueabi-gcc -std=gnu99 -O2 -fPIC -shared -fno-stack-protector \
 *     -o a5kpreload.so tools/a5kpreload_so.c -lpthread
 */
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <pthread.h>
#include <time.h>

#define OSD_PHYS   0x8f758000UL
#define STRIDE     0x500U
#define FB_BYTES   (STRIDE * 480U)
#define NBUF       2U
#define RX 360U
#define RY 120U
#define RW 180U
#define RH 110U
#define PIX_CYAN   0x0FFFU           /* RGBA4444 R0 Gf Bf Af -> (0,255,255)  */
#define FRAMES     6000U             /* ~60 s at 10 ms                       */

#define AIF_PHYS   0xF3440000UL
#define AIF_OSD_ON 0x144U
#define OSD_ON_BIT 0x2U

static void marker(void)
{
    char b[96]; snprintf(b, sizeof b, "PRELOAD payload drawing in im.elf pid=%d\n", (int)getpid());
    int fd = open("/setting/a5k/preload.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) { (void)!write(fd, b, strlen(b)); close(fd); }
}

static int osd_on(void)
{
    int fd = open("/dev/mem", O_RDONLY); if (fd < 0) return -1;
    volatile uint32_t *r = mmap(0, 0x1000, PROT_READ, MAP_SHARED, fd, AIF_PHYS);
    close(fd); if (r == MAP_FAILED) return -1;
    uint32_t v = r[AIF_OSD_ON / 4]; munmap((void *)r, 0x1000);
    return (v & OSD_ON_BIT) ? 1 : 0;
}

static void *worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < 1200; i++) {            /* wait up to ~120s for OSD on */
        int s = osd_on();
        if (s == 1) break;
        if (s < 0 && i > 100) break;
        struct timespec t = { 0, 100 * 1000000L }; nanosleep(&t, 0);
    }
    marker();
    int fd = open("/dev/mem", O_RDWR); if (fd < 0) return 0;
    unsigned long len = (NBUF * FB_BYTES + 0xfffUL) & ~0xfffUL;
    volatile uint8_t *fb = mmap(0, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, OSD_PHYS);
    if (fb == MAP_FAILED) { close(fd); return 0; }
    struct timespec ts = { 0, 10 * 1000000L };
    for (unsigned f = 0; f < FRAMES; f++) {
        for (unsigned b = 0; b < NBUF; b++) {
            volatile uint8_t *buf = fb + (unsigned long)b * FB_BYTES;
            for (unsigned y = RY; y < RY + RH; y++) {
                volatile uint16_t *row = (volatile uint16_t *)(buf + y * STRIDE + RX * 2);
                for (unsigned x = 0; x < RW; x++) row[x] = PIX_CYAN;
            }
        }
        nanosleep(&ts, 0);
    }
    munmap((void *)fb, len); close(fd);
    return 0;
}

static void comm_of_self(char *o, int n)
{
    o[0] = 0; int fd = open("/proc/self/comm", O_RDONLY); if (fd < 0) return;
    int r = read(fd, o, n - 1); close(fd); if (r < 0) r = 0;
    while (r && (o[r-1] == '\n' || o[r-1] == ' ')) r--; o[r] = 0;
}

__attribute__((constructor))
static void a5kpreload_ctor(void)
{
    char comm[32]; comm_of_self(comm, sizeof comm);
    if (strcmp(comm, "im.elf") != 0) return;    /* inert everywhere but im.elf */
    pthread_t t;
    if (pthread_create(&t, 0, worker, 0) == 0) pthread_detach(t);
}

/*
 * gate_so.c - Track B route (b) §15-A: REVERSIBLE stock-draw suppression (hook 2b, highest fidelity).
 *
 * Loaded into im.elf by a5kinject.  Unlike §12 (which no-op'd the downstream
 * vdf_if_port_send_update and deadlocked the flip handshake), this patches the
 * UPSTREAM appFw draw function
 *     sub_66D442 (libObj 0x66D442, Thumb, dirty-count test) -> return 0
 * so sub_4EFADE always takes the stock idle "nothing to draw" else-branch
 * (the very path stock runs every idle frame); GL draw throttle stays intact
 * (no busy-spin, unlike hook 2a sub_70CD64->BX LR).
 * No lock is held across the gate -> pipeline rests at the last completed frame
 * -> un-patching resumes the stock UI (detach-reversible).  (TRACKB §14.2)
 *
 * Worker:  draw a YELLOW rect once into the OSD double buffer, apply the gate,
 * hold HOLD_S s (measurement window) refilling the rect so it persists, then
 * UNPATCH so the stock UI resumes -> the key detach-reversibility test.
 *
 * In-process text patch (mprotect RW / __clear_cache / RX).  libObj base from
 * /proc/self/maps (per-boot).  No flash writes; reboot fully restores.
 *
 * Build: arm-linux-gnueabi-gcc -std=gnu99 -O2 -fPIC -shared
 *          -fno-stack-protector -o gate2.so tools/gate2_so.c -lpthread
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <pthread.h>
#include <time.h>

#define OSD_PHYS   0x8f758000UL
#define STRIDE     0x500U
#define FB_BYTES   (STRIDE * 480U)
#define NBUF       2U
#define RX 150U
#define RY 300U
#define RW 180U
#define RH 110U
#define PIX_YELLOW 0x0FFFU            /* cyan (0,255,255) */

#define DRAW_OFF 0x66D442UL           /* sub_66D442 dirty-count test (Thumb) */
#define HOLD_S   22

static unsigned long g_target;
static unsigned char g_save[4];
static int g_patched;

static void logline(const char *s)
{
    int fd = open("/android/a5k/gate2.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) { (void)!write(fd, s, strlen(s)); close(fd); }
}

static unsigned long find_base(const char *name)
{
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0) return 0;
    static char buf[1 << 20];
    int off = 0, r;
    while (off < (int)sizeof buf - 1 &&
           (r = read(fd, buf + off, sizeof buf - 1 - off)) > 0) off += r;
    close(fd);
    if (off <= 0) return 0;
    buf[off] = 0;
    unsigned long base = 0;
    for (char *p = buf; p; ) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        if (strstr(p, name) && strstr(p, "r-xp")) {
            unsigned long s = strtoul(p, 0, 16);
            if (!base || s < base) base = s;
        }
        if (!nl) break;
        p = nl + 1;
    }
    return base;
}

static int patch_gate(void)
{
    unsigned long base = find_base("libObj.so");
    if (!base) { logline("gate2: libObj base NOT found\n"); return -1; }
    g_target = base + DRAW_OFF;
    unsigned long page = g_target & ~0xfffUL;
    if (mprotect((void *)page, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC)) {
        logline("gate2: mprotect RW FAIL\n"); return -1;
    }
    unsigned char *t = (unsigned char *)g_target;
    g_save[0]=t[0]; g_save[1]=t[1]; g_save[2]=t[2]; g_save[3]=t[3];
    t[0]=0x00; t[1]=0x20; t[2]=0x70; t[3]=0x47;    /* movs r0,#0 ; bx lr */
    __builtin___clear_cache((char *)g_target, (char *)g_target + 4);
    mprotect((void *)page, 0x2000, PROT_READ | PROT_EXEC);
    g_patched = 1;
    { char m[96]; snprintf(m, sizeof m,
        "gate2: patched draw @0x%lx (libObj 0x%lx) save=%02x%02x\n",
        g_target, base, g_save[0], g_save[1]); logline(m); }
    return 0;
}

static void unpatch_gate(void)
{
    if (!g_patched) return;
    unsigned long page = g_target & ~0xfffUL;
    if (mprotect((void *)page, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
        unsigned char *t = (unsigned char *)g_target;
        t[0]=g_save[0]; t[1]=g_save[1]; t[2]=g_save[2]; t[3]=g_save[3];
        __builtin___clear_cache((char *)g_target, (char *)g_target + 4);
        mprotect((void *)page, 0x2000, PROT_READ | PROT_EXEC);
        g_patched = 0;
        logline("gate2: UNPATCHED (stock UI should resume)\n");
    } else logline("gate2: unpatch mprotect FAIL\n");
}

static void draw_rect(volatile uint8_t *fb)
{
    for (unsigned b = 0; b < NBUF; b++) {
        volatile uint8_t *buf = fb + (unsigned long)b * FB_BYTES;
        for (unsigned y = RY; y < RY + RH; y++) {
            volatile uint16_t *row =
                (volatile uint16_t *)(buf + y * STRIDE + RX * 2);
            for (unsigned x = 0; x < RW; x++) row[x] = PIX_YELLOW;
        }
    }
}

static void *worker(void *arg)
{
    (void)arg;
    int fd = open("/dev/mem", O_RDWR);
    if (fd < 0) { logline("gate2: /dev/mem FAIL\n"); return 0; }
    unsigned long len = (NBUF * FB_BYTES + 0xfffUL) & ~0xfffUL;
    volatile uint8_t *fb = mmap(0, len, PROT_READ | PROT_WRITE, MAP_SHARED,
                               fd, OSD_PHYS);
    if (fb == MAP_FAILED) { logline("gate2: mmap FAIL\n"); close(fd); return 0; }

    draw_rect(fb);
    if (patch_gate() != 0) { munmap((void *)fb, len); close(fd); return 0; }
    logline("gate2: held, refilling rect\n");
    struct timespec ts = { 0, 200 * 1000000L };   /* 200 ms */
    for (int i = 0; i < HOLD_S * 5; i++) { draw_rect(fb); nanosleep(&ts, 0); }

    unpatch_gate();
    struct timespec s1 = { 1, 0 };
    for (int i = 0; i < 4; i++) nanosleep(&s1, 0);  /* let UI resume */
    munmap((void *)fb, len); close(fd);
    logline("gate2: worker done\n");
    return 0;
}

__attribute__((constructor))
static void gate2_ctor(void)
{
    logline("gate2: ctor in im.elf\n");
    pthread_t t;
    if (pthread_create(&t, 0, worker, 0) == 0) pthread_detach(t);
    else logline("gate2: pthread_create FAIL\n");
}

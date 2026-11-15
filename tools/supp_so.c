/*
 * supp_so.c - Track B route (b) §4 experiment: stock OSD-draw suppression +
 *             our OSD persistence, from inside im.elf.  RAM-only, reversible.
 *
 * Loaded into the running im.elf by a5kinject.  Its worker:
 *   1. maps the panel-OSD double buffer via /dev/mem and draws a MAGENTA
 *      rectangle ONCE into both buffers,
 *   2. patches libObj's vdf_if_port_send_update (sub_5DC3D4, Thumb, offset
 *      0x5DC3D4; the sole OSD->VDF submit chokepoint, RE_DISPLAY_SCANOUT §5)
 *      to `BX LR` so no new OSD record ever reaches VPH -> the scanout freezes
 *      on the buffer we wrote, so our rectangle PERSISTS with no refill,
 *   3. holds the patch for HOLD_S seconds (measurement window) WITHOUT redraw,
 *   4. restores the original bytes.
 *
 * MEASURED (emulator, firmware 1.10): baseline THREAD_INFRA0 ~22% +
 * Appframework ~19% -> both drop BELOW 0.3% while patched (total im.elf
 * ~42% -> ~0.5%), and the magenta rectangle persists with NO refill ->
 * stock OSD draw fully suppressed, CPU fully recovered, our OSD persists.
 * Engine/live-view/input stay healthy (SYSV frame clock advances, no oops).
 *
 * CAVEAT: restoring the bytes does NOT resume the stock UI.  No-op'ing the
 * flip starves the OSD flip/semaphore handshake (osal_wai_sem_tmo inside
 * send_update), so the appFw/INFRA0 compose threads block permanently once
 * the first flip is dropped mid-frame; un-patching cannot replay the missing
 * flip.  It is thus a FREEZE (great for CPU/persistence) but is only
 * REBOOT-recoverable at runtime, not detach-reversible.  A detach-reversible
 * production hook must gate the appFw redraw TRIGGER upstream (idle, not
 * starve) -- next step, needs appFw.so RE (no decompile yet).
 *
 * The patch is on libObj's own text pages (mprotect to RW, restore RX,
 * __clear_cache).  libObj base is read live from /proc/self/maps (varies per
 * boot).  No flash writes; a reboot fully restores stock (RAM-only).
 *
 * Build: arm-linux-gnueabi-gcc -std=gnu99 -O2 -fPIC -shared
 *          -fno-stack-protector -o supp.so tools/supp_so.c -lpthread
 */
#include <stdint.h>
#include <stddef.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <pthread.h>
#include <time.h>

#define OSD_PHYS  0x8f758000UL
#define STRIDE    0x500U
#define FB_BYTES  (STRIDE * 480U)
#define NBUF      2U
#define RX 360U
#define RY 300U
#define RW 180U
#define RH 110U
#define PIX_MAGENTA 0xF0FFU     /* R=F G=0 B=F A=F -> (255,0,255) */

#define SEND_UPDATE_OFF 0x5DC3D4UL   /* libObj vdf_if_port_send_update (Thumb) */
#define HOLD_S 22

static unsigned long g_target;
static unsigned char g_save[2];
static int g_patched;

static void logline(const char *s)
{
    int fd = open("/android/a5k/supp.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) { (void)!write(fd, s, strlen(s)); close(fd); }
}

/* lowest r-xp base of a mapping whose path contains `name` */
static unsigned long find_base(const char *name)
{
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0) return 0;
    static char buf[1 << 20];           /* maps of a 158-thread process is large */
    int off = 0, r;
    while (off < (int)sizeof buf - 1 &&
           (r = read(fd, buf + off, sizeof buf - 1 - off)) > 0)
        off += r;
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

static int patch_send_update(void)
{
    unsigned long base = find_base("libObj.so");
    if (!base) { logline("supp: libObj base NOT found\n"); return -1; }
    g_target = base + SEND_UPDATE_OFF;
    unsigned long page = g_target & ~0xfffUL;
    if (mprotect((void *)page, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC)) {
        logline("supp: mprotect RW FAIL\n"); return -1;
    }
    unsigned char *t = (unsigned char *)g_target;
    g_save[0] = t[0]; g_save[1] = t[1];
    t[0] = 0x70; t[1] = 0x47;                  /* Thumb: BX LR (0x4770) */
    __builtin___clear_cache((char *)g_target, (char *)g_target + 2);
    mprotect((void *)page, 0x2000, PROT_READ | PROT_EXEC);
    g_patched = 1;
    { char m[96]; snprintf(m, sizeof m,
        "supp: patched send_update @0x%lx (libObj 0x%lx) save=%02x%02x\n",
        g_target, base, g_save[0], g_save[1]); logline(m); }
    return 0;
}

static void unpatch_send_update(void)
{
    if (!g_patched) return;
    unsigned long page = g_target & ~0xfffUL;
    if (mprotect((void *)page, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
        unsigned char *t = (unsigned char *)g_target;
        t[0] = g_save[0]; t[1] = g_save[1];
        __builtin___clear_cache((char *)g_target, (char *)g_target + 2);
        mprotect((void *)page, 0x2000, PROT_READ | PROT_EXEC);
        g_patched = 0;
        logline("supp: unpatched (stock UI resumes)\n");
    } else {
        logline("supp: unpatch mprotect FAIL\n");
    }
}

static void draw_rect_once(volatile uint8_t *fb)
{
    for (unsigned b = 0; b < NBUF; b++) {
        volatile uint8_t *buf = fb + (unsigned long)b * FB_BYTES;
        for (unsigned y = RY; y < RY + RH; y++) {
            volatile uint16_t *row =
                (volatile uint16_t *)(buf + y * STRIDE + RX * 2);
            for (unsigned x = 0; x < RW; x++) row[x] = PIX_MAGENTA;
        }
    }
}

static void *worker(void *arg)
{
    (void)arg;
    int fd = open("/dev/mem", O_RDWR);
    if (fd < 0) { logline("supp: /dev/mem FAIL\n"); return 0; }
    unsigned long len = (NBUF * FB_BYTES + 0xfffUL) & ~0xfffUL;
    volatile uint8_t *fb = mmap(0, len, PROT_READ | PROT_WRITE, MAP_SHARED,
                               fd, OSD_PHYS);
    if (fb == MAP_FAILED) { logline("supp: mmap FAIL\n"); close(fd); return 0; }

    draw_rect_once(fb);                 /* our OSD, once */
    logline("supp: rect drawn, patching\n");
    patch_send_update();                /* freeze stock OSD submit */
    draw_rect_once(fb);                 /* ensure it is in the frozen buffer */

    struct timespec ts = { 1, 0 };
    for (int i = 0; i < HOLD_S; i++) nanosleep(&ts, 0);   /* NO refill */

    unpatch_send_update();              /* reversible: stock resumes */
    nanosleep(&ts, 0); nanosleep(&ts, 0);
    munmap((void *)fb, len); close(fd);
    logline("supp: worker done\n");
    return 0;
}

__attribute__((constructor))
static void supp_ctor(void)
{
    logline("supp: ctor in im.elf\n");
    pthread_t t;
    if (pthread_create(&t, 0, worker, 0) == 0) pthread_detach(t);
    else logline("supp: pthread_create FAIL\n");
}

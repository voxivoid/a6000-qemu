/*
 * osdpoke - in-guest userspace OSD poke PoC (Track B route (b) pixel path).
 *
 * Opens /dev/mem, mmap2()s the live panel-OSD double buffer (AIF ch53 +0x20
 * base = 0x8f758000 / 0x8f7ee000, 16bpp RGBA4444, stride 0x500) and repeatedly
 * fills a solid rectangle in BOTH buffers so the VPH scanout DMA shows it on
 * the panel (QEMU graphic console 0).  Proves: a plain memory write into the
 * OSD framebuffer reaches the screen with NO Rigel/VPH commit or flush call
 * (the AIF channel re-reads the buffer every vsync).  This is the pixel-path
 * foundation of Track B route (b) (app/docs/TRACKB_INPROCESS.md §10).
 *
 * Freestanding (no libc): raw ARM-EABI syscalls, so it runs on the stock
 * glibc userland with no toolchain dependency.  It blinks the rectangle
 * red/green for ~9 s (DURATION_FRAMES frames at ~10 ms) so a screendump
 * reliably catches it against the stock UI's periodic redraw; when it exits,
 * the stock OSD reclaims the region (write was RAM-only, additive).
 *
 * Build (host):
 *   arm-none-eabi-gcc -O2 -marm -march=armv7-a -mfloat-abi=soft \
 *     -nostdlib -nostartfiles -ffreestanding -fno-stack-protector \
 *     -Wl,-e,_start -o osdpoke tools/osdpoke.c
 * Run (emulator, shooting screen up):
 *   tools/a5kshell.py --port <P> push osdpoke /android/a5k/osdpoke
 *   tools/a5kshell.py --port <P> run '/android/a5k/osdpoke &'
 *   tools/mon.py <MON> 'screendump out.ppm vph 0'
 *
 * Constants were baked after reading the live VPH registers over the monitor
 * (xp /1wx 0xF3441320 = base, 0xF3441324 = bytes/line, 0xF3440404 = bpp).
 */

#define SYS_exit      1
#define SYS_write     4
#define SYS_open      5
#define SYS_close     6
#define SYS_mmap2     192
#define SYS_nanosleep 162

static inline int sc3(int n, long a, long b, long c)
{
    register long r7 __asm__("r7") = n;
    register long r0 __asm__("r0") = a;
    register long r1 __asm__("r1") = b;
    register long r2 __asm__("r2") = c;
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r7), "r"(r1), "r"(r2) : "memory");
    return r0;
}
static inline long sc6(int n, long a, long b, long c, long d, long e, long f)
{
    register long r7 __asm__("r7") = n;
    register long r0 __asm__("r0") = a;
    register long r1 __asm__("r1") = b;
    register long r2 __asm__("r2") = c;
    register long r3 __asm__("r3") = d;
    register long r4 __asm__("r4") = e;
    register long r5 __asm__("r5") = f;
    __asm__ volatile("svc #0" : "+r"(r0)
                     : "r"(r7), "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5)
                     : "memory");
    return r0;
}

/* ---- baked constants (confirmed live before build) ----
 * Rigel double-buffers the panel OSD between 0x8f758000 and 0x8f7ee000
 * (back-to-back, each 0x96000 B). The AIF ch53 +0x20 base flips each flip,
 * so we poke BOTH buffers every frame. 0x8f7ee000-0x8f758000 == 0x96000.  */
#define OSD_PHYS    0x8f758000u     /* AIF ch53 +0x20 panel-OSD base buf0  */
#define STRIDE      0x500u          /* AIF ch53 +0x24 bytes per line (1280)*/
#define FB_BYTES    (STRIDE * 480u) /* 0x96000 = 614400                    */
#define NBUF        2u              /* buf0 @0x8f758000, buf1 @0x8f7ee000  */
#define RECT_X      120u
#define RECT_Y      80u
#define RECT_W      200u
#define RECT_H      120u
#define PIX_RED     0xF00Fu         /* RGBA4444 R=F G=0 B=0 A=F (opaque)   */
#define PIX_GREEN   0x0F0Fu         /* R=0 G=F B=0 A=F                     */
#define DURATION_FRAMES 900u        /* ~15 s at 16 ms                      */

static void nap_ms(long ms)
{
    long ts[2];               /* { tv_sec, tv_nsec } */
    ts[0] = ms / 1000;
    ts[1] = (ms % 1000) * 1000000L;
    sc3(SYS_nanosleep, (long)ts, 0, 0);
}

void _start(void)
{
    static const char dev[] = "/dev/mem";
    int fd = sc3(SYS_open, (long)dev, 2 /*O_RDWR*/, 0);
    if (fd < 0) {
        static const char e1[] = "osdpoke: open /dev/mem failed\n";
        sc3(SYS_write, 2, (long)e1, sizeof(e1) - 1);
        sc3(SYS_exit, 1, 0, 0);
    }

    /* mmap2 pgoffset is in 4096-byte units; cover both buffers (contiguous) */
    unsigned long map_len = (NBUF * FB_BYTES + 0xfffu) & ~0xfffu;
    long p = sc6(SYS_mmap2, 0, map_len, 3 /*RW*/, 1 /*MAP_SHARED*/, fd,
                 OSD_PHYS >> 12);
    if (p == -1L || (p >= -4095L && p < 0)) {
        static const char e2[] = "osdpoke: mmap2 failed\n";
        sc3(SYS_write, 2, (long)e2, sizeof(e2) - 1);
        sc3(SYS_exit, 2, 0, 0);
    }
    volatile unsigned char *fb = (volatile unsigned char *)p;

    static const char ok[] = "osdpoke: mapped, filling rectangle\n";
    sc3(SYS_write, 1, (long)ok, sizeof(ok) - 1);

    for (unsigned f = 0; f < DURATION_FRAMES; f++) {
        unsigned short col = (f & 16) ? PIX_GREEN : PIX_RED; /* blink R/G ~0.5s */
        for (unsigned b = 0; b < NBUF; b++) {
            volatile unsigned char *buf = fb + b * FB_BYTES;
            for (unsigned y = RECT_Y; y < RECT_Y + RECT_H; y++) {
                volatile unsigned short *row =
                    (volatile unsigned short *)(buf + y * STRIDE + RECT_X * 2);
                for (unsigned x = 0; x < RECT_W; x++) {
                    row[x] = col;
                }
            }
        }
        nap_ms(10);
    }

    static const char done[] = "osdpoke: done\n";
    sc3(SYS_write, 1, (long)done, sizeof(done) - 1);
    sc3(SYS_close, fd, 0, 0);
    sc3(SYS_exit, 0, 0, 0);
}

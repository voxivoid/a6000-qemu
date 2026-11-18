/*
 * gammamod - on-camera custom-look tester for the A6000 (BIONZ X / av-cam).
 *
 * RAM-only: reads and writes av-cam's develop tables in DDR through /dev/mem.
 * Nothing is written to flash.  av-cam's EC refills its working gamma from a
 * read-only copy within ~20 ms, so `hold` re-writes continuously while you
 * shoot; stop it (or pull the battery) and the stock curve returns.  This is
 * the A6000 analogue of ivica00/bionz-x-research's arri_isp /dev/mem pokes,
 * targeting the A6000's BASE develop gamma/matrix (the A6000 has no S-Log
 * slot).  For a camera you own; validate the TOOL in the emulator first, then
 * discover the live table on the camera by content, not a fixed address.
 *
 *   gammamod scan <pa_hex> <len_hex> [minpts]   find gamma-LUT tables in a
 *                                               DDR window (content heuristic)
 *   gammamod read  <pa_hex> <len_hex> <file>    save a region (keep originals!)
 *   gammamod write <pa_hex> <file>              write a region once
 *   gammamod hold  <pa_hex> <file> <ms>         write every ~8 ms for <ms>
 *
 * av-cam loads raw-contiguous at PA 0x83541000, so a table at av-cam.bin file
 * offset F is at PA 0x83541000+F (proven in the emulator).  By default PAs are
 * bounded to the av-cam DDR window to limit a mistake; -F lifts the bound.
 *
 * Build (freestanding guest binary, like a6kcopy / pploader):
 *   arm-none-eabi-gcc -march=armv7-a -marm -O2 -nostdlib -static \
 *       -ffreestanding -Wl,-Ttext-segment=0x10000 -o mods/look/gammamod \
 *       mods/look/gammamod.c
 */
typedef unsigned long ul;
typedef unsigned short u16;

static long sc(long n, long a, long b, long c, long d, long e)
{
    register long r0 asm("r0") = a, r1 asm("r1") = b, r2 asm("r2") = c,
                  r3 asm("r3") = d, r4 asm("r4") = e, r7 asm("r7") = n;
    asm volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3), "r"(r4),
                 "r"(r7) : "memory");
    return r0;
}
#define SYS_exit 1
#define SYS_read 3
#define SYS_write 4
#define SYS_open 5
#define SYS_close 6
#define SYS_llseek 140
#define SYS_nanosleep 162

/* av-cam DDR window (load base .. generous end); PAs are bounded here unless -F */
#define AVCAM_LO 0x83541000UL
#define AVCAM_HI 0x84200000UL

static ul slen(const char *s) { ul n = 0; while (s[n]) n++; return n; }
static void puts1(const char *s) { sc(SYS_write, 1, (long)s, slen(s), 0, 0); }
static void die(const char *s) { puts1("gammamod: "); puts1(s); puts1("\n"); sc(SYS_exit, 1, 0, 0, 0, 0); }
static void puthex(ul v)
{
    char b[11]; b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 8; i++) b[2 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15];
    b[10] = 0; puts1(b);
}
static void putdec(ul v)
{
    char t[12]; int n = 0; if (!v) t[n++] = '0';
    while (v) { t[n++] = '0' + v % 10; v /= 10; }
    char b[13]; int k = 0; while (n) b[k++] = t[--n]; b[k] = 0; puts1(b);
}
static ul hex(const char *s)
{
    ul v = 0; if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (; *s; s++) { int c = *s, d = c >= '0' && c <= '9' ? c - '0' :
        c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) break; v = v * 16 + d; }
    return v;
}
static ul udec(const char *s) { ul v = 0; for (; *s >= '0' && *s <= '9'; s++) v = v * 10 + (*s - '0'); return v; }
static int streq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }

static long g_mem = -1;
static int g_force = 0;
static long memfd(void)
{
    if (g_mem < 0) { g_mem = sc(SYS_open, (long)"/dev/mem", 2 /*O_RDWR*/, 0, 0, 0);
        if (g_mem < 0) die("open /dev/mem (root? CONFIG_STRICT_DEVMEM?)"); }
    return g_mem;
}
static void seek(ul pa)
{
    long off;
    if (sc(SYS_llseek, memfd(), (long)(pa >> 32), (long)(pa & 0xffffffff), (long)&off, 0) < 0)
        die("llseek /dev/mem");
}
static void guard(ul pa, ul len)
{
    if (g_force) return;
    if (pa < AVCAM_LO || pa + len > AVCAM_HI)
        die("PA outside av-cam window (use -F to override; only if you mean it)");
}
static long rd(ul pa, void *buf, ul len) { seek(pa); return sc(SYS_read, memfd(), (long)buf, len, 0, 0); }
static long wr(ul pa, const void *buf, ul len) { seek(pa); return sc(SYS_write, memfd(), (long)buf, len, 0, 0); }

static void nap_ms(long ms) { struct { long s, ns; } t = { ms / 1000, (ms % 1000) * 1000000L }; sc(SYS_nanosleep, (long)&t, 0, 0, 0, 0); }

static long open_rd(const char *p) { long fd = sc(SYS_open, (long)p, 0, 0, 0, 0); if (fd < 0) die("open file"); return fd; }
static long open_wr(const char *p) { long fd = sc(SYS_open, (long)p, 1 | 0100 | 01000 /*O_WRONLY|O_CREAT|O_TRUNC*/, 0644, 0, 0); if (fd < 0) die("create file"); return fd; }
static ul read_all(const char *p, char *buf, ul max)
{
    long fd = open_rd(p); ul n = 0; long r;
    while (n < max && (r = sc(SYS_read, fd, (long)(buf + n), max - n, 0, 0)) > 0) n += r;
    sc(SYS_close, fd, 0, 0, 0, 0); return n;
}

static char g_buf[1 << 20];     /* 1 MiB working buffer */

/* scan a DDR window for monotone-nondecreasing u16 gamma ramps (0 -> >=1024) */
static void cmd_scan(ul pa, ul len, int minpts)
{
    if (!minpts) minpts = 96;
    puts1("gammamod: scan "); puthex(pa); puts1(" + "); puthex(len);
    puts1(" minpts "); putdec(minpts); puts1("\n");
    ul done = 0, hits = 0;
    while (done < len) {
        ul chunk = len - done; if (chunk > sizeof g_buf) chunk = sizeof g_buf;
        long got = rd(pa + done, g_buf, chunk);
        if (got <= 0) { done += 4096; continue; }       /* skip unreadable page */
        u16 *w = (u16 *)g_buf; long cnt = got / 2;
        for (long i = 0; i < cnt - minpts; ) {
            long prev = -1, run = 0, j = i;
            while (j < cnt && w[j] >= prev) { prev = w[j]; run++; j++; }
            if (run >= minpts) {
                u16 a = w[i], b = w[i + run - 1];
                int distinct = (w[i] != w[i+1]) + (w[i+1] != w[i+2]) + (w[i+2] != w[i+3]);
                if (a <= 64 && b >= 1024 && (ul)(b - a) >= 1000 && distinct >= 2) {
                    hits++;
                    puts1("  PA "); puthex(pa + done + i * 2);
                    puts1("  len "); putdec(run);
                    puts1("  "); putdec(a); puts1(".."); putdec(b); puts1("\n");
                }
                i += run;
            } else i++;
        }
        done += chunk;
    }
    puts1("gammamod: "); putdec(hits); puts1(" candidates\n");
}

static void cmd_read(ul pa, ul len, const char *file)
{
    guard(pa, len);
    long fd = open_wr(file); ul done = 0;
    while (done < len) {
        ul chunk = len - done; if (chunk > sizeof g_buf) chunk = sizeof g_buf;
        long got = rd(pa + done, g_buf, chunk);
        if (got <= 0) die("read /dev/mem");
        if (sc(SYS_write, fd, (long)g_buf, got, 0, 0) != got) die("write file");
        done += got;
    }
    sc(SYS_close, fd, 0, 0, 0, 0);
    puts1("gammamod: saved "); putdec(len); puts1(" bytes from "); puthex(pa); puts1("\n");
}

static void cmd_write(ul pa, const char *file, long hold_ms)
{
    ul n = read_all(file, g_buf, sizeof g_buf);
    if (!n) die("empty curve file");
    guard(pa, n);
    if (hold_ms <= 0) {
        if (wr(pa, g_buf, n) != (long)n) die("write /dev/mem");
        puts1("gammamod: wrote "); putdec(n); puts1(" bytes to "); puthex(pa); puts1("\n");
        return;
    }
    puts1("gammamod: holding "); putdec(n); puts1(" bytes at "); puthex(pa);
    puts1(" for "); putdec(hold_ms); puts1(" ms (Ctrl-C/stop = revert)\n");
    long elapsed = 0;
    while (elapsed < hold_ms) { wr(pa, g_buf, n); nap_ms(8); elapsed += 8; }
    puts1("gammamod: hold done; stock curve returns on next EC refill\n");
}

void cmain(int argc, char **argv)
{
    int a = 1;
    if (a < argc && streq(argv[a], "-F")) { g_force = 1; a++; }
    if (a >= argc) die("usage: [-F] scan|read|write|hold ARGS (see header)");
    const char *op = argv[a++];
    if (streq(op, "scan") && argc - a >= 2)
        cmd_scan(hex(argv[a]), hex(argv[a + 1]), argc - a >= 3 ? udec(argv[a + 2]) : 0);
    else if (streq(op, "read") && argc - a >= 3)
        cmd_read(hex(argv[a]), hex(argv[a + 1]), argv[a + 2]);
    else if (streq(op, "write") && argc - a >= 2)
        cmd_write(hex(argv[a]), argv[a + 1], 0);
    else if (streq(op, "hold") && argc - a >= 3)
        cmd_write(hex(argv[a]), argv[a + 1], udec(argv[a + 2]));
    else
        die("bad args; usage: [-F] scan <pa> <len> [minpts] | read <pa> <len> <f> | write <pa> <f> | hold <pa> <f> <ms>");
    sc(SYS_exit, 0, 0, 0, 0, 0);
}

__attribute__((naked, section(".text.start"))) void _start(void)
{
    asm volatile("ldr r0, [sp]\n add r1, sp, #4\n bl cmain\n b .");
}

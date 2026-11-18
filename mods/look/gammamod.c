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
 *   gammamod find  <pa_hex> <len_hex> <patfile> read-only: list RAM hits of a byte pattern
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
static long sc6(long n, long a, long b, long c, long d, long e, long f)
{
    register long r0 asm("r0") = a, r1 asm("r1") = b, r2 asm("r2") = c,
                  r3 asm("r3") = d, r4 asm("r4") = e, r5 asm("r5") = f, r7 asm("r7") = n;
    asm volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5),
                 "r"(r7) : "memory");
    return r0;
}
#define SYS_mmap2 192
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

static char g_buf[1 << 16];     /* 64 KiB working buffer (low-RAM camera) */

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
                    puts1("  "); putdec(a); puts1(".."); putdec(b);
                    { ul h = 2166136261UL; for (long q = 0; q < run; q++) h = (h ^ w[i + q]) * 16777619UL; puts1("  ck "); puthex(h); }
                    puts1("\n");
                }
                i += run;
            } else i++;
        }
        done += chunk > 128 ? chunk - 128 : chunk;
    }
    puts1("gammamod: "); putdec(hits); puts1(" candidates\n");
}

/* read-only: find every 2-byte-aligned occurrence of a byte pattern (from a file, <=64 B) in a RAM window */
static void cmd_find(ul pa, ul len, const char *patfile)
{
    static char pat[64];
    ul pl = read_all(patfile, pat, sizeof pat);
    if (pl < 8) die("pattern file too short (need >= 8 bytes)");
    puts1("gammamod: find "); putdec(pl); puts1("B in "); puthex(pa); puts1(" + "); puthex(len); puts1("\n");
    ul done = 0, hits = 0, skipped = 0;
    while (done < len) {
        ul chunk = len - done; if (chunk > sizeof g_buf) chunk = sizeof g_buf;
        long got = rd(pa + done, g_buf, chunk);
        if (got <= 0) { done += 4096; skipped++; continue; }
        for (long i = 0; i + (long)pl <= got; i += 2) {
            if (g_buf[i] != pat[0] || g_buf[i + 1] != pat[1]) continue;
            ul k = 2; while (k < pl && g_buf[i + k] == pat[k]) k++;
            if (k == pl && hits < 60) { hits++; puts1("  HIT PA "); puthex(pa + done + i); puts1("\n"); }
        }
        /* advance, overlapping by pl-2 so boundary matches are not missed */
        done += (ul)got > pl ? (ul)got - (pl & ~1UL) : (ul)got;
        if (((pa + done) & 0x3fffff) < 0x10000) { puts1("  .. "); puthex(pa + done); puts1("\n"); }   /* progress ~4 MiB */
    }
    puts1("gammamod: "); putdec(hits); puts1(" hits, "); putdec(skipped); puts1(" unreadable pages\n");
}

/* scan for monotone-nondecreasing u32 ramps (0..<=64 -> >=1000); prints FNV checksum so two runs can be diffed */
static void cmd_scan32(ul pa, ul len, int minpts)
{
    if (!minpts) minpts = 64;
    puts1("gammamod: scan32 "); puthex(pa); puts1(" + "); puthex(len); puts1("\n");
    ul done = 0, hits = 0;
    while (done < len) {
        ul chunk = len - done; if (chunk > sizeof g_buf) chunk = sizeof g_buf;
        long got = rd(pa + done, g_buf, chunk);
        if (got <= 0) { done += 4096; continue; }
        ul *w = (ul *)g_buf; long cnt = got / 4;
        for (long i = 0; i < cnt - minpts; ) {
            ul prev = 0; long run = 0, j = i;
            while (j < cnt && w[j] >= prev && w[j] < 0x10000) { prev = w[j]; run++; j++; }
            if (run >= minpts) {
                ul a = w[i], b = w[i + run - 1];
                int distinct = (w[i] != w[i+1]) + (w[i+1] != w[i+2]) + (w[i+2] != w[i+3]);
                if (a <= 64 && b >= 1000 && distinct >= 2) {
                    hits++;
                    puts1("  PA "); puthex(pa + done + i * 4); puts1("  len "); putdec(run);
                    puts1("  "); putdec(a); puts1(".."); putdec(b);
                    { ul h = 2166136261UL; for (long q = 0; q < run; q++) h = (h ^ w[i + q]) * 16777619UL; puts1("  ck "); puthex(h); }
                    puts1("\n");
                }
                i += run;
            } else i++;
        }
        done += (ul)got > 128 ? (ul)got - 128 : (ul)got;      /* overlap so a ramp on a chunk edge is seen */
    }
    puts1("gammamod: "); putdec(hits); puts1(" ramps\n");
}

/* ---- MMIO (ISP gamma SRAM) access: /dev/mem mmap, 32-bit accesses -------------------
 * Hardware gamma table found by RE: CamCore CC block base 0xF3200000, table at +0x15300,
 * 96 x u32 (0x180 B).  Default window = exactly that table; -F lifts it. */
#define HW_LO 0xF3215300UL
#define HW_HI 0xF3215480UL
static volatile ul *hwmap(ul pa, ul len)
{
    if (!g_force && (pa < HW_LO || pa + len > HW_HI))
        die("MMIO PA outside the gamma window 0xF3215300..0xF321547F (use -F only if you mean it)");
    if (pa & 3 || len & 3) die("MMIO access must be 4-byte aligned");
    ul base = pa & ~0xfffUL;
    if (pa + len > base + 0x1000) die("MMIO window crosses a page");
    long fd = sc(SYS_open, (long)"/dev/mem", 2 | 0x101000 /*O_RDWR|O_SYNC*/, 0, 0, 0);
    if (fd < 0) die("open /dev/mem");
    long m = sc6(SYS_mmap2, 0, 0x1000, 3 /*RW*/, 1 /*SHARED*/, fd, base >> 12);
    if ((ul)m > (ul)-4096) die("mmap /dev/mem MMIO failed");
    return (volatile ul *)(m + (pa - base));
}
static void cmd_hwrd(ul pa, ul nw)
{
    volatile ul *p = hwmap(pa, nw * 4);
    for (ul i = 0; i < nw; i++) { puthex(pa + i * 4); puts1(": "); putdec(p[i]); puts1("\n"); }
}
static void cmd_hwdump(ul pa, ul nw, const char *file)
{
    volatile ul *p = hwmap(pa, nw * 4);
    static ul tmp[256]; if (nw > 256) die("too many words");
    for (ul i = 0; i < nw; i++) tmp[i] = p[i];
    long fd = open_wr(file); sc(SYS_write, fd, (long)tmp, nw * 4, 0, 0); sc(SYS_close, fd, 0, 0, 0, 0);
    puts1("gammamod: saved "); putdec(nw); puts1(" words from "); puthex(pa); puts1("\n");
}
static void cmd_hwwr(ul pa, const char *file, long hold_ms)
{
    static ul tmp[256];
    ul n = read_all(file, (char *)tmp, sizeof tmp); ul nw = n / 4;
    if (!nw) die("empty file");
    volatile ul *p = hwmap(pa, nw * 4);
    for (ul i = 0; i < nw; i++) p[i] = tmp[i];
    if (hold_ms > 0) {
        /* The camera rewrites the stock table about once per live-view frame.  Poll a few words
         * and rewrite the whole table the instant any differs (sched_yield keeps the camera alive). */
        struct { long s, ns; } t0, t1;
        sc(263, 1, (long)&t0, 0, 0, 0);
        ul fixes = 0, loops = 0;
        for (;;) {
            if (p[0] != tmp[0] || p[nw / 2] != tmp[nw / 2] || p[nw - 1] != tmp[nw - 1] || p[1] != tmp[1]) {
                for (ul i = 0; i < nw; i++) p[i] = tmp[i];
                fixes++;
            }
            if (!(++loops & 0x3ff)) {
                sc(263, 1, (long)&t1, 0, 0, 0);
                if ((t1.s - t0.s) * 1000 + (t1.ns - t0.ns) / 1000000 >= hold_ms) break;
            }
            sc(158, 0, 0, 0, 0, 0);                 /* sched_yield */
        }
        puts1("gammamod: hold done, stock table seen+overwritten "); putdec(fixes); puts1(" times\n");
    }
    ul bad = 0; for (ul i = 0; i < nw; i++) if (p[i] != tmp[i]) bad++;
    puts1("gammamod: wrote "); putdec(nw); puts1(" words to "); puthex(pa);
    puts1(bad ? "  READBACK MISMATCH words: " : "  readback ok"); if (bad) putdec(bad); puts1("\n");
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
    else if (streq(op, "hwrd") && argc - a >= 2)
        cmd_hwrd(hex(argv[a]), udec(argv[a + 1]));
    else if (streq(op, "hwdump") && argc - a >= 3)
        cmd_hwdump(hex(argv[a]), udec(argv[a + 1]), argv[a + 2]);
    else if (streq(op, "hwwr") && argc - a >= 2)
        cmd_hwwr(hex(argv[a]), argv[a + 1], argc - a >= 3 ? udec(argv[a + 2]) : 0);
    else if (streq(op, "scan32") && argc - a >= 2)
        cmd_scan32(hex(argv[a]), hex(argv[a + 1]), argc - a >= 3 ? udec(argv[a + 2]) : 0);
    else if (streq(op, "find") && argc - a >= 3)
        cmd_find(hex(argv[a]), hex(argv[a + 1]), argv[a + 2]);
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

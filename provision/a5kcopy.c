/*
 * a5kcopy — guest-side provisioning helper (freestanding, raw syscalls).
 * Copies a physical memory window (exposed by the sony-a5000 machine's
 * "provision" ROM) into a block device through /dev/mem.
 *
 *   a5kcopy <phys-hex> <len-hex> <dst-device> [skip-hex]
 *
 * Built with arm-none-eabi-gcc -nostdlib; no libc needed in the guest.
 */
typedef unsigned long ul;
static long sc(long n, long a, long b, long c, long d, long e, long f)
{
    register long r0 asm("r0") = a, r1 asm("r1") = b, r2 asm("r2") = c,
                  r3 asm("r3") = d, r4 asm("r4") = e, r5 asm("r5") = f,
                  r7 asm("r7") = n;
    asm volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3), "r"(r4),
                 "r"(r5), "r"(r7) : "memory");
    return r0;
}
#define SYS_exit 1
#define SYS_write 4
#define SYS_open 5
#define SYS_close 6
#define SYS_munmap 91
#define SYS_fsync 118
#define SYS_mmap2 192
#define SYS_llseek 140

static ul slen(const char *s) { ul n = 0; while (s[n]) n++; return n; }
static void puts1(const char *s) { sc(SYS_write, 1, (long)s, slen(s), 0, 0, 0); }
static ul hex(const char *s)
{
    ul v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (; *s; s++) {
        int c = *s, d = c >= '0' && c <= '9' ? c - '0' :
                c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) break;
        v = v * 16 + d;
    }
    return v;
}
static void puthex(ul v)
{
    char b[11]; b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 8; i++) b[2 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15];
    b[10] = 0; puts1(b);
}

void cmain(int argc, char **argv)
{
    if (argc < 4) { puts1("usage: a5kcopy <phys> <len> <dev> [skip]\n"); sc(SYS_exit, 2, 0, 0, 0, 0, 0); }
    ul phys = hex(argv[1]), len = hex(argv[2]), skip = argc > 4 ? hex(argv[4]) : 0;
    long mem = sc(SYS_open, (long)"/dev/mem", 0 /* O_RDONLY */, 0, 0, 0, 0);
    long dst = sc(SYS_open, (long)argv[3], 1 | 0x101000 /* O_WRONLY|O_SYNC */, 0, 0, 0, 0);
    if (mem < 0 || dst < 0) { puts1("a5kcopy: open failed\n"); sc(SYS_exit, 1, 0, 0, 0, 0, 0); }
    if (skip) {
        long long res;
        sc(SYS_llseek, dst, 0L, (long)skip, (long)&res, 0 /* SEEK_SET */, 0);
    }
    const ul CH = 0x100000;
    for (ul off = 0; off < len; off += CH) {
        ul n = len - off < CH ? len - off : CH;
        long p = sc(SYS_mmap2, 0, CH, 1 /* PROT_READ */, 1 /* MAP_SHARED */, mem,
                    (long)((phys + off) >> 12));
        if ((ul)p >= (ul)-4096) { puts1("a5kcopy: mmap failed at "); puthex(phys + off); puts1("\n"); sc(SYS_exit, 1, 0, 0, 0, 0, 0); }
        for (ul w = 0; w < n; ) {
            long r = sc(SYS_write, dst, p + w, n - w, 0, 0, 0);
            if (r <= 0) { puts1("a5kcopy: write failed at "); puthex(off + w); puts1("\n"); sc(SYS_exit, 1, 0, 0, 0, 0, 0); }
            w += r;
        }
        sc(SYS_munmap, p, CH, 0, 0, 0, 0);
        if ((off & 0xffffff) == 0) { puts1("a5kcopy: "); puthex(off); puts1("\n"); }
    }
    sc(SYS_fsync, dst, 0, 0, 0, 0, 0);
    sc(SYS_close, dst, 0, 0, 0, 0, 0);
    puts1("a5kcopy: done\n");
    sc(SYS_exit, 0, 0, 0, 0, 0, 0);
}

__attribute__((naked, section(".text.start"))) void _start(void)
{
    asm volatile("ldr r0, [sp]\n add r1, sp, #4\n bl cmain\n b .");
}

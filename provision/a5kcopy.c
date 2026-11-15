/*
 * a5kcopy — guest-side helper for the emulator's provisioning and
 * stage-1 loader environments (freestanding, raw syscalls, no libc).
 *
 *   a5kcopy <phys> <len|auto> <dst-file> [skip]   phys window -> file
 *   a5kcopy -f2p <file> <phys>                    file -> phys RAM
 *   a5kcopy -backup <Backup.bin>                  file -> phys from its
 *                                                 header word 0
 *   a5kcopy -mumin <dev_va>                       MUMIN hand-off: copy the
 *        mounted muminmem (dev+0x34 / dev+0x38) to PA 0x80000000, with
 *        pointers into itself rebased to VA 0x40000000
 *   a5kcopy -w32 <phys> <val> [<val>...]          write words
 *   a5kcopy -stage2 <kernel> <initrd>             stage kernel @0x90000000,
 *        initrd @0x90400000 and write the board's loader descriptor
 *        {"A5KB", kpa, klen, ipa, ilen} at PA 0x80027000
 *   a5kcopy -bkset <prop-id> <hexbytes> [Backup.bin]
 *        set one fixed-size property in the RAM copy of Backup.bin that
 *        -backup placed at the PA in the file's header word 0 (file default
 *        /mnt/setting/Backup.bin) and recompute the header checksum
 *        (docs/RE_STILL_CAPTURE.md section 6)
 *
 * Build (from the emulator repository root):
 *   arm-none-eabi-gcc -march=armv7-a -marm -O2 -nostdlib -static \
 *       -ffreestanding -o provision/a5kcopy provision/a5kcopy.c
 */
typedef unsigned long ul;
typedef unsigned int u32;
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
#define SYS_read 3
#define SYS_write 4
#define SYS_open 5
#define SYS_close 6
#define SYS_lseek 19
#define SYS_munmap 91
#define SYS_fsync 118
#define SYS_llseek 140
#define SYS_mmap2 192
#define PROT_RW 3
#define MAP_SHARED 1

static ul slen(const char *s) { ul n = 0; while (s[n]) n++; return n; }
static void puts1(const char *s) { sc(SYS_write, 1, (long)s, slen(s), 0, 0, 0); }
static void die(const char *s) { puts1("a5kcopy: "); puts1(s); puts1("\n"); sc(SYS_exit, 1, 0, 0, 0, 0, 0); }
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
static int streq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }

static long g_mem = -1;
static long memfd(void)
{
    if (g_mem < 0) g_mem = sc(SYS_open, (long)"/dev/mem", 2 /* O_RDWR */ | 0x101000, 0, 0, 0, 0);
    if (g_mem < 0) die("cannot open /dev/mem");
    return g_mem;
}
/* map [pa, pa+len) (page aligned internally); returns pointer to pa */
static unsigned char *pmap(ul pa, ul len, ul *mlen, long *base)
{
    ul pg = pa & ~0xffful, off = pa - pg;
    *mlen = (off + len + 0xfff) & ~0xffful;
    long p = sc(SYS_mmap2, 0, *mlen, PROT_RW, MAP_SHARED, memfd(), (long)(pg >> 12));
    if ((ul)p >= (ul)-4096) die("mmap failed");
    *base = p;
    return (unsigned char *)p + off;
}
static void punmap(long base, ul mlen) { sc(SYS_munmap, base, mlen, 0, 0, 0, 0); }
static u32 rd32(ul pa) { ul ml; long b; u32 v = *(volatile u32 *)pmap(pa, 4, &ml, &b); punmap(b, ml); return v; }

/* ARMv7 short-descriptor walk through swapper_pg_dir (PA 0x80034000) */
static ul v2p(ul va)
{
    if (va >= 0x40000000 && va < 0x80000000) return va + 0x40000000;   /* lowmem */
    u32 l1 = rd32(0x80034000 + (va >> 20) * 4);
    if ((l1 & 3) == 2) return (l1 & 0xfff00000) | (va & 0xfffff);
    if ((l1 & 3) != 1) die("unmapped VA");
    u32 l2 = rd32((l1 & 0xfffffc00) + ((va >> 12) & 0xff) * 4);
    if (l2 & 2) return (l2 & 0xfffff000) | (va & 0xfff);
    if ((l2 & 3) == 1) return (l2 & 0xffff0000) | (va & 0xffff);
    die("unmapped VA (l2)");
    return 0;
}

static ul file_to_phys(const char *path, ul phys)
{
    long fd = sc(SYS_open, (long)path, 0, 0, 0, 0, 0);
    if (fd < 0) die("cannot open input file");
    long size = sc(SYS_lseek, fd, 0, 2 /* SEEK_END */, 0, 0, 0);
    sc(SYS_lseek, fd, 0, 0, 0, 0, 0);
    if (size <= 0) die("empty file");
    ul ml; long b;
    unsigned char *p = pmap(phys, size, &ml, &b);
    for (long got = 0; got < size; ) {
        long r = sc(SYS_read, fd, (long)(p + got), size - got, 0, 0, 0);
        if (r <= 0) die("read failed");
        got += r;
    }
    punmap(b, ml);
    sc(SYS_close, fd, 0, 0, 0, 0, 0);
    return size;
}

static void phys_to_file(int argc, char **argv)
{
    ul phys = hex(argv[1]), len = hex(argv[2]), skip = argc > 4 ? hex(argv[4]) : 0;
    if (argv[2][0] == 'a') {
        /* "auto": phys points at an emulator provision header
         * {"A5KP", u32 length}, image at +0x1000 */
        if (rd32(phys) != 0x504b3541) die("no A5KP header");
        len = rd32(phys + 4);
        phys += 0x1000;
        puts1("a5kcopy: image length "); puthex(len); puts1("\n");
    }
    long dst = sc(SYS_open, (long)argv[3], 1 | 0x101000 /* O_WRONLY|O_SYNC */, 0, 0, 0, 0);
    if (dst < 0) die("open dst failed");
    if (skip) {
        long long res;
        sc(SYS_llseek, dst, 0L, (long)skip, (long)&res, 0, 0);
    }
    const ul CH = 0x100000;
    for (ul off = 0; off < len; off += CH) {
        ul n = len - off < CH ? len - off : CH, ml; long b;
        unsigned char *p = pmap(phys + off, n, &ml, &b);
        for (ul w = 0; w < n; ) {
            long r = sc(SYS_write, dst, (long)(p + w), n - w, 0, 0, 0);
            if (r <= 0) { puts1("a5kcopy: write failed at "); puthex(off + w); puts1("\n"); sc(SYS_exit, 1, 0, 0, 0, 0, 0); }
            w += r;
        }
        punmap(b, ml);
        if ((off & 0xffffff) == 0) { puts1("a5kcopy: "); puthex(off); puts1("\n"); }
    }
    sc(SYS_fsync, dst, 0, 0, 0, 0, 0);
    sc(SYS_close, dst, 0, 0, 0, 0, 0);
    puts1("a5kcopy: done\n");
}

static void mumin_handoff(ul dev_va)
{
    ul dev = v2p(dev_va);
    ul mva = rd32(dev + 0x34), size = rd32(dev + 0x38);
    puts1("a5kcopy: muminmem va "); puthex(mva); puts1(" size "); puthex(size); puts1("\n");
    if (mva < 0x40000000 || mva >= 0x80000000 || size == 0 || size > 0x20000) die("bad muminmem");
    ul ml1, ml2; long b1, b2;
    u32 *src = (u32 *)pmap(mva + 0x40000000, size, &ml1, &b1);
    u32 *dst = (u32 *)pmap(0x80000000, 0x20000, &ml2, &b2);
    ul n = 0;
    for (ul i = 0; i < 0x20000 / 4; i++) {
        u32 v = i < size / 4 ? src[i] : 0;
        if (v >= mva && v < mva + 0x20000) { v = v - mva + 0x40000000; n++; }
        dst[i] = v;
    }
    punmap(b1, ml1); punmap(b2, ml2);
    puts1("a5kcopy: MUMIN hand-off written, rebased "); puthex(n); puts1(" pointers\n");
}

/*
 * Backup.bin RAM image (docs/RE_STILL_CAPTURE.md 6.2, layout as parsed by
 * PMCA BackupFile):
 *   +0x0C "BKn\0" (n = 4: 6-byte property entries {u16 attr, u32 ptr};
 *         n = 2: 5-byte entries {u8 attr, u32 ptr})
 *   +0x10 numSubsystems, +0x14 numProperties, +0x18 dataOffset,
 *   +0x1C dataSize, +0x20 checksum = byte sum of [0, dataOffset + dataSize)
 *         without +0x20..+0x23
 *   +0x100 subsystem table {u16 numProperties, u32 firstIndex} (index =
 *         id >> 16), then the property table (entry firstIndex + (id & 0xFFFF));
 *   property data at ptr & 0xFFFFFF, size ptr >> 24 (0 = variable,
 *   0xFF = oversize: both refused here)
 */
static int xdig(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
           c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
static void putbytes(const unsigned char *p, ul n)
{
    for (ul i = 0; i < n; i++) {
        char t[3] = { "0123456789abcdef"[p[i] >> 4], "0123456789abcdef"[p[i] & 15], 0 };
        puts1(t);
    }
}
static u32 ld32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static u32 bk_sum(const unsigned char *p, ul len)
{
    u32 s = 0;
    for (ul i = 0; i < len; i++)
        if (i < 0x20 || i >= 0x24) s += p[i];
    return s;
}
static void bkset(ul id, const char *val, const char *file)
{
    unsigned char nv[64];
    ul n = 0;
    if (val[0] == '0' && (val[1] == 'x' || val[1] == 'X')) val += 2;
    for (; val[0]; val += 2) {
        int hi = xdig(val[0]), lo = val[1] ? xdig(val[1]) : -1;
        if (hi < 0 || lo < 0) die("bkset: value must be an even number of hex digits");
        if (n == sizeof(nv)) die("bkset: value too long");
        nv[n++] = hi << 4 | lo;
    }
    if (n == 0) die("bkset: empty value");

    long fd = sc(SYS_open, (long)file, 0, 0, 0, 0, 0);
    u32 pa = 0;
    if (fd < 0 || sc(SYS_read, fd, (long)&pa, 4, 0, 0, 0) != 4) die("bkset: cannot read Backup header");
    sc(SYS_close, fd, 0, 0, 0, 0, 0);
    if (pa < 0x80000000 || pa >= 0xA0000000) die("bkset: bad Backup phys address");

    ul ml; long b;
    unsigned char *h = pmap(pa, 0x100, &ml, &b);
    u32 w0 = ld32(h), ns = ld32(h + 0x10), np = ld32(h + 0x14);
    u32 doff = ld32(h + 0x18), dsz = ld32(h + 0x1c), ck = ld32(h + 0x20);
    unsigned char m0 = h[0xc], m1 = h[0xd], rev = h[0xe], m3 = h[0xf];
    punmap(b, ml);
    if (w0 != pa) die("bkset: RAM copy not found (header word 0 mismatch)");
    if (m0 != 'B' || m1 != 'K' || m3 != 0) die("bkset: bad Backup magic");
    if (rev != '4' && rev != '2') die("bkset: unknown Backup revision");
    ul es = rev == '4' ? 6 : 5;
    ul ptab = 0x100 + 6 * (ul)ns, total = (ul)doff + dsz;
    if (ns == 0 || np == 0 || ptab + es * np > doff || dsz == 0 || total > 0x1000000)
        die("bkset: bad Backup header");

    unsigned char *p = pmap(pa, total, &ml, &b);
    if (bk_sum(p, total) != ck) die("bkset: header checksum mismatch before write");
    ul sub = id >> 16, idx = id & 0xffff;
    if (sub >= ns) die("bkset: subsystem outside the table");
    unsigned char *se = p + 0x100 + 6 * sub;
    ul cnt = se[0] | se[1] << 8, first = ld32(se + 2);
    if (idx >= cnt || first + idx >= np) die("bkset: property outside the table");
    ul eoff = ptab + es * (first + idx);
    u32 ptr = ld32(p + eoff + es - 4);
    ul doffp = ptr & 0xffffff, sz = ptr >> 24;
    if (sz == 0 || sz == 0xff) die("bkset: variable-size property not supported");
    if (sz != n) die("bkset: size mismatch");
    if (doffp < doff || doffp + sz > total) die("bkset: property data outside the data area");

    puts1("a5kcopy: bkset "); puthex(id); puts1(" entry "); puthex(eoff);
    puts1(" ptr "); puthex(ptr); puts1(" PA "); puthex(pa + doffp);
    puts1(" old "); putbytes(p + doffp, sz);
    for (ul i = 0; i < sz; i++) p[doffp + i] = nv[i];
    u32 nck = bk_sum(p, total);
    p[0x20] = nck; p[0x21] = nck >> 8; p[0x22] = nck >> 16; p[0x23] = nck >> 24;
    puts1(" new "); putbytes(p + doffp, sz);
    puts1(" checksum "); puthex(ck); puts1(" -> "); puthex(ld32(p + 0x20)); puts1("\n");
    punmap(b, ml);
}

void cmain(int argc, char **argv)
{
    if (argc >= 4 && streq(argv[1], "-f2p")) {
        ul n = file_to_phys(argv[2], hex(argv[3]));
        puts1("a5kcopy: loaded "); puthex(n); puts1(" bytes at "); puts1(argv[3]); puts1("\n");
    } else if (argc >= 3 && streq(argv[1], "-backup")) {
        long fd = sc(SYS_open, (long)argv[2], 0, 0, 0, 0, 0);
        u32 pa = 0;
        if (fd < 0 || sc(SYS_read, fd, (long)&pa, 4, 0, 0, 0) != 4) die("cannot read Backup header");
        sc(SYS_close, fd, 0, 0, 0, 0, 0);
        if (pa < 0x80000000 || pa >= 0xA0000000) die("bad Backup phys address");
        ul n = file_to_phys(argv[2], pa);
        puts1("a5kcopy: Backup "); puthex(n); puts1(" bytes at "); puthex(pa); puts1("\n");
    } else if (argc >= 4 && streq(argv[1], "-bkset")) {
        bkset(hex(argv[2]), argv[3], argc > 4 ? argv[4] : "/mnt/setting/Backup.bin");
    } else if (argc >= 3 && streq(argv[1], "-mumin")) {
        mumin_handoff(hex(argv[2]));
    } else if (argc >= 4 && streq(argv[1], "-w32")) {
        ul pa = hex(argv[2]), ml; long b;
        u32 *p = (u32 *)pmap(pa, 4 * (argc - 3), &ml, &b);
        for (int i = 3; i < argc; i++) p[i - 3] = hex(argv[i]);
        punmap(b, ml);
    } else if (argc >= 4 && streq(argv[1], "-stage2")) {
        ul kl = file_to_phys(argv[2], 0x90000000);
        ul il = file_to_phys(argv[3], 0x90400000);
        ul ml; long b;
        u32 *p = (u32 *)pmap(0x80027000, 20, &ml, &b);
        p[1] = 0x90000000; p[2] = kl; p[3] = 0x90400000; p[4] = il;
        p[0] = 0x424b3541;      /* "A5KB" written last */
        punmap(b, ml);
        puts1("a5kcopy: stage 2 staged, kernel "); puthex(kl);
        puts1(" initrd "); puthex(il); puts1("\n");
    } else if (argc >= 4) {
        phys_to_file(argc, argv);
    } else {
        puts1("usage: see source\n");
        sc(SYS_exit, 2, 0, 0, 0, 0, 0);
    }
    sc(SYS_exit, 0, 0, 0, 0, 0, 0);
}

__attribute__((naked, section(".text.start"))) void _start(void)
{
    asm volatile("ldr r0, [sp]\n add r1, sp, #4\n bl cmain\n b .");
}

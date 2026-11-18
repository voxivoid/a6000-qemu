/*
 * pploader - RAM-only loader for pphook.so (A6000 fw 3.21, emulator-tested).
 *
 * A debugger-style call, the same technique gdb uses for `call dlopen(...)`:
 * attach to the running camera app, make one of its threads call the C
 * library's own __libc_dlopen_mode() to map a shared object, then put the
 * thread's registers back exactly as they were and detach.  Nothing is
 * written to flash: the shared object lives on the /android tmpfs (RAM) and
 * is gone at power-off, and the app's on-disk image is never touched.  This
 * is for a device you own; it is validated in the QEMU emulator before it is
 * ever used on hardware, where a fault costs one battery-pull, not a brick.
 *
 *   pploader <pid> <dlopen_off_hex> <so_path>
 *     pid            target process id (pidof im.elf)
 *     dlopen_off_hex file offset of __libc_dlopen_mode in libc-2.11.2.so
 *                    (0xc37cd: the low bit marks the Thumb entry)
 *     so_path        absolute path of the object to map, e.g. /android/pp.so
 *
 * Build (as a6kcopy, a freestanding guest binary, no cross-libc needed):
 *   arm-none-eabi-gcc -march=armv7-a -marm -O2 -nostdlib -static \
 *       -ffreestanding -Wl,-Ttext-segment=0x10000 -o provision/pploader \
 *       provision/pploader.c
 */
typedef unsigned long ul;

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
#define SYS_ptrace 26
#define SYS_wait4 114
#define SYS_llseek 140

/* ptrace requests */
#define PT_ATTACH  16
#define PT_DETACH  17
#define PT_CONT    7
#define PT_GETREGS 12
#define PT_SETREGS 13

#define RTLD_NOW 2
#define CPSR_T   0x20          /* Thumb bit */
#define SIGSEGV  11
#define SIGSTOP  19

/* ARM pt_regs: uregs[0..15]=r0..r15(pc), [16]=cpsr, [17]=orig_r0 */
struct regs { ul uregs[18]; };

void *memcpy(void *d, const void *s, unsigned n)      /* gcc emits calls to this */
{ char *a = d; const char *b = s; while (n--) *a++ = *b++; return d; }
static ul slen(const char *s) { ul n = 0; while (s[n]) n++; return n; }
static void puts1(const char *s) { sc(SYS_write, 1, (long)s, slen(s), 0, 0); }
static void die(const char *s)
{ puts1("pploader: "); puts1(s); puts1("\n"); sc(SYS_exit, 1, 0, 0, 0, 0); }
static void puthex(ul v)
{
    char b[11]; b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 8; i++) b[2 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15];
    b[10] = 0; puts1(b);
}
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
static ul udec(const char *s) { ul v = 0; for (; *s >= '0' && *s <= '9'; s++) v = v * 10 + (*s - '0'); return v; }
static int utod(ul v, char *b)          /* unsigned -> decimal, returns len */
{
    char t[12]; int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = '0' + v % 10; v /= 10; }
    for (int i = 0; i < n; i++) b[i] = t[n - 1 - i];
    b[n] = 0; return n;
}
static int substr(const char *h, const char *n)   /* n occurs in h? */
{
    for (; *h; h++) { const char *a = h, *b = n; while (*b && *a == *b) a++, b++; if (!*b) return 1; }
    return 0;
}

static long ptrace(long req, long pid, long addr, void *data)
{ return sc(SYS_ptrace, req, pid, addr, (long)data, 0); }

static long waitstop(long pid);

/* set up the stopped thread to call func(a0,a1), run until it returns to
 * pc=0 (SIGSEGV we catch), and return r0.  saved = its real registers; the
 * thread is left stopped.  Returns 0 and sets *ok=0 if the target died. */
static ul remote_call(long pid, struct regs *saved, ul func, ul a0, ul a1,
                      ul newsp, int *ok)
{
    struct regs work = *saved;
    work.uregs[0]  = a0;
    work.uregs[1]  = a1;
    work.uregs[13] = newsp;
    work.uregs[14] = 0;                    /* lr = 0 -> fault on return */
    work.uregs[15] = func & ~1UL;
    if (func & 1) work.uregs[16] = saved->uregs[16] | CPSR_T;
    else          work.uregs[16] = saved->uregs[16] & ~(ul)CPSR_T;
    ptrace(PT_SETREGS, pid, 0, &work);
    ptrace(PT_CONT, pid, 0, 0);
    for (;;) {
        long sig = waitstop(pid);
        if (sig < 0) { *ok = 0; return 0; }
        if (sig == SIGSEGV) break;
        ptrace(PT_CONT, pid, 0, (void *)sig);
    }
    struct regs after;
    ptrace(PT_GETREGS, pid, 0, &after);
    *ok = 1;
    return after.uregs[0];
}

/* lowest mapping of a named object in /proc/<pid>/maps (its load base) */
static ul maps_base(long pid, const char *name)
{
    char path[32]; int i = 0;
    const char *a = "/proc/"; while (*a) path[i++] = *a++;
    i += utod(pid, path + i);
    a = "/maps"; while (*a) path[i++] = *a++; path[i] = 0;
    long fd = sc(SYS_open, (long)path, 0, 0, 0, 0);
    if (fd < 0) die("open maps");
    static char buf[1 << 16]; int n = 0, r;
    while (n < (int)sizeof buf - 1 && (r = sc(SYS_read, fd, (long)(buf + n), sizeof buf - 1 - n, 0, 0)) > 0) n += r;
    sc(SYS_close, fd, 0, 0, 0, 0);
    buf[n] = 0;
    for (char *p = buf; *p; ) {
        char *e = p; while (*e && *e != '\n') e++;
        char save = *e; *e = 0;
        if (substr(p, name)) { *e = save; return hex(p); }
        *e = save; p = *e ? e + 1 : e;
    }
    return 0;
}

static long memfd(long pid)
{
    char path[32]; int i = 0;
    const char *a = "/proc/"; while (*a) path[i++] = *a++;
    i += utod(pid, path + i);
    a = "/mem"; while (*a) path[i++] = *a++; path[i] = 0;
    return sc(SYS_open, (long)path, 2 /*O_RDWR*/, 0, 0, 0);
}

static void mem_write(long fd, ul addr, const void *data, ul len)
{
    long off;
    if (sc(SYS_llseek, fd, 0 /*offhi, 32-bit addr*/, (long)addr, (long)&off, 0 /*SEEK_SET*/) < 0)
        die("llseek mem");
    if (sc(SYS_write, fd, (long)data, len, 0, 0) != (long)len)
        die("write mem");
}

static long waitstop(long pid)              /* wait, return WSTOPSIG or -1 */
{
    int status = 0;
    if (sc(SYS_wait4, pid, (long)&status, 0, 0, 0) < 0) die("wait4");
    if ((status & 0xff) == 0x7f) return (status >> 8) & 0xff;   /* stopped */
    return -1;                                                  /* exited */
}

void cmain(int argc, char **argv)
{
    if (argc < 4) die("usage: pploader <pid> <dlopen_off_hex> <so_path>");
    long pid = udec(argv[1]);
    ul off = hex(argv[2]);
    const char *so = argv[3];
    ul so_len = slen(so) + 1;

    ul libc = maps_base(pid, "/libc-2.11");
    if (!libc) die("libc not mapped in target");
    ul dlopen = libc + off;                 /* low bit = Thumb entry */
    puts1("pploader: libc "); puthex(libc); puts1(" dlopen "); puthex(dlopen); puts1("\n");

    if (ptrace(PT_ATTACH, pid, 0, 0) < 0) die("PTRACE_ATTACH (not root / no CONFIG_PTRACE?)");
    if (waitstop(pid) != SIGSTOP) die("no SIGSTOP after attach");

    struct regs saved;
    if (ptrace(PT_GETREGS, pid, 0, &saved) < 0) die("GETREGS");

    /* scratch for the path string, high on the thread's stack; dlopen's own
     * frames grow down from work sp, kept well below the string */
    ul sp = saved.uregs[13];
    ul scratch = (sp - 0x400) & ~7UL;
    ul newsp   = (scratch - 0x800) & ~7UL;

    long fd = memfd(pid);
    if (fd < 0) die("open /proc/pid/mem");
    mem_write(fd, scratch, so, so_len);
    (void)sp;

    int ok = 0;
    ul libdl = maps_base(pid, "/libdl-2.11");
    if (!libdl) die("libdl not mapped in target");
    (void)dlopen;
    ul handle = remote_call(pid, &saved, libdl + 0xbb1 /*dlopen*/, scratch, RTLD_NOW, newsp, &ok);
    if (!ok) die("target exited during dlopen");
    puts1("pploader: dlopen handle "); puthex(handle); puts1("\n");

    if (!handle) {
        ul derr = remote_call(pid, &saved, libdl + 0xded /*dlerror*/, 0, 0, newsp, &ok);
        if (ok && derr) {
            char eb[160]; long o;
            sc(SYS_llseek, fd, 0, (long)derr, (long)&o, 0);
            long n = sc(SYS_read, fd, (long)eb, sizeof eb - 1, 0, 0);
            if (n > 0) { eb[n] = 0; puts1("pploader: dlerror: "); puts1(eb); puts1("\n"); }
        }
    }

    if (ptrace(PT_SETREGS, pid, 0, &saved) < 0) die("SETREGS restore");
    sc(SYS_close, fd, 0, 0, 0, 0);
    if (ptrace(PT_DETACH, pid, 0, 0) < 0) die("DETACH");

    if (!handle) die("dlopen returned 0 (load failed)");
    puts1("pploader: ok, pphook mapped RAM-only\n");
    sc(SYS_exit, 0, 0, 0, 0, 0);
}

__attribute__((naked, section(".text.start"))) void _start(void)
{
    asm volatile("ldr r0, [sp]\n add r1, sp, #4\n bl cmain\n b .");
}

/*
 * pphook.c - in-process probe for the camera model API inside im.elf
 * (A6000 fw 3.21 only).  RAM-only: it calls the camera app's own functions,
 * never writes flash, and touches no file outside its own directory.
 *
 * Loading (RAM-only, preferred): provision/pploader maps this library into
 * the already-running im.elf with a debugger-style dlopen() call, from the
 * /android tmpfs.  Nothing is written to flash; a power cycle clears it.
 *     tools/pp_inject.sh            (push + inject, once per boot)
 * The constructor does nothing outside im.elf.
 *
 * Loading (flash, legacy): the stock init debug hook.  With /setting/mode/dmode
 * present, init putenv()s every NAME=VALUE of /setting/env.txt, so
 *     LD_PRELOAD=/android/pp/pphook.so
 * maps it into every process init starts.  This writes two small files to the
 * settings partition (NAND); delete them to undo.  Prefer the RAM-only path.
 *
 * Safety gates:
 *   - acts only when /proc/self/comm is "im.elf";
 *   - waits until the app's ViewManager singleton exists (calling the model
 *     API earlier crashes im.elf: ViewManager::getModelInfo with this == 0,
 *     seen in the emulator) -- polled by reading pointers only;
 *   - safe mode: /android/pp/attempt exists while the hook does anything that
 *     could crash (start-up until it idles, and every command batch).  If a
 *     boot finds it present, the previous run crashed: the hook stays idle
 *     until the file is deleted (a crash costs one boot, never a boot loop);
 *   - all libc/libpthread references are weak (a process without them still
 *     loads this library; nothing is called there);
 *   - refuses to call anything unless libObj.so's bytes at the 3.21
 *     ParamList::add entry match (a different firmware build is never called).
 *
 * Interface: a command file polled once per second,
 *     /android/pp/cmd.txt   (consumed: renamed to cmd.done after execution)
 * results appended to
 *     /android/pp/out.txt
 * Commands (numbers in hex or decimal):
 *     model <name>                 model for later commands (default model/CAMERA)
 *     scan <sel_lo> <sel_hi> <id>  getModelInfo(model, sel) for each selector;
 *                                  report the list's size and param <id> if present
 *     get <sel> <id> [<id>...]     values of params in getModelInfo(model, sel)
 *     dump <sel>                   every param id 0..0x545 present in that list
 *     exec <cmd> <id>=<val> ...    requestModelExecute(model, cmd, [int params])
 *     sleep <seconds>
 *
 * ABI (3.21 libObj.so, measured):
 *   ParamList::ParamList()            0x7edc3f (exported)  object 8 bytes
 *   ParamList::add(id, ParamBase*)    0x7ee0e7 (exported)  list takes ownership
 *   ParamList::get(id, idx) const     0x7edacb (exported)  -> ParamBase* or 0
 *   viewManagerIf::getModelInfo(const char*, int, ParamList*, const ParamList*)
 *                                     0x12503d (exported, static)
 *   viewManagerIf::requestModelExecute(const char*, unsigned long, ParamList*)
 *                                     0x1250c1 (exported, static)
 *   int param (ParamBase type 1): operator new(16), ctor 0xf0fb1(obj, value),
 *     layout {vtable, type=1, ?, int value @+12}  (libObj 0x68f8a4 pattern)
 *   PARAMID = index into libObj's PARAMID name table (0x1a4 = PP_GAMMA_TYPE_SET,
 *     checked against the list builder at 0x68f8b6)
 *
 * Build (no glibc toolchain needed; symbols bind at load time inside im.elf):
 *   arm-none-eabi-gcc -march=armv7-a -marm -mfloat-abi=soft -O2 -fPIC -shared \
 *     -nostdlib -ffreestanding -fno-builtin -Wl,-z,lazy -o pphook.so pphook.c
 */
#include <stdint.h>
#include <stddef.h>

/* ---- libc / libpthread, weak so a process without them still loads us ----
 * Never called through the PLT: glibc 2.11's lazy binder left a weak symbol's
 * PLT slot unrelocated (jump to 0x320, im.elf crash in the emulator).  Each
 * function is taken by address into a table instead; those R_ARM_ABS32
 * relocations are resolved eagerly at load time (0 when absent). */
#define W __attribute__((weak))
extern int open(const char *, int, ...) W;
extern int read(int, void *, unsigned) W;
extern int write(int, const void *, unsigned) W;
extern int close(int) W;
extern int rename(const char *, const char *) W;
extern void *malloc(unsigned) W;
extern int snprintf(char *, unsigned, const char *, ...) W;
extern unsigned long strtoul(const char *, char **, int) W;
extern int nanosleep(const void *, void *) W;
extern int pthread_create(void *, const void *, void *(*)(void *), void *) W;
extern int pthread_detach(unsigned long) W;
extern int getpid(void) W;
extern int unlink(const char *) W;
__attribute__((visibility("hidden"))) struct pp_libc {
    int (*open)(const char *, int, ...);
    int (*read)(int, void *, unsigned);
    int (*write)(int, const void *, unsigned);
    int (*close)(int);
    int (*rename)(const char *, const char *);
    void *(*malloc)(unsigned);
    int (*snprintf)(char *, unsigned, const char *, ...);
    unsigned long (*strtoul)(const char *, char **, int);
    int (*nanosleep)(const void *, void *);
    int (*pthread_create)(void *, const void *, void *(*)(void *), void *);
    int (*pthread_detach)(unsigned long);
    int (*getpid)(void);
    int (*unlink)(const char *);
} C = { open, read, write, close, rename, malloc, snprintf, strtoul, nanosleep,
        pthread_create, pthread_detach, getpid, unlink };
#define open C.open
#define read C.read
#define write C.write
#define close C.close
#define rename C.rename
#define malloc C.malloc
#define snprintf C.snprintf
#define strtoul C.strtoul
#define nanosleep C.nanosleep
#define pthread_create C.pthread_create
#define pthread_detach C.pthread_detach
#define getpid C.getpid
#define unlink C.unlink

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0100
#define O_APPEND 02000
#define O_TRUNC  01000

#define DIR_PP   "/android/pp/"   /* tmpfs: RAM-only, gone at power-off */
#define CMD_FILE DIR_PP "cmd.txt"
#define DONE_FILE DIR_PP "cmd.done"
#define OUT_FILE DIR_PP "out.txt"
#define ATTEMPT_FILE DIR_PP "attempt"

/* 3.21 libObj.so offsets (Thumb functions: +1 when called) */
#define OFF_PL_CTOR   0x7edc3e
#define OFF_PL_ADD    0x7ee0e6
#define OFF_PL_GET    0x7edaca
#define OFF_GETMI     0x12503c
#define OFF_REQEXEC   0x1250c0
#define OFF_INT_CTOR  0xf0fb0
#define OFF_VM_GOT    0x1030e58  /* GOT slot -> &ViewManager* (viewManagerIf::getModelInfo) */
#define OBJ_DATA_LO   0xfd25a4   /* libObj RW segment start (vaddr) */
#define OBJ_END       0x10e2bd0  /* end of libObj bss */
static const unsigned char PL_ADD_SIG[] = { 0xf0, 0xb5, 0x83, 0xb0, 0x00, 0xaf, 0x04, 0x46 };

typedef void *(*pl_ctor_t)(void *);
typedef void  (*pl_add_t)(void *, unsigned long, void *);
typedef void *(*pl_get_t)(const void *, unsigned long, unsigned long);
typedef int   (*getmi_t)(const char *, int, void *, const void *);
typedef int   (*reqexec_t)(const char *, unsigned long, void *);
typedef void *(*int_ctor_t)(void *, int);

static unsigned long g_obj;     /* libObj.so load base */
static char g_model[64] = "model/CAMERA";

static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int sstarts(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }

static void out(const char *s)
{
    int fd = open(OUT_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) { write(fd, s, slen(s)); close(fd); }
}

static void nap_ms(long ms)
{
    struct { long s, ns; } t = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&t, 0);
}

static int read_file(const char *path, char *buf, int max)
{
    int fd = open(path, O_RDONLY), n = 0, r;
    if (fd < 0) return -1;
    while (n < max - 1 && (r = read(fd, buf + n, max - 1 - n)) > 0) n += r;
    close(fd);
    buf[n] = 0;
    return n;
}

/* libObj.so base: lowest mapping of it (file offset 0) in /proc/self/maps */
static unsigned long find_base(const char *name)
{
    static char b[1 << 16];
    if (read_file("/proc/self/maps", b, sizeof b) <= 0) return 0;
    unsigned long base = 0;
    for (char *p = b; *p; ) {
        char *e = p;
        while (*e && *e != '\n') e++;
        char save = *e; *e = 0;
        int hit = 0;
        for (char *q = p; *q; q++)
            if (sstarts(q, name)) { hit = 1; break; }
        if (hit) {
            unsigned long s = strtoul(p, 0, 16);
            if (!base || s < base) base = s;
        }
        *e = save;
        p = *e ? e + 1 : e;
    }
    return base;
}

static int build_ok(void)
{
    const unsigned char *p = (const unsigned char *)(g_obj + OFF_PL_ADD);
    for (unsigned i = 0; i < sizeof PL_ADD_SIG; i++)
        if (p[i] != PL_ADD_SIG[i]) return 0;
    return 1;
}

#define FN(t, off) ((t)(g_obj + (off) + 1))

static void *new_list(void)
{
    void *l = malloc(8);
    if (l) FN(pl_ctor_t, OFF_PL_CTOR)(l);
    return l;
}

static void *fetch(int sel, int *ret)
{
    void *l = new_list();
    if (!l) return 0;
    *ret = FN(getmi_t, OFF_GETMI)(g_model, sel, l, 0);
    return l;
}

static int param_int(void *list, unsigned long id, int *val)
{
    void *p = FN(pl_get_t, OFF_PL_GET)(list, id, 0);
    if (!p) return 0;
    *val = *(int *)((char *)p + 12);
    return 1;
}

static char *tok(char **s)
{
    char *p = *s;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) { *s = p; return 0; }
    char *t = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) *p++ = 0;
    *s = p;
    return t;
}
static unsigned long num(const char *t) { return t ? strtoul(t, 0, 0) : 0; }

static void run_line(char *line)
{
    char m[256], *s = line, *c = tok(&s);
    if (!c || c[0] == '#') return;
    snprintf(m, sizeof m, "> %s %s\n", c, s); out(m);
    if (sstarts(c, "model")) {
        char *n = tok(&s);
        if (n && slen(n) < sizeof g_model) { unsigned i; for (i = 0; n[i]; i++) g_model[i] = n[i]; g_model[i] = 0; }
    } else if (sstarts(c, "scan")) {
        int lo = num(tok(&s)), hi = num(tok(&s)); unsigned long id = num(tok(&s));
        for (int sel = lo; sel <= hi; sel++) {
            int r, v; void *l = fetch(sel, &r);
            if (!l) break;
            if (param_int(l, id, &v)) { snprintf(m, sizeof m, "  sel %d ret %d id 0x%lx = %d\n", sel, r, id, v); out(m); }
        }
        out("  scan done\n");
    } else if (sstarts(c, "getq")) {
        /* getModelInfo with a non-null `in` list of requested ids */
        int sel = num(tok(&s)), r, v;
        unsigned long ids[32]; int nid = 0;
        for (char *t; nid < 32 && (t = tok(&s)); ) ids[nid++] = num(t);
        void *in = new_list();
        for (int i = 0; in && i < nid; i++) {
            void *p = malloc(16);
            if (!p) break;
            FN(int_ctor_t, OFF_INT_CTOR)(p, 0);
            FN(pl_add_t, OFF_PL_ADD)(in, ids[i], p);
        }
        void *out_l = new_list();
        r = FN(getmi_t, OFF_GETMI)(g_model, sel, out_l, in);
        snprintf(m, sizeof m, "  getq sel %d ret %d (%d req)\n", sel, r, nid); out(m);
        for (int i = 0; out_l && i < nid; i++) {
            if (param_int(out_l, ids[i], &v)) snprintf(m, sizeof m, "  0x%lx = %d (0x%x)\n", ids[i], v, v);
            else snprintf(m, sizeof m, "  0x%lx absent\n", ids[i]);
            out(m);
        }
    } else if (sstarts(c, "get")) {
        int sel = num(tok(&s)), r, v; void *l = fetch(sel, &r);
        snprintf(m, sizeof m, "  sel %d ret %d\n", sel, r); out(m);
        for (char *t; l && (t = tok(&s)); ) {
            unsigned long id = num(t);
            if (param_int(l, id, &v)) snprintf(m, sizeof m, "  0x%lx = %d (0x%x)\n", id, v, v);
            else snprintf(m, sizeof m, "  0x%lx absent\n", id);
            out(m);
        }
    } else if (sstarts(c, "dump")) {
        int sel = num(tok(&s)), r, v, n = 0; void *l = fetch(sel, &r);
        snprintf(m, sizeof m, "  sel %d ret %d\n", sel, r); out(m);
        for (unsigned long id = 0; l && id < 0x546; id++)
            if (param_int(l, id, &v)) { snprintf(m, sizeof m, "  0x%03lx = %d\n", id, v); out(m); n++; }
        snprintf(m, sizeof m, "  %d params\n", n); out(m);
    } else if (sstarts(c, "exec")) {
        unsigned long cmd = num(tok(&s));
        void *l = new_list(); int n = 0;
        for (char *t; l && (t = tok(&s)); ) {
            char *eq = t; while (*eq && *eq != '=') eq++;
            if (!*eq) continue;
            *eq = 0;
            void *p = malloc(16);
            if (!p) break;
            FN(int_ctor_t, OFF_INT_CTOR)(p, (int)num(eq + 1));
            FN(pl_add_t, OFF_PL_ADD)(l, num(t), p);
            n++;
        }
        int r = l ? FN(reqexec_t, OFF_REQEXEC)(g_model, cmd, l) : -999;
        /* the request event takes ownership of the list (stock code leaks it too) */
        snprintf(m, sizeof m, "  requestModelExecute(%s, 0x%lx, %d params) = %d\n", g_model, cmd, n, r); out(m);
    } else if (sstarts(c, "sleep")) {
        nap_ms(num(tok(&s)) * 1000);
    } else {
        out("  ? unknown command\n");
    }
}

static int file_exists(const char *p)
{
    int fd = open(p, O_RDONLY);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

/* 1 if [a, a+4) lies in a readable mapping of /proc/self/maps */
static int readable(unsigned long a)
{
    static char b[1 << 16];
    if (read_file("/proc/self/maps", b, sizeof b) <= 0) return 0;
    for (char *p = b; *p; ) {
        char *q;
        unsigned long lo = strtoul(p, &q, 16), hi = strtoul(q + 1, &q, 16);
        if (a >= lo && a + 4 <= hi && q[1] == 'r') return 1;
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return 0;
}

/* ViewManager singleton and its model manager (+24), read without calling.
 * im.elf maps libObj.so before the loader has relocated it: the GOT slot then
 * still holds its link-time value (0x010a9004, seen in the emulator).  Only a
 * relocated slot (pointing into libObj's own image) is followed, and every
 * pointer is checked against /proc/self/maps first. */
static int app_ready(void)
{
    unsigned long slot = *(volatile unsigned long *)(g_obj + OFF_VM_GOT);
    if (slot < g_obj + OBJ_DATA_LO || slot >= g_obj + OBJ_END || !readable(slot)) return 0;
    unsigned long vm = *(volatile unsigned long *)slot;
    if (!vm || !readable(vm + 24)) return 0;
    return *(volatile unsigned long *)(vm + 24) != 0;
}

static void *worker(void *arg)
{
    (void)arg;
    char m[160];
    for (int i = 0; i < 300 && !(g_obj = find_base("libObj.so")); i++)
        nap_ms(1000);                   /* im.elf dlopen()s it after start */
    if (!g_obj || !build_ok()) {
        snprintf(m, sizeof m, "pphook: libObj base 0x%lx, build check FAILED: not 3.21, idle\n", g_obj);
        out(m);
        return 0;
    }
    if (file_exists(ATTEMPT_FILE)) {
        out("pphook: SAFE MODE: previous run did not finish (crash?); idle until " ATTEMPT_FILE " is deleted\n");
        return 0;
    }
    int afd = open(ATTEMPT_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (afd >= 0) close(afd);
    int waited = 0;
    while (!app_ready()) { nap_ms(1000); waited++; }
    nap_ms(10000);                      /* let the first screen settle */
    snprintf(m, sizeof m, "pphook: app ready after ~%d s\n", waited + 15); out(m);
    snprintf(m, sizeof m, "pphook: pid %d libObj 0x%lx ready, polling %s\n", getpid ? getpid() : -1, g_obj, CMD_FILE);
    out(m);
    unlink(ATTEMPT_FILE);               /* start-up survived */
    static char buf[4096];
    for (;;) {
        if (read_file(CMD_FILE, buf, sizeof buf) > 0) {
            rename(CMD_FILE, DONE_FILE);    /* consume first: a crash never re-runs it */
            int fd = open(ATTEMPT_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) close(fd);
            for (char *p = buf; *p; ) {
                char *e = p;
                while (*e && *e != '\n' && *e != '\r') e++;
                char save = *e; *e = 0;
                run_line(p);
                p = save ? e + 1 : e;
            }
            out("pphook: batch done\n");
            unlink(ATTEMPT_FILE);
        }
        nap_ms(1000);
    }
    return 0;
}

static int in_im_elf(void)
{
    char c[32];
    int n = read_file("/proc/self/comm", c, sizeof c);
    return n >= 6 && sstarts(c, "im.elf");
}

__attribute__((constructor)) static void pphook_init(void)
{
    if (!open || !read || !pthread_create || !malloc || !snprintf || !nanosleep)
        return;                         /* not a process we can work in */
    if (!in_im_elf())
        return;
    unsigned long th;
    if (pthread_create(&th, 0, worker, 0) == 0 && pthread_detach)
        pthread_detach(th);
}

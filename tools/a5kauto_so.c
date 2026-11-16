/*
 * a5kauto_so.c - Track B route (b) *autostart* hook (persistence step).
 *
 * This shared object is injected into an EARLY boot process by the stock
 * environment-injection mechanism, with no flash / brick-tier write:
 *
 *   /setting/mode/dmode   (any benign content) -> stock /sbin/init calls
 *                          set_oreore_env() only when this file is present
 *                          (init's 0x144bc gate, firmware 1.10).
 *   /setting/env.txt      line  LD_PRELOAD=/setting/a5k/a5kauto.so
 *                          -> init putenv()s it; init launches bootin.elf,
 *                             sen.elf, s_start_android.sh, net_cmdd.elf with
 *                             execv(), so they inherit LD_PRELOAD and the glibc
 *                             loader maps THIS .so into them.  (bootin.elf
 *                             clears LD_PRELOAD only for the im.elf it forks, so
 *                             im.elf itself is never preloaded -- we re-inject.)
 *
 * /setting is "vfat rw" with NO noexec/nosuid -> ld.so can map a .so from it.
 *
 * We run only inside bootin.elf (the earliest always-present host; one clean
 * instance, no ash fan-out).  The constructor forks a detached worker that:
 *   1. waits for im.elf to exist with glibc mapped,
 *   2. waits for the panel OSD layer to be ON (AIF 0xF3440144 bit1) so the
 *      payload is visible, with a timeout fallback,
 *   3. ptrace-attaches an im.elf thread and makes it call __libc_dlopen_mode(
 *      "/setting/a5k/pay.so", RTLD_NOW) -- the proven route-(b) injection
 *      (TRACKB_INPROCESS.md section 11) -- then restores and detaches.
 * An abstract-socket lock + an "already in maps" check keep it single-shot.
 *
 * RAM-only at run time: the only persistent writes are the three /setting
 * files above (rw tier, reversible by deletion; nflasha1/nflasha2 FAT, /usr,
 * /system, rootfs and the kernel are untouched, so the USB updater path stays
 * reachable -- RECOVERY_MAP.md invariant preserved).
 *
 * Build (glibc ARM, soft-float, docker image a5k-cross):
 *   arm-linux-gnueabi-gcc -std=gnu99 -O2 -fPIC -shared -fno-stack-protector \
 *     -o a5kauto.so tools/a5kauto_so.c -lpthread
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>

#define PAYLOAD      "/setting/a5k/pay.so"
#define PAYLOAD_BASE "pay.so"
#define DLOPEN_OFF   0xc37ccUL            /* __libc_dlopen_mode, Thumb (bit0) */
#define LIBC_MATCH   "libc-2.11"
#define LOGPATH      "/setting/a5k/autohook.log"
#define LOCK_NAME    "\0a5kautolock"      /* abstract unix socket, boot-local  */
#define LOCK_LEN     (1 + 11)

/* AIF panel registers (RE_DISPLAY_SCANOUT.md): 0xF3440144 bit1 = OSD layer on */
#define AIF_PHYS     0xF3440000UL
#define AIF_OSD_ON   0x144U
#define OSD_ON_BIT   0x2U

#define NREG 18
#define R_SP 13
#define R_LR 14
#define R_PC 15
#define R_CPSR 16
#define CPSR_T 0x20u
#define RTLD_NOW 2

static void logline(const char *s)
{
    int fd = open(LOGPATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) { (void)!write(fd, s, strlen(s)); close(fd); }
}

static void logkv(const char *k, unsigned long v)
{
    char b[96]; snprintf(b, sizeof b, "%s0x%lx\n", k, v); logline(b);
}

/* read /proc/self/comm, trimmed */
static void self_comm(char *out, int n)
{
    out[0] = 0;
    int fd = open("/proc/self/comm", O_RDONLY);
    if (fd < 0) return;
    int r = read(fd, out, n - 1); close(fd);
    if (r < 0) r = 0;
    while (r && (out[r-1] == '\n' || out[r-1] == ' ')) r--;
    out[r] = 0;
}

/* scan /proc for a process whose cmdline contains "im.elf"; return pid or 0 */
static pid_t find_improc(void)
{
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *e;
    pid_t found = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char p[64]; snprintf(p, sizeof p, "/proc/%s/cmdline", e->d_name);
        int fd = open(p, O_RDONLY); if (fd < 0) continue;
        char b[256]; int r = read(fd, b, sizeof b - 1); close(fd);
        if (r <= 0) continue; b[r] = 0;
        for (int i = 0; i < r; i++) if (b[i] == 0) b[i] = ' ';
        if (strstr(b, "im.elf")) { found = (pid_t)atoi(e->d_name); break; }
    }
    closedir(d);
    return found;
}

/* return lowest r-xp base of a mapping matching `match` in pid's maps, else 0.
 * also sets *has_payload if PAYLOAD_BASE already mapped. */
static unsigned long map_base(pid_t pid, const char *match, int *has_payload)
{
    char p[64]; snprintf(p, sizeof p, "/proc/%d/maps", (int)pid);
    FILE *f = fopen(p, "r"); if (!f) return 0;
    char line[512]; unsigned long best = 0;
    while (fgets(line, sizeof line, f)) {
        if (has_payload && strstr(line, PAYLOAD_BASE)) *has_payload = 1;
        if (!strstr(line, match)) continue;
        if (!strstr(line, "r-xp")) continue;
        unsigned long s = strtoul(line, 0, 16);
        if (!best || s < best) best = s;
    }
    fclose(f);
    return best;
}

/* true once the panel OSD layer is on (so the payload rectangle is visible) */
static int osd_is_on(void)
{
    int fd = open("/dev/mem", O_RDONLY);
    if (fd < 0) return -1;                      /* unknown */
    volatile uint32_t *r = mmap(0, 0x1000, PROT_READ, MAP_SHARED, fd, AIF_PHYS);
    close(fd);
    if (r == MAP_FAILED) return -1;
    uint32_t v = r[AIF_OSD_ON / 4];
    munmap((void *)r, 0x1000);
    return (v & OSD_ON_BIT) ? 1 : 0;
}

static long xpt(int req, pid_t pid, void *a, void *d) { errno = 0; return ptrace(req, pid, a, d); }

/* ptrace __libc_dlopen_mode(PAYLOAD) inside im.elf; 0 ok */
static int inject(pid_t pid, unsigned long dlopen_addr)
{
    if (xpt(PTRACE_ATTACH, pid, 0, 0) == -1) { logkv("attach_errno=", errno); return -1; }
    int st; if (waitpid(pid, &st, 0) < 0 || !WIFSTOPPED(st)) { xpt(PTRACE_DETACH, pid, 0, 0); return -1; }

    unsigned long regs[NREG], saved[NREG];
    if (xpt(PTRACE_GETREGS, pid, 0, regs) == -1) { xpt(PTRACE_DETACH, pid, 0, 0); return -1; }
    memcpy(saved, regs, sizeof regs);

    const char *so = PAYLOAD;
    unsigned long sp = regs[R_SP];
    unsigned long str = (sp - 0x400) & ~7UL;
    unsigned long newsp = (sp - 0x800) & ~7UL;
    size_t n = strlen(so) + 1;
    for (size_t i = 0; i < n; i += 4) {
        unsigned long w = 0; memcpy(&w, so + i, (n - i >= 4) ? 4 : (n - i));
        if (xpt(PTRACE_POKETEXT, pid, (void *)(str + i), (void *)w) == -1) goto restore;
    }
    regs[0] = str; regs[1] = RTLD_NOW; regs[R_SP] = newsp; regs[R_LR] = 0;
    regs[R_PC] = dlopen_addr & ~1UL; regs[R_CPSR] = saved[R_CPSR] | CPSR_T;
    if (xpt(PTRACE_SETREGS, pid, 0, regs) == -1) goto restore;
    if (xpt(PTRACE_CONT, pid, 0, 0) == -1) goto restore;
    if (waitpid(pid, &st, 0) < 0) goto restore;
    if (WIFSTOPPED(st)) {
        unsigned long rr[NREG];
        if (xpt(PTRACE_GETREGS, pid, 0, rr) != -1) logkv("dlopen_handle=", rr[0]);
    } else { logline("im.elf vanished during call!\n"); xpt(PTRACE_DETACH, pid, 0, 0); return -1; }
restore:
    xpt(PTRACE_SETREGS, pid, 0, saved);
    xpt(PTRACE_DETACH, pid, 0, 0);
    return 0;
}

static void worker(void)
{
    /* single-shot across the whole boot via an abstract unix socket */
    int lk = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (lk >= 0) {
        struct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX;
        memcpy(a.sun_path, LOCK_NAME, LOCK_LEN);
        if (bind(lk, (struct sockaddr *)&a, offsetof(struct sockaddr_un, sun_path) + LOCK_LEN) < 0)
            _exit(0);                           /* another worker owns it */
    }
    logline("worker: armed\n");

    pid_t im = 0; unsigned long base = 0;
    for (int i = 0; i < 1200 && !base; i++) {   /* up to ~120s for im.elf+libc */
        im = find_improc();
        if (im) { int hp = 0; base = map_base(im, LIBC_MATCH, &hp); if (hp) { logline("payload already loaded\n"); _exit(0); } }
        struct timespec ts = { 0, 100 * 1000000L }; nanosleep(&ts, 0);
    }
    if (!base) { logline("im.elf/libc not found\n"); _exit(0); }
    logkv("im_pid=", (unsigned long)im);
    logkv("libc_base=", base);

    for (int i = 0; i < 900; i++) {             /* up to ~90s for OSD on */
        int s = osd_is_on();
        if (s == 1) { logline("osd on\n"); break; }
        if (s < 0 && i > 50) { logline("osd state unknown, proceeding\n"); break; }
        struct timespec ts = { 0, 100 * 1000000L }; nanosleep(&ts, 0);
    }

    int hp = 0; (void)map_base(im, LIBC_MATCH, &hp);
    if (hp) { logline("payload already loaded (late)\n"); _exit(0); }

    logkv("inject dlopen=", base + DLOPEN_OFF);
    if (inject(im, base + DLOPEN_OFF) == 0) logline("inject: done\n");
    else logline("inject: FAILED\n");
    _exit(0);
}

__attribute__((constructor))
static void a5kauto_ctor(void)
{
    char comm[32]; self_comm(comm, sizeof comm);
    /* run only in the earliest clean host; skip ash and its fan-out */
    if (strcmp(comm, "bootin.elf") != 0 && strcmp(comm, "net_cmdd.elf") != 0)
        return;

    pid_t p = fork();
    if (p != 0) return;                         /* host continues normally */
    /* double-fork: grandchild reparents to init at once, so the worker never
     * lingers as bootin's child (no interaction with bootin's wait()).      */
    setsid();
    if (fork() != 0) _exit(0);
    worker();                                   /* never returns */
}

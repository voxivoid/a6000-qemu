/*
 * a5kinject.c - ptrace .so injector for im.elf (Track B route (b)).
 *
 *   a5kinject <pid> <so_path> <dlopen_off_hex> [libc_match]
 *
 * Attaches to the im.elf thread <pid>, hijacks it to call glibc's
 * __libc_dlopen_mode(so_path, RTLD_NOW) at (libc_base + dlopen_off), catches
 * the return via a NULL return address (deliberate SIGSEGV), reads the handle,
 * then restores the saved registers and detaches so im.elf resumes unchanged.
 * libc_base is read from /proc/<pid>/maps (first r-xp mapping of libc_match,
 * default "libc-2.11").  __libc_dlopen_mode on this glibc is a Thumb function,
 * so the call is entered with the CPSR Thumb bit set.
 *
 * RAM-only, no flash writes.  On any ptrace failure it detaches and reports the
 * exact failing step so im.elf (the camera-engine host) is never left stopped.
 *
 * dlopen offset (firmware 1.10): from the guest rootfs libc,
 *   arm-*-readelf -sW lib/libc-2.11.2.so | grep __libc_dlopen_mode
 *   -> st_value 0x000c37cd (LSB=1 -> Thumb) => pass off = c37cc.
 * libc base varies per boot, so it is resolved live from /proc/<pid>/maps.
 *
 * Build (glibc ARM, soft-float; docker image a5k-cross = Ubuntu 12.04
 * arm-linux-gnueabi gcc 4.6):
 *   arm-linux-gnueabi-gcc -std=gnu99 -O2 -static -fno-stack-protector \
 *     -o a5kinject tools/a5kinject.c
 * Run (emulator guest shell):
 *   P=$(grep -la im.elf /proc/[0-9]+/cmdline ...); a5kinject $P /android/a5k/inj.so c37cc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/types.h>

/* ARM PTRACE_GETREGS/SETREGS: 18 words (r0-15, cpsr, orig_r0) */
#define NREG 18
#define R_SP   13
#define R_LR   14
#define R_PC   15
#define R_CPSR 16
#define CPSR_T 0x20u          /* Thumb execution state bit */
#define RTLD_NOW 2

static long xptrace(int req, pid_t pid, void *addr, void *data, const char *w)
{
    errno = 0;
    long r = ptrace(req, pid, addr, data);
    if (r == -1 && errno) {
        fprintf(stderr, "a5kinject: ptrace(%s) failed: %s\n", w, strerror(errno));
    }
    return r;
}

static unsigned long libc_base(pid_t pid, const char *match)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/maps", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) { perror("open maps"); return 0; }
    char line[512];
    unsigned long best = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strstr(line, match)) continue;
        if (!strstr(line, "r-xp")) continue;       /* executable text seg */
        unsigned long s = strtoul(line, 0, 16);
        if (!best || s < best) best = s;            /* lowest = load base */
    }
    fclose(f);
    return best;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <pid> <so_path> <dlopen_off_hex> [libc_match]\n",
                argv[0]);
        return 2;
    }
    pid_t pid = (pid_t)atoi(argv[1]);
    const char *so = argv[2];
    unsigned long off = strtoul(argv[3], 0, 16);
    const char *match = argc > 4 ? argv[4] : "libc-2.11";

    unsigned long base = libc_base(pid, match);
    if (!base) { fprintf(stderr, "a5kinject: libc base not found\n"); return 1; }
    unsigned long target = base + off;             /* Thumb entry */
    printf("a5kinject: pid=%d libc_base=0x%lx dlopen=0x%lx so=%s\n",
           (int)pid, base, target, so);

    if (xptrace(PTRACE_ATTACH, pid, 0, 0, "ATTACH") == -1) return 1;
    int st;
    if (waitpid(pid, &st, 0) < 0) { perror("waitpid attach"); return 1; }
    if (!WIFSTOPPED(st)) { fprintf(stderr, "not stopped after attach\n"); return 1; }

    unsigned long regs[NREG], saved[NREG];
    if (xptrace(PTRACE_GETREGS, pid, 0, regs, "GETREGS") == -1) goto detach;
    memcpy(saved, regs, sizeof regs);

    /* scratch on the remote stack: string well above the new sp */
    unsigned long sp = regs[R_SP];
    unsigned long str = (sp - 0x400) & ~7UL;
    unsigned long newsp = (sp - 0x800) & ~7UL;

    size_t n = strlen(so) + 1;
    for (size_t i = 0; i < n; i += 4) {
        unsigned long w = 0;
        memcpy(&w, so + i, (n - i >= 4) ? 4 : (n - i));
        if (xptrace(PTRACE_POKETEXT, pid, (void *)(str + i), (void *)w,
                    "POKETEXT str") == -1) goto restore;
    }

    regs[0] = str;                 /* arg1: so path            */
    regs[1] = RTLD_NOW;            /* arg2: mode               */
    regs[R_SP] = newsp;
    regs[R_LR] = 0;                /* return to NULL -> SIGSEGV */
    regs[R_PC] = target & ~1UL;    /* Thumb entry, bit0 cleared */
    regs[R_CPSR] = saved[R_CPSR] | CPSR_T;
    if (xptrace(PTRACE_SETREGS, pid, 0, regs, "SETREGS call") == -1) goto restore;
    if (xptrace(PTRACE_CONT, pid, 0, 0, "CONT call") == -1) goto restore;

    if (waitpid(pid, &st, 0) < 0) { perror("waitpid call"); goto restore; }
    if (WIFSTOPPED(st)) {
        int sig = WSTOPSIG(st);
        unsigned long rr[NREG];
        if (xptrace(PTRACE_GETREGS, pid, 0, rr, "GETREGS result") != -1) {
            printf("a5kinject: returned, stopsig=%d pc=0x%lx handle=0x%lx\n",
                   sig, rr[R_PC], rr[0]);
            if (rr[0] == 0)
                fprintf(stderr, "a5kinject: WARNING dlopen returned NULL\n");
        }
    } else if (WIFEXITED(st)) {
        fprintf(stderr, "a5kinject: TARGET EXITED (code %d) during call!\n",
                WEXITSTATUS(st));
        return 1;
    } else if (WIFSIGNALED(st)) {
        fprintf(stderr, "a5kinject: TARGET KILLED by signal %d during call!\n",
                WTERMSIG(st));
        return 1;
    }

restore:
    xptrace(PTRACE_SETREGS, pid, 0, saved, "SETREGS restore");
detach:
    xptrace(PTRACE_DETACH, pid, 0, 0, "DETACH");
    printf("a5kinject: detached\n");
    return 0;
}

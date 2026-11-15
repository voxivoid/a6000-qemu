/*
 * mset_so.c - Track B route (b) §15-B: in-process requestModelExecute invoke.
 *
 * Loaded into im.elf by a5kinject.  viewManagerIf::requestModelExecute
 * (libObj 0x3948E0, Thumb) uses the process-global AbstractUtilityManager
 * singleton (dword_EEDDDC) and ends in EventManager::push(mgr,ev,true)
 * (sub_70A770) -- a cross-thread-safe ENQUEUE; the model is then executed by
 * the appFw pump on its own thread.  So a worker-thread call is a safe enqueue
 * (resolves TRACKB §13.4 blocker2 without a trampoline).
 *
 * Steps, incremental + logged (so a crash still leaves the earlier results):
 *   1. read funcEventMode (0x394924)                       -> before
 *   2. requestModelExecute("model/ZZPROBE", 0, NULL)        -> non-matching
 *      model id: exercises create-event + push + pump dispatch with NO engine
 *      effect (no handler). Proves the invoke path runs safely from our code.
 *   3. read funcEventMode                                   -> unchanged check
 *   4. requestModelExecute("@M0E9", 0xD05, NULL)            -> one real stock
 *      command (nullptr ParamList), appFw pump processes it.
 *   5. read funcEventMode                                   -> after
 *
 * Build: arm-linux-gnueabi-gcc -std=gnu99 -O2 -fPIC -shared
 *          -fno-stack-protector -o mset.so tools/mset_so.c -lpthread
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#define REQ_MODEL_EXECUTE_OFF 0x3948E0UL   /* viewManagerIf::requestModelExecute */
#define GET_MODE_OFF          0x394924UL   /* viewManagerIf::getCurrentFuncEventMode */

static void logline(const char *s)
{
    int fd = open("/android/a5k/mset.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) { (void)!write(fd, s, strlen(s)); close(fd); }
}

static unsigned long find_base(const char *name)
{
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0) return 0;
    static char buf[1 << 20];
    int off = 0, r;
    while (off < (int)sizeof buf - 1 &&
           (r = read(fd, buf + off, sizeof buf - 1 - off)) > 0) off += r;
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

typedef int (*mode_fn)(void *);
typedef int (*req_fn)(const char *name, unsigned long cmd, void *paramlist);

static void nap(int s) { struct timespec t = { s, 0 }; nanosleep(&t, 0); }

static void *worker(void *arg)
{
    (void)arg;
    unsigned long base = find_base("libObj.so");
    if (!base) { logline("mset: libObj base NOT found\n"); return 0; }
    mode_fn getmode = (mode_fn)(base + GET_MODE_OFF + 1);
    req_fn  reqexec = (req_fn)(base + REQ_MODEL_EXECUTE_OFF + 1);
    char m[96];
    snprintf(m, sizeof m, "mset: libObj base=0x%lx reqExec=0x%lx\n",
             base, base + REQ_MODEL_EXECUTE_OFF); logline(m);

    int b = getmode((void *)0);
    snprintf(m, sizeof m, "mset: 1 funcEventMode before=%d\n", b); logline(m);

    logline("mset: 2 requestModelExecute(\"model/ZZPROBE\",0,NULL) ...\n");
    int r1 = reqexec("model/ZZPROBE", 0, (void *)0);
    snprintf(m, sizeof m, "mset: 2 returned=%d (non-matching, safe path)\n", r1);
    logline(m);
    nap(1);
    int a1 = getmode((void *)0);
    snprintf(m, sizeof m, "mset: 3 funcEventMode after-probe=%d\n", a1); logline(m);

    logline("mset: 4 requestModelExecute(\"@M0E9\",0xD05,NULL) ...\n");
    int r2 = reqexec("@M0E9", 0xD05, (void *)0);
    snprintf(m, sizeof m, "mset: 4 returned=%d\n", r2); logline(m);
    nap(1);
    int a2 = getmode((void *)0);
    snprintf(m, sizeof m, "mset: 5 funcEventMode after-cmd=%d\n", a2); logline(m);

    logline("mset: worker done\n");
    return 0;
}

__attribute__((constructor))
static void mset_ctor(void)
{
    logline("mset: ctor in im.elf\n");
    pthread_t t;
    if (pthread_create(&t, 0, worker, 0) == 0) pthread_detach(t);
    else logline("mset: pthread_create FAIL\n");
}

/*
 * set2_so.c - Track B route (b) §15.1: OBSERVABLE reversible in-process SET.
 *
 * Loaded into im.elf by a5kinject.  Builds a one-IntParam MWF::ParamList and
 * calls viewManagerIf::requestModelExecute("model/CAMERA", 0x55, list) to set
 * camera param 0x29D -- a clean single-param reference (viewUnified2 sub_172F20,
 * no Backup write, RAM-only).  ParamList ABI (TRACKB §13.2):
 *   ParamList::ParamList  libObj 0x70912E   ParamList::add   libObj 0x7095D6
 *   IntParam ctor sub_EA468 viewUnified2 0xEA468 (vtable off_40B6D0)
 *   viewManagerIf::requestModelExecute libObj 0x3948E0 (global singleton)
 * requestModelExecute enqueues (EventManager::push); the appFw pump applies it.
 *
 * Sequence (worker), with external screendumps between the sleeps:
 *   t0  sleep 4   -> "before" dump
 *   set 0x29D = 1 -> sleep 5 -> "after-set" dump
 *   set 0x29D = 0 -> sleep 4 -> "after-revert" dump  (reversible)
 * The stock reference sets 0, so 0 is a known-safe value; 1 is the toggle.
 *
 * RAM-only (libraries' own code, heap).  No flash writes.  The event takes
 * ownership of the ParamList (stock leaks it too), so we do NOT free it.
 *
 * Build: arm-linux-gnueabi-gcc -std=gnu99 -O2 -fPIC -shared
 *          -fno-stack-protector -o set2.so tools/set2_so.c -lpthread
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#define PL_CTOR_OFF   0x70912EUL   /* libObj ParamList::ParamList */
#define PL_ADD_OFF    0x7095D6UL   /* libObj ParamList::add(id,ParamBase*) */
#define REQEXEC_OFF   0x3948E0UL   /* libObj viewManagerIf::requestModelExecute */
#define INTPARAM_OFF  0x0EA468UL   /* viewUnified2 sub_EA468 (IntParam ctor) */
#define PARAM_ID      0x29DU
#define MODEL_CMD     0x55U

static void logline(const char *s)
{
    int fd = open("/android/a5k/set2.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
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

typedef void *(*plctor_fn)(void *list);
typedef void  (*pladd_fn)(void *list, unsigned long id, void *param);
typedef void *(*intp_fn)(void *p, int val);
typedef int   (*req_fn)(const char *name, unsigned long cmd, void *list);

static unsigned long g_lib, g_vu;

static void do_set(int value)
{
    plctor_fn pl_ctor = (plctor_fn)(g_lib + PL_CTOR_OFF + 1);
    pladd_fn  pl_add  = (pladd_fn)(g_lib + PL_ADD_OFF + 1);
    req_fn    reqexec = (req_fn)(g_lib + REQEXEC_OFF + 1);
    intp_fn   intp    = (intp_fn)(g_vu + INTPARAM_OFF + 1);

    void *list = malloc(8);
    pl_ctor(list);
    void *p = malloc(0x10);
    intp(p, value);
    pl_add(list, PARAM_ID, p);
    int r = reqexec("model/CAMERA", MODEL_CMD, list);
    char m[80]; snprintf(m, sizeof m,
        "set2: requestModelExecute(model/CAMERA,0x55, [0x29D=%d]) ret=%d\n",
        value, r); logline(m);
    /* event owns the list; do not free (matches stock) */
}

static void nap(int s) { struct timespec t = { s, 0 }; nanosleep(&t, 0); }

static void *worker(void *arg)
{
    (void)arg;
    g_lib = find_base("libObj.so");
    g_vu  = find_base("viewUnified2.so");
    if (!g_lib || !g_vu) { logline("set2: base NOT found\n"); return 0; }
    char m[96]; snprintf(m, sizeof m, "set2: libObj=0x%lx viewUnified2=0x%lx\n",
                         g_lib, g_vu); logline(m);

    logline("set2: t0 before (dump now)\n");
    nap(4);
    do_set(0);                 /* known-safe stock value: ABI smoke test */
    logline("set2: set=0 (ABI test) done\n");
    nap(3);
    do_set(1);                 /* the toggle */
    logline("set2: set=1 done (dump now)\n");
    nap(5);
    do_set(0);                 /* revert (reversible) */
    logline("set2: set=0 revert done (dump now)\n");
    nap(4);
    logline("set2: worker done\n");
    return 0;
}

__attribute__((constructor))
static void set2_ctor(void)
{
    logline("set2: ctor in im.elf\n");
    pthread_t t;
    if (pthread_create(&t, 0, worker, 0) == 0) pthread_detach(t);
    else logline("set2: pthread_create FAIL\n");
}

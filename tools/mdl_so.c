/*
 * mdl_so.c - Track B route (b) §2: in-process engine/view-state read via the
 *            stock viewManagerIf path, from inside im.elf.  RAM-only.
 *
 * Loaded into im.elf by a5kinject.  Its worker resolves libObj's base from
 * /proc/self/maps and calls the stock
 *     viewManagerIf::getCurrentFuncEventMode()   (libObj 0x394924, Thumb)
 * which internally reads the process-global ViewManager singleton
 * (dword_EEDDD8) and returns the current function/event mode of the top view
 * (sub_70A734: walk the view stack -> virtual getMode on the active view).
 * This is the safest member of the same family as getModelInfo (0x39485C) /
 * requestModelExecute (0x3948E0), which reach the engine agent through the
 * global singletons (ViewManager dword_EEDDD8 / AbstractUtilityManager
 * dword_EEDDDC) -- no live view object needed.
 *
 * It samples the mode ~20x over 20 s so an external key press (MENU) can be
 * injected mid-run to show the in-process read TRACKS live state -> the
 * in-process model/view path reports the same state the UI/engine is in.
 *
 * Build: arm-linux-gnueabi-gcc -std=gnu99 -O2 -fPIC -shared
 *          -fno-stack-protector -o mdl.so tools/mdl_so.c -lpthread
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#define GET_FUNC_EVENT_MODE_OFF 0x394924UL   /* viewManagerIf::getCurrentFuncEventMode (Thumb) */

static void logline(const char *s)
{
    int fd = open("/android/a5k/mdl.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
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

static void *worker(void *arg)
{
    (void)arg;
    unsigned long base = find_base("libObj.so");
    if (!base) { logline("mdl: libObj base NOT found\n"); return 0; }
    mode_fn f = (mode_fn)(base + GET_FUNC_EVENT_MODE_OFF + 1); /* +1 = Thumb */
    { char m[80]; snprintf(m, sizeof m,
        "mdl: libObj base=0x%lx getFuncEventMode=0x%lx\n",
        base, base + GET_FUNC_EVENT_MODE_OFF); logline(m); }

    struct timespec ts = { 1, 0 };
    for (int i = 0; i < 20; i++) {
        int mode = f((void *)0);          /* this is ignored (reads global) */
        char m[64]; snprintf(m, sizeof m, "mdl: t=%2d funcEventMode=%d\n", i, mode);
        logline(m);
        nanosleep(&ts, 0);
    }
    logline("mdl: worker done\n");
    return 0;
}

__attribute__((constructor))
static void mdl_ctor(void)
{
    logline("mdl: ctor in im.elf\n");
    pthread_t t;
    if (pthread_create(&t, 0, worker, 0) == 0) pthread_detach(t);
    else logline("mdl: pthread_create FAIL\n");
}

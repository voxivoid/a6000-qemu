/*
 * a5ktrace — QEMU TCG plugin: log guest state when given addresses execute.
 * Debug tool for the sony-a5000 machine (not hardware; the guest cannot see it).
 *
 * Unlike gdb breakpoint commands it never loses events when several vCPUs hit
 * at once.  For each hit it prints the vCPU, pc, r0-r3, lr, the Linux task
 * name (thread_info = sp & ~0x1FFF, task = ti+12, comm = task+0x1E8 on this
 * 3.0 kernel) and the first 4 words at r1.
 *
 *   -plugin emulator/tools/liba5ktrace.dylib,pc=0x3f04cbd4,r0=0x920021,pc=...
 * "words=N" (<=20) words dumped at r1.
 * "x=V" (repeatable) drops hits with r0 == V at every probe.
 * "comm=NAME" (repeatable) keeps only hits from tasks whose name starts NAME.
 * "str=N" also prints the C string at r1+N (e.g. osal text messages) for the
 * probes that follow; "s=N" after a "pc=" sets it for that probe only.
 * "r0=" after a "pc=" filters that address on r0 (repeatable per pc: any match).
 * "user=0x<base>" is not supported: user-space addresses must be passed resolved.
 * Output goes to the QEMU log (-D file).
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define MAXP 64
static struct { uint64_t pc; uint32_t r0[8]; int nr0; int soff; } probes[MAXP];
static int nprobes;
static int nwords = 4;
static int stroff = -1;   /* "str=N": also print the C string at r1+N */
static uint32_t xr0[32];   /* "x=": r0 values ignored at every probe */
static int nxr0;
static char *comms[8];     /* "comm=": only log tasks whose name starts so */
static int ncomms;
/*
 * "fn=PATH": a reloadable set of extra probe addresses (one hex address per
 * line, optional second column = label).  The file is re-read when its mtime
 * changes (checked at most every 500 ms from the translator); the plugin then
 * resets itself so that already translated blocks are instrumented again.
 * Each address logs at most "fncap=N" hits (default 4) as one "A5KF" line.
 * Meant for user-space code whose load address is only known at run time.
 * "fnwords=N" adds N words at r0, "fnstr=K" the C string at *(r0 + 4K),
 * "fnr1s=1" the C string at r1.
 */
static char *fnpath;
static int fncap = 4;
static int fnwords;           /* "fnwords=N": also dump N words at r0 */
static int fnstr = -1;        /* "fnstr=K": print the string *(r0 + 4K) */
static int fnr1s;             /* "fnr1s=1": print the string at r1 */
static GHashTable *fnset;          /* pc -> FnProbe */
static gint64 fn_mtime, fn_checked;
static qemu_plugin_id_t self_id;
typedef struct { uint64_t pc; int hits; char label[48]; } FnProbe;
/*
 * "libobj=1" (2 = also list all im.elf file mappings): find the load address of /usr/lib/libObj.so in im.elf without a
 * debugger.  At the first probe hit from an im.elf thread ("Appframework"),
 * walk task->mm's VMA list (3.0 kernel: task+0xCC mm, mm+0 mmap, vma +4 start,
 * +8 end, +12 next, +72 pgoff) for the 0xE0E000-byte text mapping at pgoff 0.
 * Then "poke=OFF:VAL" (repeatable) writes byte VAL at base+OFF (diagnostic
 * switches such as appFw's debug print byte 0xF13D88), and fn= lines that
 * start with '+' are offsets relative to this base; a fn= line "!OFF VAL"
 * queues such a poke at run time.
 */
static int want_libobj;
static uint32_t libobj_base;
static uint32_t pokes[8][2];
static int npokes;
static bool pokes_pending;
static bool vma_rescan;            /* fn= line "?": list im.elf VMAs again */

typedef struct {
    struct qemu_plugin_register *r[16];
} VcpuRegs;
static VcpuRegs vregs[8];

static uint32_t rd_reg(int cpu, int n)
{
    GByteArray *b = g_byte_array_new();
    uint32_t v = 0;
    if (vregs[cpu].r[n] && qemu_plugin_read_register(vregs[cpu].r[n], b) >= 4) {
        memcpy(&v, b->data, 4);
    }
    g_byte_array_free(b, TRUE);
    return v;
}

static uint32_t rd32(uint32_t va)
{
    GByteArray *b = g_byte_array_new();
    uint32_t v = 0;
    if (qemu_plugin_read_memory_vaddr(va, b, 4)) {
        memcpy(&v, b->data, 4);
    }
    g_byte_array_free(b, TRUE);
    return v;
}

static void rdstr(uint32_t va, char *out, int n)
{
    GByteArray *b = g_byte_array_new();
    out[0] = 0;
    if (qemu_plugin_read_memory_vaddr(va, b, n - 1)) {
        memcpy(out, b->data, n - 1);
        out[n - 1] = 0;
    }
    g_byte_array_free(b, TRUE);
}

static void find_libobj(unsigned int cpu)
{
    uint32_t sp = rd_reg(cpu, 13);
    char comm[17] = "";
    if (sp < 0x40000000u) {
        return;
    }
    uint32_t task = rd32((sp & ~0x1fffu) + 12);
    if (!task) {
        return;
    }
    rdstr(task + 0x1e8, comm, sizeof(comm));
    if (strncmp(comm, "Appframework", 12)) {
        return;
    }
    uint32_t mm = rd32(task + 0xcc), v = mm ? rd32(mm) : 0;
    for (int n = 0; v && n < 2000; n++, v = rd32(v + 12)) {
        uint32_t a = rd32(v + 4), e = rd32(v + 8);
        if (want_libobj > 1 && rd32(v + 72) == 0 && rd32(v + 76)) {
            /* "libobj=2": list every file mapping at offset 0 */
            g_autofree char *t = g_strdup_printf("A5KB vma %08x size %x\n",
                                                 a, e - a);
            qemu_plugin_outs(t);
        }
        if (e - a == 0xe0e000 && rd32(v + 72) == 0) {
            libobj_base = a;
            if (want_libobj < 2) {
                break;
            }
        }
    }
    if (!libobj_base) {
        return;
    }
    g_autofree char *m = g_strdup_printf("A5KB libObj base %08x\n",
                                         libobj_base);
    qemu_plugin_outs(m);
    pokes_pending = npokes > 0;
    fn_mtime = 0;                       /* reload fn= with the base */
    fn_checked = 0;
}

/* apply queued pokes from an im.elf thread (its user mapping is live) */
static void do_pokes(unsigned int cpu)
{
    uint32_t sp = rd_reg(cpu, 13);
    char comm[17] = "";
    if (sp < 0x40000000u) {
        return;
    }
    uint32_t task = rd32((sp & ~0x1fffu) + 12);
    rdstr(task + 0x1e8, comm, sizeof(comm));
    if (strncmp(comm, "Appframework", 12)) {
        return;
    }
    pokes_pending = false;
    for (int k = 0; k < npokes; k++) {
        GByteArray *b = g_byte_array_new();
        uint8_t val = pokes[k][1];
        g_byte_array_append(b, &val, 1);
        bool ok = qemu_plugin_write_memory_vaddr(libobj_base + pokes[k][0], b);
        g_byte_array_free(b, TRUE);
        g_autofree char *t = g_strdup_printf("A5KB poke +%x = %02x %s\n",
                                             pokes[k][0], val, ok ? "ok" : "FAILED");
        qemu_plugin_outs(t);
    }
    npokes = 0;
}

static void hit(unsigned int cpu, void *udata)
{
    if (want_libobj && (!libobj_base || vma_rescan)) {
        uint32_t keep = libobj_base;
        int w = want_libobj;
        if (vma_rescan) {
            want_libobj = 2;
            libobj_base = 0;
        }
        find_libobj(cpu);
        want_libobj = w;
        if (libobj_base) {
            vma_rescan = false;
        } else {
            libobj_base = keep;
        }
    }
    if (pokes_pending && libobj_base) {
        do_pokes(cpu);
    }
    int i = GPOINTER_TO_INT(udata);
    uint32_t r[4];
    for (int k = 0; k < 4; k++) {
        r[k] = rd_reg(cpu, k);
    }
    if (probes[i].nr0) {
        int ok = 0;
        for (int k = 0; k < probes[i].nr0; k++) {
            ok |= r[0] == probes[i].r0[k];
        }
        if (!ok) {
            return;
        }
    }
    for (int k = 0; k < nxr0; k++) {
        if (r[0] == xr0[k]) {
            return;
        }
    }
    uint32_t sp = rd_reg(cpu, 13), lr = rd_reg(cpu, 14), task = 0;
    char comm[17] = "";
    if (sp >= 0x40000000u) {
        task = rd32((sp & ~0x1fffu) + 12);
        if (task) {
            rdstr(task + 0x1e8, comm, sizeof(comm));
        }
    }
    if (ncomms) {
        int ok = 0;
        for (int k = 0; k < ncomms; k++) {
            ok |= !strncmp(comm, comms[k], strlen(comms[k]));
        }
        if (!ok) {
            return;
        }
    }
    uint32_t m[20] = {0};
    if (r[1] >= 0x1000) {
        for (int k = 0; k < nwords; k++) {
            m[k] = rd32(r[1] + 4 * k);
        }
    }
    g_autofree char *s = g_strdup_printf(
        "A5KT cpu%u pc=%08" PRIx64 " r0=%08x r1=%08x r2=%08x r3=%08x lr=%08x "
        "[%s] t=%08x m=", cpu, probes[i].pc, r[0], r[1], r[2], r[3], lr, comm,
        task);
    GString *g = g_string_new(s);
    for (int k = 0; k < nwords; k++) {
        g_string_append_printf(g, "%08x ", m[k]);
    }
    if (probes[i].soff >= 0 && r[1] >= 0x1000) {
        char t[97];
        rdstr(r[1] + probes[i].soff, t, sizeof(t));
        for (char *c = t; *c; c++) {
            if (*c == '\n' || *c == '\r') {
                *c = ' ';
            }
        }
        g_string_append_printf(g, "s=\"%s\"", t);
    }
    g_string_append_c(g, '\n');
    qemu_plugin_outs(g->str);
    g_string_free(g, TRUE);
}

static void fn_hit(unsigned int cpu, void *udata)
{
    FnProbe *f = udata;
    if (f->hits >= fncap) {
        return;
    }
    f->hits++;
    uint32_t r0 = rd_reg(cpu, 0);
    GString *g = g_string_new(NULL);
    g_string_printf(g,
        "A5KF cpu%u pc=%08" PRIx64 " r0=%08x r1=%08x r2=%08x r3=%08x lr=%08x "
        "n=%d %s", cpu, f->pc, r0, rd_reg(cpu, 1),
        rd_reg(cpu, 2), rd_reg(cpu, 3), rd_reg(cpu, 14), f->hits, f->label);
    if (r0 >= 0x1000) {
        if (fnwords) {
            g_string_append(g, " w=");
            for (int k = 0; k < MIN(fnwords, 16); k++) {
                g_string_append_printf(g, "%08x ", rd32(r0 + 4 * k));
            }
        }
        if (fnstr >= 0) {
            char t[64];
            rdstr(rd32(r0 + 4 * fnstr), t, sizeof(t));
            g_string_append_printf(g, " s=\"%s\"", t);
        }
    }
    if (fnr1s && rd_reg(cpu, 1) >= 0x1000) {
        char t[64];
        rdstr(rd_reg(cpu, 1), t, sizeof(t));
        g_string_append_printf(g, " s1=\"%s\"", t);
    }
    g_string_append_c(g, '\n');
    qemu_plugin_outs(g->str);
    g_string_free(g, TRUE);
}

static void tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb);

static void fn_reset_done(qemu_plugin_id_t id)
{
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans);
}

/* re-read the fn= file if it changed; returns true if a reset was requested */
static bool fn_reload(void)
{
    gint64 now = g_get_monotonic_time();
    GStatBuf st;
    if (!fnpath || now - fn_checked < 500000) {
        return false;
    }
    fn_checked = now;
    if (g_stat(fnpath, &st) != 0 || (gint64)st.st_mtime * 1000000 +
        st.st_mtimespec.tv_nsec / 1000 == fn_mtime) {
        return false;
    }
    fn_mtime = (gint64)st.st_mtime * 1000000 + st.st_mtimespec.tv_nsec / 1000;
    g_autofree char *txt = NULL;
    if (!g_file_get_contents(fnpath, &txt, NULL, NULL)) {
        return false;
    }
    GHashTable *t = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                          NULL, g_free);
    g_auto(GStrv) lines = g_strsplit(txt, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        char *e, *l = lines[i];
        if (!strncmp(l, "#cap ", 5)) {       /* "#cap N": per-probe hit cap */
            fncap = atoi(l + 5);
            continue;
        }
        if (*l == '?') {
            vma_rescan = true;
            continue;
        }
        if (*l == '!' && npokes < 8) {      /* "!OFF VAL": poke libObj+OFF */
            pokes[npokes][0] = g_ascii_strtoull(l + 1, &e, 16);
            pokes[npokes++][1] = g_ascii_strtoull(e, NULL, 16);
            pokes_pending = true;
            continue;
        }
        bool rel = *l == '+';
        if (rel && !libobj_base) {
            continue;
        }
        uint64_t pc = g_ascii_strtoull(l + rel, &e, 16);
        if (e == l + rel) {
            continue;
        }
        if (rel) {
            pc += libobj_base;
        }
        FnProbe *f = g_new0(FnProbe, 1);
        f->pc = pc & ~1ull;
        g_strlcpy(f->label, g_strstrip(e), sizeof(f->label));
        g_hash_table_replace(t, &f->pc, f);
    }
    /* old table is leaked on purpose: callbacks may still point into it */
    fnset = t;
    g_autofree char *m = g_strdup_printf("A5KF loaded %u probes from %s\n",
                                         g_hash_table_size(t), fnpath);
    qemu_plugin_outs(m);
    qemu_plugin_reset(self_id, fn_reset_done);
    return true;
}

static void tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    if (fn_reload()) {
        return;
    }
    size_t n = qemu_plugin_tb_n_insns(tb);
    for (size_t j = 0; j < n; j++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, j);
        uint64_t va = qemu_plugin_insn_vaddr(insn);
        for (int i = 0; i < nprobes; i++) {
            if (probes[i].pc == va) {
                qemu_plugin_register_vcpu_insn_exec_cb(
                    insn, hit, QEMU_PLUGIN_CB_R_REGS, GINT_TO_POINTER(i));
            }
        }
        if (fnset) {
            FnProbe *f = g_hash_table_lookup(fnset, &va);
            if (f) {
                qemu_plugin_register_vcpu_insn_exec_cb(
                    insn, fn_hit, QEMU_PLUGIN_CB_R_REGS, f);
            }
        }
    }
}

static void vcpu_init(qemu_plugin_id_t id, unsigned int cpu)
{
    if (cpu >= 8) {
        return;
    }
    g_autoptr(GArray) regs = qemu_plugin_get_registers();
    for (guint i = 0; i < regs->len; i++) {
        qemu_plugin_reg_descriptor *d =
            &g_array_index(regs, qemu_plugin_reg_descriptor, i);
        int n = -1;
        if (d->name[0] == 'r' && d->name[1] >= '0' && d->name[1] <= '9') {
            n = atoi(d->name + 1);
        } else if (!strcmp(d->name, "sp")) {
            n = 13;
        } else if (!strcmp(d->name, "lr")) {
            n = 14;
        }
        if (n >= 0 && n < 16) {
            vregs[cpu].r[n] = d->handle;
        }
    }
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        if (!strncmp(argv[i], "words=", 6)) {
            nwords = MIN(20, atoi(argv[i] + 6));
        } else if (!strncmp(argv[i], "fn=", 3)) {
            fnpath = g_strdup(argv[i] + 3);
        } else if (!strncmp(argv[i], "libobj=", 7)) {
            want_libobj = atoi(argv[i] + 7);
        } else if (!strncmp(argv[i], "poke=", 5) && npokes < 8) {
            char *e;
            pokes[npokes][0] = g_ascii_strtoull(argv[i] + 5, &e, 16);
            pokes[npokes++][1] = *e == ':' ? g_ascii_strtoull(e + 1, NULL, 16) : 1;
        } else if (!strncmp(argv[i], "fnwords=", 8)) {
            fnwords = atoi(argv[i] + 8);
        } else if (!strncmp(argv[i], "fnr1s=", 6)) {
            fnr1s = atoi(argv[i] + 6);
        } else if (!strncmp(argv[i], "fnstr=", 6)) {
            fnstr = atoi(argv[i] + 6);
        } else if (!strncmp(argv[i], "fncap=", 6)) {
            fncap = atoi(argv[i] + 6);
        } else if (!strncmp(argv[i], "x=", 2) && nxr0 < 32) {
            xr0[nxr0++] = g_ascii_strtoull(argv[i] + 2, NULL, 0);
        } else if (!strncmp(argv[i], "comm=", 5) && ncomms < 8) {
            comms[ncomms++] = g_strdup(argv[i] + 5);
        } else if (!strncmp(argv[i], "str=", 4)) {
            stroff = atoi(argv[i] + 4);
        } else if (!strncmp(argv[i], "pc=", 3) && nprobes < MAXP) {
            probes[nprobes].soff = stroff;
            probes[nprobes++].pc = g_ascii_strtoull(argv[i] + 3, NULL, 0);
        } else if (!strncmp(argv[i], "s=", 2) && nprobes) {
            probes[nprobes - 1].soff = atoi(argv[i] + 2);
        } else if (!strncmp(argv[i], "r0=", 3) && nprobes &&
                   probes[nprobes - 1].nr0 < 8) {
            probes[nprobes - 1].r0[probes[nprobes - 1].nr0++] =
                g_ascii_strtoull(argv[i] + 3, NULL, 0);
        }
    }
    self_id = id;
    qemu_plugin_register_vcpu_init_cb(id, vcpu_init);
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans);
    return 0;
}

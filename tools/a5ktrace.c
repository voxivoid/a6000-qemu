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
 */
static char *fnpath;
static int fncap = 4;
static GHashTable *fnset;          /* pc -> FnProbe */
static gint64 fn_mtime, fn_checked;
static qemu_plugin_id_t self_id;
typedef struct { uint64_t pc; int hits; char label[48]; } FnProbe;

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

static void hit(unsigned int cpu, void *udata)
{
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
    uint32_t sp = rd_reg(cpu, 13), lr = rd_reg(cpu, 14);
    char comm[17] = "";
    if (sp >= 0x40000000u) {
        uint32_t task = rd32((sp & ~0x1fffu) + 12);
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
        "[%s] m=", cpu, probes[i].pc, r[0], r[1], r[2], r[3], lr, comm);
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
    g_autofree char *s = g_strdup_printf(
        "A5KF cpu%u pc=%08" PRIx64 " r0=%08x r1=%08x r2=%08x r3=%08x lr=%08x "
        "n=%d %s\n", cpu, f->pc, rd_reg(cpu, 0), rd_reg(cpu, 1),
        rd_reg(cpu, 2), rd_reg(cpu, 3), rd_reg(cpu, 14), f->hits, f->label);
    qemu_plugin_outs(s);
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
        char *e;
        uint64_t pc = g_ascii_strtoull(lines[i], &e, 16);
        if (e == lines[i]) {
            continue;
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

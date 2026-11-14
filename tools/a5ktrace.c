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
 * "r0=" after a "pc=" filters that address on r0 (repeatable per pc: any match).
 * "user=0x<base>" is not supported: user-space addresses must be passed resolved.
 * Output goes to the QEMU log (-D file).
 */
#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define MAXP 64
static struct { uint64_t pc; uint32_t r0[8]; int nr0; } probes[MAXP];
static int nprobes;
static int nwords = 4;

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
    uint32_t sp = rd_reg(cpu, 13), lr = rd_reg(cpu, 14);
    char comm[17] = "";
    if (sp >= 0x40000000u) {
        uint32_t task = rd32((sp & ~0x1fffu) + 12);
        if (task) {
            rdstr(task + 0x1e8, comm, sizeof(comm));
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
    g_string_append_c(g, '\n');
    qemu_plugin_outs(g->str);
    g_string_free(g, TRUE);
}

static void tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
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
        } else if (!strncmp(argv[i], "pc=", 3) && nprobes < MAXP) {
            probes[nprobes++].pc = g_ascii_strtoull(argv[i] + 3, NULL, 0);
        } else if (!strncmp(argv[i], "r0=", 3) && nprobes &&
                   probes[nprobes - 1].nr0 < 8) {
            probes[nprobes - 1].r0[probes[nprobes - 1].nr0++] =
                g_ascii_strtoull(argv[i] + 3, NULL, 0);
        }
    }
    qemu_plugin_register_vcpu_init_cb(id, vcpu_init);
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans);
    return 0;
}

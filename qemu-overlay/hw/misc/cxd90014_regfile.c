/*
 * Catch-all register file for not-yet-modelled CXD90014 I/O.
 *
 * Plain config registers read back what was last written, which is what
 * most Sony drivers expect (e.g. grm_gles BUG()s unless the GPU clock gate
 * 0xF3400080 reads back 0x15).  Unwritten registers read 0.  Every access is
 * logged under -d unimp so unmodelled blocks remain visible; blocks that need
 * real behaviour get their own model mapped on top (this device is mapped at
 * priority -1000, like unimplemented-device).
 *
 * Storage is sparse (hash of 32-bit words) so huge windows are cheap.
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "qemu/log.h"
#include "qemu/cutils.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_CXD_REGFILE "cxd90014-regfile"
OBJECT_DECLARE_SIMPLE_TYPE(CXDRegfileState, CXD_REGFILE)

struct CXDRegfileState {
    SysBusDevice parent_obj;
    MemoryRegion mr;
    GHashTable *regs;
    uint64_t size;
    uint64_t base;          /* for log messages only */
    char *name;
    bool clkgen;        /* PLL/clock-generator semantics, see below */
    char *init;         /* "off=val,..." reset values */
    char *autoclear;    /* "off=mask,...": bits that self-clear on write */
    GHashTable *acl;
};

static void parse_pairs(const char *spec, GHashTable *h)
{
    if (!spec) {
        return;
    }
    g_auto(GStrv) it = g_strsplit(spec, ",", -1);
    for (int i = 0; it[i]; i++) {
        uint64_t o, v;
        char *eq = strchr(it[i], '=');
        if (!eq) {
            continue;
        }
        *eq = 0;
        if (qemu_strtou64(it[i], NULL, 0, &o) < 0 ||
            qemu_strtou64(eq + 1, NULL, 0, &v) < 0) {
            continue;
        }
        g_hash_table_insert(h, GUINT_TO_POINTER((guint)(o & ~3ULL)),
                            GUINT_TO_POINTER((guint)v));
    }
}

static uint64_t rf_read(void *opaque, hwaddr off, unsigned size)
{
    CXDRegfileState *s = opaque;
    hwaddr w = off & ~3ULL;
    uint32_t v = GPOINTER_TO_UINT(g_hash_table_lookup(s->regs,
                                  GUINT_TO_POINTER((guint)w)));
    /*
     * clkgen: CXD90014 clock generator blocks (kernel clk table
     * 0xF2A01000/02000/11000/12000/21000/22000, 0xF2901000, 0xF2A31000/
     * 32000).  av-cam.bin @0x438bccd4: write +0x160 = 3, poll +0x004 bit0,
     * write +0x030 = 1, poll +0x030 == 0.  Model: lock bit always set,
     * update request self-clears (hypothesis consistent with that code).
     */
    if (s->clkgen && w == 0x004) {
        v |= 1;
    } else if (s->clkgen && w == 0x030) {
        v = 0;
    }
    v >>= (off & 3) * 8;
    if (size < 4) {
        v &= (1u << (size * 8)) - 1;
    }
    qemu_log_mask(LOG_UNIMP, "%s: rd%u 0x%08" HWADDR_PRIx " -> 0x%x\n",
                  s->name, size, s->base + off, v);
    return v;
}

static void rf_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    CXDRegfileState *s = opaque;
    hwaddr w = off & ~3ULL;
    uint32_t old = GPOINTER_TO_UINT(g_hash_table_lookup(s->regs,
                                    GUINT_TO_POINTER((guint)w)));
    unsigned sh = (off & 3) * 8;
    uint32_t mask = size >= 4 ? 0xffffffffu : ((1u << (size * 8)) - 1) << sh;
    uint32_t nv = (old & ~mask) | (((uint32_t)v << sh) & mask);
    nv &= ~GPOINTER_TO_UINT(g_hash_table_lookup(s->acl,
                            GUINT_TO_POINTER((guint)w)));
    g_hash_table_insert(s->regs, GUINT_TO_POINTER((guint)w),
                        GUINT_TO_POINTER(nv));
    qemu_log_mask(LOG_UNIMP, "%s: wr%u 0x%08" HWADDR_PRIx " = 0x%" PRIx64
                  "\n", s->name, size, s->base + off, v);
}

static const MemoryRegionOps rf_ops = {
    .read = rf_read, .write = rf_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void rf_reset(DeviceState *dev)
{
    CXDRegfileState *s = CXD_REGFILE(dev);
    g_hash_table_remove_all(s->regs);
    parse_pairs(s->init, s->regs);
}

static void rf_realize(DeviceState *dev, Error **errp)
{
    CXDRegfileState *s = CXD_REGFILE(dev);
    s->regs = g_hash_table_new(g_direct_hash, g_direct_equal);
    s->acl = g_hash_table_new(g_direct_hash, g_direct_equal);
    parse_pairs(s->autoclear, s->acl);
    memory_region_init_io(&s->mr, OBJECT(dev), &rf_ops, s,
                          s->name ? s->name : "regfile", s->size);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mr);
}

static const Property rf_props[] = {
    DEFINE_PROP_UINT64("size", CXDRegfileState, size, 0x1000),
    DEFINE_PROP_UINT64("base", CXDRegfileState, base, 0),
    DEFINE_PROP_STRING("name", CXDRegfileState, name),
    DEFINE_PROP_BOOL("clkgen", CXDRegfileState, clkgen, false),
    DEFINE_PROP_STRING("init", CXDRegfileState, init),
    DEFINE_PROP_STRING("autoclear", CXDRegfileState, autoclear),
};

static void rf_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = rf_realize;
    device_class_set_legacy_reset(dc, rf_reset);
    device_class_set_props(dc, rf_props);
}

static const TypeInfo rf_info = {
    .name = TYPE_CXD_REGFILE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CXDRegfileState),
    .class_init = rf_class_init,
};

static void rf_register(void)
{
    type_register_static(&rf_info);
}
type_init(rf_register)

/*
 * CXD90014 16-channel memory DMA used by av-cam.bin (a5000).
 * Units at PA 0xF0102000 (ch0-7) and 0xF0103000 (ch8-15), GIC 173 + ch.
 * Recovered from av-cam: entry_pre copies its initialised data section
 * with it (ch6 regs +0x1C0..+0x1D0, 25 x 16 KiB), ISR 0x439172b0,
 * status 0x43917994, stop 0x43917bb0 (docs/RE_AVCAM_IRQ_BOOT.md):
 *   unit +0x00  per-channel done/IRQ status (bit ch)     +0x08 W clear
 *        +0x0C  per-channel error status                 +0x10 W clear
 *        +0x1C  busy bits (always 0: transfers finish at once)
 *   ch k +0x100 + 0x20*k: +0x00 src PA, +0x04 dst PA, +0x0C cfg (bits
 *        [11:0] = 32-bit word count, 0 = 4096), +0x10 ctl bit0 = start
 * Without this model the engine ran with an all-zero .data section.
 */
#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "system/address-spaces.h"
#include "trace.h"

#define TYPE_CXD_MDMA "cxd90014-mdma"
OBJECT_DECLARE_SIMPLE_TYPE(MdmaState, CXD_MDMA)

struct MdmaState {
    SysBusDevice parent_obj;
    MemoryRegion mr;
    uint32_t reg[0x1000 / 4];
    uint32_t status, err;
    qemu_irq irq[8];
};

static void mdma_update(MdmaState *s)
{
    for (int k = 0; k < 8; k++) {
        qemu_set_irq(s->irq[k], (s->status >> k) & 1);
    }
}

static void mdma_run(MdmaState *s, int k)
{
    uint32_t *c = &s->reg[(0x100 + 0x20 * k) / 4];
    uint32_t words = c[3] & 0xfff;
    size_t len = (size_t)(words ? words : 4096) * 4;
    g_autofree uint8_t *buf = g_malloc(len);
    address_space_read(&address_space_memory, c[0], MEMTXATTRS_UNSPECIFIED,
                       buf, len);
    address_space_write(&address_space_memory, c[1], MEMTXATTRS_UNSPECIFIED,
                        buf, len);
    trace_cxd_mdma_copy(k, c[0], c[1], len);
    c[4] &= ~1u;
    s->status |= 1u << k;
    mdma_update(s);
}

static uint64_t mdma_read(void *opaque, hwaddr off, unsigned size)
{
    MdmaState *s = opaque;
    switch (off) {
    case 0x00: return s->status;
    case 0x0C: return s->err;
    case 0x1C: return 0;
    }
    return s->reg[off / 4];
}

static void mdma_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    MdmaState *s = opaque;
    switch (off) {
    case 0x08:
        s->status &= ~v;
        mdma_update(s);
        return;
    case 0x10:
        s->err &= ~v;
        return;
    }
    s->reg[off / 4] = v;
    if (off >= 0x100 && off < 0x200 && (off & 0x1f) == 0x10 && (v & 1)) {
        mdma_run(s, (off - 0x100) / 0x20);
    }
}

static const MemoryRegionOps mdma_ops = {
    .read = mdma_read, .write = mdma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void mdma_reset(DeviceState *dev)
{
    MdmaState *s = CXD_MDMA(dev);
    memset(s->reg, 0, sizeof(s->reg));
    s->status = s->err = 0;
}

static void mdma_init(Object *obj)
{
    MdmaState *s = CXD_MDMA(obj);
    memory_region_init_io(&s->mr, obj, &mdma_ops, s, "cxd-mdma", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mr);
    for (int i = 0; i < 8; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[i]);
    }
}

static void mdma_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), mdma_reset);
}

static const TypeInfo mdma_info = {
    .name = TYPE_CXD_MDMA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MdmaState),
    .instance_init = mdma_init,
    .class_init = mdma_class_init,
};

static void mdma_register(void)
{
    type_register_static(&mdma_info);
}
type_init(mdma_register)

/*
 * Sony CXD90014 "BOSS" memory-to-memory DMA controller (a5000).
 *
 * Two units (UDIF dmac_ch0-3 @ PA 0xF0000000, dmac_ch4-7 @ 0xF0010000),
 * four channels each, GIC IRQ 189 + ch.  Layout from dmac.ko
 * (boss_lld_get_base_addr_dmac_ch, boss_hw_dmac_start_wait/_transfer,
 * boss_hw_dmac_process, boss_hw_dmac_unit_enable):
 *
 *   unit +0x00               unit enable (0x11000000 on / 0x10000000 off)
 *   ch   +0x10 + 0x30*(ch%4)
 *     +0x00..+0x14  first descriptor: {len-1, src, dst, cfg, cfg, next|1}
 *     +0x18         start/config (dmac.ko writes 0x11102003) -> go
 *     +0x1C         control (dmac.ko writes 17 before start)
 *     +0x20         status: low nibble 8 = done OK; written 0 to ack
 *
 * In-memory descriptors (32 B, built by nand.ko muminlzp_request):
 *   {len-1 (0xFFFFFFFF = empty), src phys, dst phys, cfg, cfg,
 *    next phys | 1 (bit0 = another descriptor follows), -, -}
 * cfg (0x30F00) is treated as plain incrementing byte copy [hypothesis].
 * Transfers complete instantly.
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "system/address-spaces.h"
#include "trace.h"

#define TYPE_CXD_DMAC "cxd90014-dmac"
OBJECT_DECLARE_SIMPLE_TYPE(CXDDmacState, CXD_DMAC)

#define NCH 4

struct CXDDmacState {
    SysBusDevice parent_obj;
    MemoryRegion mr;
    uint32_t enable;
    uint32_t ch[NCH][12];
    qemu_irq irq[NCH];
};

static uint32_t g32(hwaddr pa)
{
    return address_space_ldl_le(&address_space_memory, pa,
                                MEMTXATTRS_UNSPECIFIED, NULL);
}

static void copy_one(uint32_t lenm1, uint32_t src, uint32_t dst)
{
    if (lenm1 == 0xFFFFFFFF) {
        return;
    }
    uint32_t len = lenm1 + 1;
    g_autofree uint8_t *buf = g_malloc(len);
    address_space_read(&address_space_memory, src, MEMTXATTRS_UNSPECIFIED,
                       buf, len);
    address_space_write(&address_space_memory, dst, MEMTXATTRS_UNSPECIFIED,
                        buf, len);
}

static void run_chain(CXDDmacState *s, int c)
{
    uint32_t *r = s->ch[c];
    uint32_t n = 0;
    copy_one(r[0], r[1], r[2]);
    uint32_t next = r[5];
    while ((next & 1) && n++ < 65536) {
        hwaddr d = next & ~1u;
        copy_one(g32(d), g32(d + 4), g32(d + 8));
        next = g32(d + 20);
    }
    trace_cxd_dmac_done(c, n + 1);
    r[8] = 8;                       /* status: done OK */
    qemu_set_irq(s->irq[c], 1);
}

static uint64_t dmac_read(void *opaque, hwaddr off, unsigned size)
{
    CXDDmacState *s = opaque;
    if (off == 0) {
        return s->enable;
    }
    if (off >= 0x10 && off < 0x10 + 0x30 * NCH) {
        hwaddr o = off - 0x10;
        return s->ch[o / 0x30][(o % 0x30) / 4];
    }
    qemu_log_mask(LOG_UNIMP, "cxd-dmac: rd 0x%" HWADDR_PRIx "\n", off);
    return 0;
}

static void dmac_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    CXDDmacState *s = opaque;
    if (off == 0) {
        s->enable = v;
        return;
    }
    if (off >= 0x10 && off < 0x10 + 0x30 * NCH) {
        hwaddr o = off - 0x10;
        int c = o / 0x30, reg = (o % 0x30) / 4;
        s->ch[c][reg] = v;
        if (reg == 6 && v) {        /* start */
            run_chain(s, c);
        } else if (reg == 8 && v == 0) {
            qemu_set_irq(s->irq[c], 0);
        }
        return;
    }
    qemu_log_mask(LOG_UNIMP, "cxd-dmac: wr 0x%" HWADDR_PRIx " = 0x%"
                  PRIx64 "\n", off, v);
}

static const MemoryRegionOps dmac_ops = {
    .read = dmac_read, .write = dmac_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void dmac_reset(DeviceState *dev)
{
    CXDDmacState *s = CXD_DMAC(dev);
    s->enable = 0;
    memset(s->ch, 0, sizeof(s->ch));
}

static void dmac_init(Object *obj)
{
    CXDDmacState *s = CXD_DMAC(obj);
    memory_region_init_io(&s->mr, obj, &dmac_ops, s, "cxd-dmac", 0x10000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mr);
    for (int i = 0; i < NCH; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[i]);
    }
}

static void dmac_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), dmac_reset);
}

static const TypeInfo dmac_info = {
    .name = TYPE_CXD_DMAC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CXDDmacState),
    .instance_init = dmac_init,
    .class_init = dmac_class_init,
};

static void dmac_register(void)
{
    type_register_static(&dmac_info);
}
type_init(dmac_register)

/*
 * CXD90014 SIO (SPI-style serial) controller, 5 channels (a5000).
 * PA 0xF2405000 + 0x200*ch, GIC IRQ 201 + ch.  From av-cam.bin's driver
 * (docs/RE_AVCAM_IRQ_BOOT.md §4):
 *   +0x00  W config bits; R bit29 = done, bit25 = error;
 *          W |= bit31 clears the IRQ, |= bit30 clears error, &= ~bit29
 *   +0x10  0x01000000 = enable
 *   +0x100..+0x1FF  data FIFO window (TX words written / RX words read,
 *          av-cam 0x43918ea0 / 0x43918f5c)
 * Start = the driver sets +0x00 bit29 (0x43918df0); the hardware keeps it set
 * as "done" until the ISR clears it (status reader 0x43918e60).
 *   +0x04  transfer mode word; on ch2 bit29 = TX, bit28 = RX [H]
 *          (docs/RE_NFC.md section 1.2)
 * Model: a start completes immediately (done + IRQ).  Board-level slaves
 * (sub-micro on ch0/ch3, NFC tag on ch2) exchange the FIFO bytes; a channel
 * without a slave transfers nothing (receive data stays as is).
 */
#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "trace.h"
#include "hw/misc/cxd90014_sio.h"

#define TYPE_CXD_SIO "cxd90014-sio"
OBJECT_DECLARE_SIMPLE_TYPE(SioState, CXD_SIO)

#define NCH 5

struct SioState {
    SysBusDevice parent_obj;
    MemoryRegion mr;
    uint32_t reg[NCH][0x200 / 4];
    qemu_irq irq[NCH];
    CXDSioSlaveXfer slave[NCH];
    CXDSioSlaveXferDir slave_dir[NCH];
    void *slave_opaque[NCH];
};

void cxd90014_sio_attach(DeviceState *dev, int ch, CXDSioSlaveXfer fn,
                         void *opaque)
{
    SioState *s = CXD_SIO(dev);
    s->slave[ch] = fn;
    s->slave_opaque[ch] = opaque;
}

void cxd90014_sio_attach_dir(DeviceState *dev, int ch, CXDSioSlaveXferDir fn,
                             void *opaque)
{
    SioState *s = CXD_SIO(dev);
    s->slave_dir[ch] = fn;
    s->slave_opaque[ch] = opaque;
}

static uint64_t sio_read(void *opaque, hwaddr off, unsigned size)
{
    SioState *s = opaque;
    uint32_t v = s->reg[off / 0x200][(off % 0x200) / 4];
    trace_cxd_sio_rd(off / 0x200, off % 0x200, v);
    return v;
}

static void sio_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    SioState *s = opaque;
    int ch = off / 0x200;
    uint32_t *r = s->reg[ch];
    hwaddr o = off % 0x200;
    trace_cxd_sio_wr(ch, o, v);
    if (o == 0x00) {
        uint32_t st = r[0];
        if (v & 0x80000000u) {
            qemu_set_irq(s->irq[ch], 0);
        }
        if (v & 0x40000000u) {
            st &= ~0x02000000u;
        }
        /* config bits are written, the done bit is only cleared by an
         * explicit write without it */
        r[0] = (v & 0x1fffffffu) | (st & 0x02000000u) | (v & 0x20000000u);
        if ((v & 0x20000000u) && !(st & 0x20000000u)) {
            trace_cxd_sio_xfer(ch, r[0x0C / 4] >> 24);
            trace_cxd_sio_data(ch, r[0x100 / 4], r[0x104 / 4], r[0x108 / 4],
                               r[0x10C / 4]);
            if (s->slave[ch] || s->slave_dir[ch]) {
                int len = (r[0x0C / 4] >> 24) + 1;
                uint8_t buf[0x100];
                for (int i = 0; i < 0x100; i++) {
                    buf[i] = r[0x100 / 4 + i / 4] >> (8 * (i % 4));
                }
                if (s->slave_dir[ch]) {
                    s->slave_dir[ch](s->slave_opaque[ch], ch, r[0x04 / 4],
                                     buf, len);
                } else {
                    s->slave[ch](s->slave_opaque[ch], ch, buf, len);
                }
                for (int i = 0; i < 0x100; i += 4) {
                    r[(0x100 + i) / 4] = buf[i] | buf[i + 1] << 8 |
                                         buf[i + 2] << 16 | buf[i + 3] << 24;
                }
            }
            qemu_set_irq(s->irq[ch], 1);        /* start -> done at once */
        }
        return;
    }
    r[o / 4] = v;

}

static const MemoryRegionOps sio_ops = {
    .read = sio_read, .write = sio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void sio_reset(DeviceState *dev)
{
    memset(CXD_SIO(dev)->reg, 0, sizeof(CXD_SIO(dev)->reg));
}

static void sio_init(Object *obj)
{
    SioState *s = CXD_SIO(obj);
    memory_region_init_io(&s->mr, obj, &sio_ops, s, "cxd-sio", 0x200 * NCH);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mr);
    for (int i = 0; i < NCH; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[i]);
    }
}

static void sio_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), sio_reset);
}

static const TypeInfo sio_info = {
    .name = TYPE_CXD_SIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SioState),
    .instance_init = sio_init,
    .class_init = sio_class_init,
};

static void sio_register(void)
{
    type_register_static(&sio_info);
}
type_init(sio_register)

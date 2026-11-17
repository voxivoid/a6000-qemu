/*
 * CXD90014 ADC unit: PA 0xF2406000 + 0x1000*u, GIC 215 + u.
 * From av-cam.bin (docs/reference/RE_AVCAM_IRQ_BOOT.md, 0x4391722c result reader):
 *   +0x00 channel mask   +0x04 bit0 start   +0x08 mode (2 = continuous [?])
 *   +0x0C bit0 IRQ status, bit4 ack, bit8 IRQ enable   +0x10 bit0 busy
 *   +0x14 + 4*ch  result (u16, 10 bit), 12 channels
 *   +0x44 (div<<8)|n, +0x48 timing
 * All results read 1023: every resistor-ladder key released, zoom lever in
 * its neutral band (input.ko key table, docs/reference/RE_INPUT_I2C_POWER.md).
 * The board drives levels with cxd90014_adc_set() (key front end in
 * hw/arm/sony_a6000.c).
 */
#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "qemu/timer.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "hw/misc/cxd90014_adc.h"

#define TYPE_CXD_ADC "cxd90014-adc"
OBJECT_DECLARE_SIMPLE_TYPE(AdcState, CXD_ADC)

struct AdcState {
    SysBusDevice parent_obj;
    MemoryRegion mr;
    uint32_t reg[0x100 / 4];
    uint16_t result[12];
    uint16_t idle[12];      /* level with no key pressed (reset value) */
    bool idle_set;
    QEMUTimer *t;
    qemu_irq irq;
};

static void adc_update(AdcState *s)
{
    qemu_set_irq(s->irq, (s->reg[0x0C / 4] & 0x101) == 0x101);
}

static void adc_done(void *opaque)
{
    AdcState *s = opaque;
    s->reg[0x0C / 4] |= 1;
    adc_update(s);
    if ((s->reg[0x08 / 4] & 3) == 2 && (s->reg[0x04 / 4] & 1)) {
        timer_mod(s->t, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 10 * SCALE_MS);
    }
}

static uint64_t adc_read(void *opaque, hwaddr off, unsigned size)
{
    AdcState *s = opaque;
    if (off >= 0x14 && off < 0x14 + 4 * 12) {
        return s->result[(off - 0x14) / 4];
    }
    if (off == 0x10) {
        return 0;                       /* never busy */
    }
    return s->reg[off / 4];
}

static void adc_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    AdcState *s = opaque;
    if (off == 0x0C) {
        if (v & 0x10) {
            v &= ~0x11u;
            s->reg[0x0C / 4] &= ~1u;    /* ack */
        }
        s->reg[0x0C / 4] = (s->reg[0x0C / 4] & 1) | (v & ~1u);
        adc_update(s);
        return;
    }
    s->reg[off / 4] = v;
    if (off == 0x04 && (v & 1)) {
        timer_mod(s->t, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 50 * SCALE_US);
    }
}

static const MemoryRegionOps adc_ops = {
    .read = adc_read, .write = adc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 2, .max_access_size = 4 },
};

void cxd90014_adc_set(DeviceState *dev, int ch, uint16_t level)
{
    AdcState *s = CXD_ADC(dev);
    if (ch >= 0 && ch < 12) {
        s->result[ch] = level & 0x3ff;
    }
}

/*
 * Idle (released) level of a channel, kept across resets.  Default 1023;
 * a position-type input such as the A6000 mode dial (two ladders, one of
 * them inside a band at every dial position) sets it to the band it rests in.
 */
void cxd90014_adc_set_idle(DeviceState *dev, int ch, uint16_t level)
{
    AdcState *s = CXD_ADC(dev);
    if (!s->idle_set) {
        for (int i = 0; i < 12; i++) {
            s->idle[i] = 1023;
        }
        s->idle_set = true;
    }
    if (ch >= 0 && ch < 12) {
        s->idle[ch] = level & 0x3ff;
        s->result[ch] = s->idle[ch];
    }
}

uint16_t cxd90014_adc_idle(DeviceState *dev, int ch)
{
    AdcState *s = CXD_ADC(dev);
    return s->idle_set && ch >= 0 && ch < 12 ? s->idle[ch] : 1023;
}

static void adc_reset(DeviceState *dev)
{
    AdcState *s = CXD_ADC(dev);
    memset(s->reg, 0, sizeof(s->reg));
    for (int i = 0; i < 12; i++) {
        s->result[i] = s->idle_set ? s->idle[i] : 1023;
    }
    timer_del(s->t);
}

static void adc_init(Object *obj)
{
    AdcState *s = CXD_ADC(obj);
    memory_region_init_io(&s->mr, obj, &adc_ops, s, "cxd-adc", 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mr);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->t = timer_new_ns(QEMU_CLOCK_VIRTUAL, adc_done, s);
}

static void adc_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), adc_reset);
}

static const TypeInfo adc_info = {
    .name = TYPE_CXD_ADC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AdcState),
    .instance_init = adc_init,
    .class_init = adc_class_init,
};

static void adc_register(void)
{
    type_register_static(&adc_info);
}
type_init(adc_register)

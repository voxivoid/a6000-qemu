/*
 * CXD90014 OPD block (register analysis: docs/reference/).
 *
 * Spec: docs/reference/RE_RAW_DEVELOP.md §4 and §7.3.  Addresses below are av-cam.bin
 * VAs (load address 0x43541000, Thumb).
 *
 * PA 0xF3030000..0xF3031FFF.  Four interrupt units share one layout:
 *   unit 0  +0x100  IRQ 67  ISR 436AC9B4  handler 43ADF086 set_flg(29, st)
 *   unit 1  +0x140  IRQ 68  ISR 436AC9D0  handler 439836D4
 *   unit 2  +0x200  IRQ 69  ISR 436AC9EC  handler 43983750 (IDT sync pass)
 *   unit 3  +0x300  IRQ 70  ISR 436ACA08  handler 43983710
 * (av-cam IRQ records at 0x43E9510C; handlers installed by 43983668 after
 * IDT init 4397BB1C succeeds.)
 *   unit +0x04  status byte, write-1-to-clear (the ISRs write back what they
 *               read; 43A49198 writes 0xFF to all four at init)
 *   unit +0x08  source enable byte (0xFF armed, 0 idle; 43A49190)
 *   IRQ_k level = (status_k & enable_k) != 0.
 *   +0x00C      control.  A write with bit 1 going 0 -> 1 (436ADA9C, the
 *               "start" of the synchronous IDT pass 43973F50) completes
 *               after start-us by setting status bit 0 in every unit whose
 *               enable is non-zero, and clearing bit 1 again.
 *               hypothesis: which unit belongs to which job (only unit 2 /
 *               IRQ 69 is proven, for 43973F50), which status bit the
 *               hardware sets (the handlers do not test it), the self-clear
 *               of bit 1 and the delay.
 *   +0x000..+0x008, unit +0x00/+0x0C, 0xF3031000..0xF30317FF (OPD tables;
 *               436ADB7A clears 512 words at 0xF3031000) and +0x17C0..
 *               +0x17FC (FOPD output address 0x9EC00000 at +0x17C0,
 *               439739F0): read back as written.
 * No data is moved (the OPD tables / results are not written).
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "trace.h"

#define TYPE_CXD_OPD "cxd90014-opd"
OBJECT_DECLARE_SIMPLE_TYPE(CxdOpdState, CXD_OPD)

#define OPD_SIZE    0x2000
#define OPD_CTRL    0x00C
#define OPD_START   0x2u
#define OPD_NUNIT   4

static const hwaddr opd_unit[OPD_NUNIT] = { 0x100, 0x140, 0x200, 0x300 };

struct CxdOpdState {
    SysBusDevice parent_obj;
    MemoryRegion mr;
    uint32_t r[OPD_SIZE / 4];
    uint64_t base;
    uint32_t start_us;
    QEMUTimer *done;
    qemu_irq irq[OPD_NUNIT];
};

#define R(o) (s->r[(o) / 4])

static void opd_update(CxdOpdState *s)
{
    for (int k = 0; k < OPD_NUNIT; k++) {
        qemu_set_irq(s->irq[k], (R(opd_unit[k] + 4) &
                                 R(opd_unit[k] + 8) & 0xFF) != 0);
    }
}

static void opd_done(void *opaque)
{
    CxdOpdState *s = opaque;
    uint32_t units = 0;

    for (int k = 0; k < OPD_NUNIT; k++) {
        if (R(opd_unit[k] + 8) & 0xFF) {
            R(opd_unit[k] + 4) |= 1;
            units |= 1u << k;
        }
    }
    R(OPD_CTRL) &= ~OPD_START;
    trace_cxd_opd_done(units);
    opd_update(s);
}

/* unit index of a status/enable register, or -1 */
static int opd_unit_reg(hwaddr w, hwaddr *reg)
{
    for (int k = 0; k < OPD_NUNIT; k++) {
        if (w >= opd_unit[k] && w < opd_unit[k] + 0x10) {
            *reg = w - opd_unit[k];
            return k;
        }
    }
    return -1;
}

static bool opd_known(hwaddr w)
{
    hwaddr reg;

    return w <= OPD_CTRL || opd_unit_reg(w, &reg) >= 0 ||
           (w >= 0x1000 && w < 0x1800);
}

static uint64_t opd_read(void *opaque, hwaddr off, unsigned size)
{
    CxdOpdState *s = opaque;
    hwaddr w = off & ~3;
    unsigned sh = (off & 3) * 8;
    uint32_t v = R(w);

    if (!opd_known(w)) {
        qemu_log_mask(LOG_UNIMP, "cxd90014-opd: rd 0x%08" HWADDR_PRIx
                      " -> 0x%x\n", s->base + w, v);
    }
    return size == 4 ? v : (v >> sh) & ((1u << (size * 8)) - 1);
}

static void opd_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    CxdOpdState *s = opaque;
    hwaddr w = off & ~3, reg;
    unsigned sh = (off & 3) * 8;
    uint32_t m = size == 4 ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1) << sh;
    uint32_t v = (uint32_t)val << sh;
    int k = opd_unit_reg(w, &reg);

    trace_cxd_opd_wr((uint32_t)(s->base + w), v, m);
    if (k >= 0 && reg == 0x04) {    /* status byte, W1C */
        R(w) &= ~(v & m & 0xFF);
        opd_update(s);
        return;
    }
    if (k >= 0 && reg == 0x08) {    /* enable byte */
        R(w) = (R(w) & ~(m & 0xFF)) | (v & m & 0xFF);
        opd_update(s);
        return;
    }
    if (w == OPD_CTRL) {
        bool was = R(w) & OPD_START;

        R(w) = (R(w) & ~m) | (v & m);
        if (!was && (R(w) & OPD_START)) {
            trace_cxd_opd_start(R(w), R(0x108), R(0x148), R(0x208),
                                R(0x308));
            timer_mod(s->done, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      (int64_t)s->start_us * SCALE_US);
        }
        return;
    }
    if (!opd_known(w)) {
        qemu_log_mask(LOG_UNIMP, "cxd90014-opd: wr 0x%08" HWADDR_PRIx
                      " = 0x%x (mask 0x%x)\n", s->base + w, v, m);
    }
    R(w) = (R(w) & ~m) | (v & m);
}

static const MemoryRegionOps opd_ops = {
    .read = opd_read,
    .write = opd_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void opd_reset(DeviceState *dev)
{
    CxdOpdState *s = CXD_OPD(dev);

    memset(s->r, 0, sizeof(s->r));
    timer_del(s->done);
    opd_update(s);
}

static void opd_init(Object *obj)
{
    CxdOpdState *s = CXD_OPD(obj);

    memory_region_init_io(&s->mr, obj, &opd_ops, s, "cxd90014-opd",
                          OPD_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mr);
    for (int k = 0; k < OPD_NUNIT; k++) {
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[k]);
    }
    s->done = timer_new_ns(QEMU_CLOCK_VIRTUAL, opd_done, s);
}

static const Property opd_props[] = {
    DEFINE_PROP_UINT64("base", CxdOpdState, base, 0xF3030000), /* log */
    /* hypothesis: completion delay, see header */
    DEFINE_PROP_UINT32("start-us", CxdOpdState, start_us, 1000),
};

static void opd_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, opd_reset);
    device_class_set_props(dc, opd_props);
}

static const TypeInfo opd_info = {
    .name = TYPE_CXD_OPD,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CxdOpdState),
    .instance_init = opd_init,
    .class_init = opd_class_init,
};

static void opd_register(void)
{
    type_register_static(&opd_info);
}
type_init(opd_register)

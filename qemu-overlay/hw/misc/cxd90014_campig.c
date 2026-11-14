/*
 * CXD90014 camera front end: "Pig" frame/interrupt block of the CamCore
 * front-end units, and the SIF sync-code counters (a5000).
 *
 * cxd90014-campig: first 0x100 bytes of each unit (the rest of each 64 KiB
 * block stays on the catch-all register file).  docs/RE_SENSOR_ISP.md §2.3.
 *   0xF3000000 unit 0, GIC IRQ 36 (ISR 43A465F2)
 *   0xF3100000 unit 1, GIC IRQ 37 (ISR 436A6940)
 *   0xF3120000 input unit, GIC IRQ 35 (ISR 436A6904)
 * Registers (accessors 43A45FA4..43A46054 in av-cam.bin 1.10):
 *   +0x10  path start: path k is armed by writing bit k (43A45FD0 = 2*x
 *          "A", 43A45FD6 = 4*x, 43A45FDC = 8*x "B"; 43A45FE2 raw).  The ISR
 *          writes the high byte of the status it read back here, i.e. it
 *          re-arms the paths that just completed (436A6794).
 *   +0x14  same bit layout (43A45FEA..43A45FFC) [? meaning]
 *   +0x1C / +0x20 / +0x24  per-path parameters (AIF channel mask for path A,
 *          436A6700 / 436A6640) [?]
 *   +0x48  status: bit (24 + k) = path k frame done; written back by the
 *          ISR -> write-1-to-clear.  436A6794 tests 0x2000000 / 0x8000000.
 *   +0x4C  second status word, W1C (436A62B0); its source is unknown and
 *          the model never sets it [?]
 *   +0x50  bit (24 + k) = IRQ enable of path k (43A45FAC..43A46040; the
 *          driver sets bit25/bit27 for waited paths, 436A6700/436A666C)
 * Model: every engine frame (33367 us, the engine's own constant 0x8257,
 * docs/RE_ENGINE_TICK.md) each armed path completes: its status bit is set,
 * it is disarmed, and the IRQ is asserted while (status & enable) != 0.
 * Frame alignment relative to the 0xF3C30000 event block is arbitrary [?].
 * GPIO out 0 pulses once per frame (frame end); the board connects it to
 * the unit's AIF channel group and the SIF counters.
 *
 * +0x200..0x2FF "Sgc1" line latch (CamCore::Sgc1 436A70DC, docs/
 *   RE_PLAYBACK_SWITCH.md §6): writing 1 to +0x214 latches +0x218 and
 *   bit0 self-clears at once (the guest polls it with IRQs masked on
 *   CPU3, so it must not depend on a timer).  +0x218 low 14 bits must be
 *   5..0x3FFE, or 0x3FFF ("no sync") while +0x210 bit0 = 0; we latch
 *   0x00800080 (hypothesis: the real value is the current line position).
 *   +0x210 bit16 reads 0; the rest reads back.
 *
 * cxd90014-sif: SIF sync-code receivers 0xF3122000..0xF3123FFF.  The
 * imager driver reads one 16-bit sync-code counter per block
 * (ReadSIFScodeCnt 436B2474: 0xF3122330, 0xF3122224, 0xF3123330,
 * 0xF3123224) and only prints their per-frame sum (436B0E12).  Modelled as
 * counting received frames (hypothesis; 0 did not fail either).  All other
 * registers read back as written.
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

#define TYPE_CXD_CAMPIG "cxd90014-campig"
OBJECT_DECLARE_SIMPLE_TYPE(CamPigState, CXD_CAMPIG)

#define PIG_SIZE  0x400                 /* +0x200..0x2FF: Sgc1 latch */
#define FRAME_US  33367                 /* engine constant 0x8257 */
#define PATHS     0x0F000000u

struct CamPigState {
    SysBusDevice parent_obj;
    MemoryRegion mr;
    uint32_t r[PIG_SIZE / 4];
    uint32_t st48, st4c;
    uint32_t armed;                     /* bits 24..27 */
    uint64_t base;
    QEMUTimer *vd;
    qemu_irq irq, frame;
};

#define R(o) (s->r[(o) / 4])

static void pig_update(CamPigState *s)
{
    qemu_set_irq(s->irq, ((s->st48 | s->st4c) & R(0x50) & PATHS) != 0);
}

static void pig_vd(void *opaque)
{
    CamPigState *s = opaque;

    if (s->armed) {
        trace_cxd_campig_frame(s->base, s->armed);
        s->st48 |= s->armed;
        s->armed = 0;
        pig_update(s);
    }
    qemu_irq_pulse(s->frame);
    timer_mod(s->vd, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              FRAME_US * SCALE_US);
}

static uint64_t pig_read(void *opaque, hwaddr off, unsigned size)
{
    CamPigState *s = opaque;

    switch (off) {
    case 0x48:
        return s->st48;
    case 0x4C:
        return s->st4c;
    case 0x00 ... 0x2C:
    case 0x38 ... 0x3C:
    case 0x50 ... 0x54:
        return R(off);
    case 0x210:
        return R(off) & ~(1u << 16);
    case 0x200 ... 0x20C:
    case 0x214 ... 0x2FC:
        return R(off);
    }
    qemu_log_mask(LOG_UNIMP, "cxd90014-campig: rd 0x%08" HWADDR_PRIx
                  " -> 0x%x\n", s->base + off, R(off));
    return R(off);
}

static void pig_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    CamPigState *s = opaque;

    switch (off) {
    case 0x10:
        s->armed |= ((uint32_t)v & 0xf) << 24;
        R(off) = v;
        break;
    case 0x48:
        s->st48 &= ~v;
        break;
    case 0x4C:
        s->st4c &= ~v;
        break;
    case 0x214:
        if (v & 1) {
            R(0x218) = (R(0x210) & 1) ? 0x00800080 : 0x3FFF;
        }
        R(off) = v & ~1u;
        break;
    case 0x200 ... 0x210:
    case 0x218 ... 0x2FC:
        R(off) = v;
        break;
    case 0x00 ... 0x0C:
    case 0x14 ... 0x2C:
    case 0x38 ... 0x3C:
    case 0x50 ... 0x54:
        R(off) = v;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "cxd90014-campig: wr 0x%08" HWADDR_PRIx
                      " = 0x%" PRIx64 "\n", s->base + off, v);
        R(off) = v;
        break;
    }
    pig_update(s);
}

static const MemoryRegionOps pig_ops = {
    .read = pig_read, .write = pig_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void pig_reset(DeviceState *dev)
{
    CamPigState *s = CXD_CAMPIG(dev);
    memset(s->r, 0, sizeof(s->r));
    s->st48 = s->st4c = s->armed = 0;
    timer_mod(s->vd, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              FRAME_US * SCALE_US);
}

static void pig_init(Object *obj)
{
    CamPigState *s = CXD_CAMPIG(obj);
    memory_region_init_io(&s->mr, obj, &pig_ops, s, "campig", PIG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mr);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_out(DEVICE(obj), &s->frame, 1);
    s->vd = timer_new_ns(QEMU_CLOCK_VIRTUAL, pig_vd, s);
}

static const Property pig_props[] = {
    DEFINE_PROP_UINT64("base", CamPigState, base, 0),   /* log only */
};

static void pig_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_legacy_reset(dc, pig_reset);
    device_class_set_props(dc, pig_props);
}

static const TypeInfo pig_info = {
    .name = TYPE_CXD_CAMPIG,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CamPigState),
    .instance_init = pig_init,
    .class_init = pig_class_init,
};

/* ---- SIF sync-code receivers ---- */

#define TYPE_CXD_SIF "cxd90014-sif"
OBJECT_DECLARE_SIMPLE_TYPE(SifState, CXD_SIF)

#define SIF_BASE  0xF3122000
#define SIF_SIZE  0x2000

struct SifState {
    SysBusDevice parent_obj;
    MemoryRegion mr;
    uint32_t r[SIF_SIZE / 4];
    uint16_t frames;
};

static bool sif_counter(hwaddr off)
{
    return off == 0x330 || off == 0x224 || off == 0x1330 || off == 0x1224;
}

static void sif_frame(void *opaque, int n, int level)
{
    SifState *s = opaque;
    if (level) {
        s->frames++;
    }
}

static uint64_t sif_read(void *opaque, hwaddr off, unsigned size)
{
    SifState *s = opaque;
    if (sif_counter(off & ~3)) {
        return s->frames;
    }
    qemu_log_mask(LOG_UNIMP, "cxd90014-sif: rd%u 0x%08" HWADDR_PRIx
                  " -> 0x%x\n", size, SIF_BASE + off, s->r[off / 4]);
    return s->r[off / 4] >> ((off & 3) * 8);
}

static void sif_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    SifState *s = opaque;
    qemu_log_mask(LOG_UNIMP, "cxd90014-sif: wr%u 0x%08" HWADDR_PRIx
                  " = 0x%" PRIx64 "\n", size, SIF_BASE + off, v);
    if (size == 4) {
        s->r[off / 4] = v;
    } else {
        unsigned sh = (off & 3) * 8;
        uint32_t m = ((1u << (size * 8)) - 1) << sh;
        s->r[off / 4] = (s->r[off / 4] & ~m) | (((uint32_t)v << sh) & m);
    }
}

static const MemoryRegionOps sif_ops = {
    .read = sif_read, .write = sif_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void sif_reset(DeviceState *dev)
{
    SifState *s = CXD_SIF(dev);
    memset(s->r, 0, sizeof(s->r));
    s->frames = 0;
}

static void sif_init(Object *obj)
{
    SifState *s = CXD_SIF(obj);
    memory_region_init_io(&s->mr, obj, &sif_ops, s, "sif", SIF_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mr);
    qdev_init_gpio_in(DEVICE(obj), sif_frame, 1);
}

static void sif_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), sif_reset);
}

static const TypeInfo sif_info = {
    .name = TYPE_CXD_SIF,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SifState),
    .instance_init = sif_init,
    .class_init = sif_class_init,
};

static void campig_register(void)
{
    type_register_static(&pig_info);
    type_register_static(&sif_info);
}
type_init(campig_register)

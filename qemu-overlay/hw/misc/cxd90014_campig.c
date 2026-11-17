/*
 * CXD90014 camera front end: "Pig" frame/interrupt block of the CamCore
 * front-end units, and the SIF sync-code counters.
 *
 * cxd90014-campig: first 0x100 bytes of each unit (the rest of each 64 KiB
 * block stays on the catch-all register file).  docs/reference/RE_SENSOR_ISP.md §2.3.
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
 *   +0x4C  second status word, W1C (436A62B0).  Input unit: bit 0 = Sgc1
 *          event B (below); units 0/1: never set by the model [?]
 *   +0x50  IRQ enable of +0x48, bit for bit: bit (24 + k) = path k
 *          (43A45FAC..43A46040; the driver sets bit25/bit27 for waited
 *          paths, 436A6700/436A666C); input unit bit 0 = Sgc1 event A
 *   +0x54  IRQ enable of +0x4C, bit for bit.  Evidence: the interrupt
 *          object's register slots 43A46272 (A list, ack +0x48, +0x50 |=
 *          bits) and 43A4651C (B list, ack +0x4C, +0x54 |= bits).
 *   IRQ = ((+0x48 & +0x50) | (+0x4C & +0x54)) != 0; both status words are
 *   full 32-bit W1C (ISR 436A6904 writes back what it read, 436A62A4 /
 *   436A62B0, then dispatches 436A6414 / 436A65A8).
 * Model: every engine frame (33367 us, the engine's own constant 0x8257,
 * docs/reference/RE_ENGINE_TICK.md) each armed path completes: its status bit is set,
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
 * Input unit only (property input-unit, 0xF3120000):
 * Sgc1 sync generator, docs/reference/RE_CAPTURE_COMPLETION.md §2/§5.2.  Job
 *   programming CamCore::Sgc1 436A7284: +0x220 = (V-1) << 16 | (H-1)
 *   (43A45C46), +0x224 = line of event A, then +0x210 = 1 | cont << 8
 *   (43A4697C; cont = 1 for live modes 0/1/2, 0 for still modes 3/4).
 *   Modes 3/4 return 0x108 if +0x210 bit 0 is still set, so a one-shot
 *   drops bit 0 itself at its frame end.
 *   Writing +0x210 bit 0 = 1 while stopped starts a frame; bit 0 = 0 stops
 *   (no events).  Per frame: event A at line min(+0x224 & 0x3FFF, V-1)
 *   sets +0x48 bit 0 (handler 436A75CC: calls completion callback 1);
 *   event B at the frame end sets +0x4C bit 0 (handler 43A46E62: calls
 *   callback 2).  Then the next frame if bit 8 (continuous), else bit 0
 *   is cleared.  Clock 27 MHz: live view programs 819 x 1100 = 900900
 *   clocks = 33367 us, the engine frame (docs/reference/RE_ENGINE_TICK.md).
 *   hypothesis: 27 MHz and the line x H timing also hold for the still
 *   one-shots (1336 x 202 = 10 ms, 1336 x 7868 = 389 ms, RE_CAPTURE_
 *   COMPLETION §2.1); the [1 ms, 2 s] clamp and the 33367 us fallback for
 *   +0x220 = 0 are emulator choices.
 * CamCore::Iif 0x400..0x4FF (RE_CAPTURE_COMPLETION §3/§5.3): +0x410 bit 0
 *   is the input-frame run bit, set only by software (Iif start 436A5BD4,
 *   436A5C64 -> 43A45CF4).  CamCore start 43A40058 returns 0x208 while it
 *   is still set (43A45D8A).  The device clears it at the end of the next
 *   input frame, before event B is raised.  hypothesis: the clear point
 *   (next Sgc1 frame end) and, with the SG stopped, the next campig frame
 *   tick as the frame source.  +0x410 bit 16 (busy, 43A45DA2) reads 0;
 *   +0x47C reads back (bit 0 cleared by software, 43A45DCE).
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
#define IN_SIZE   0x500                 /* input unit: + Iif 0x400..0x4FF */
#define FRAME_US  33367                 /* engine constant 0x8257 */
#define PATHS     0x0F000000u
#define SG_HZ     27000000ull           /* 900900 clocks per live frame */

struct CamPigState {
    SysBusDevice parent_obj;
    MemoryRegion mr;
    uint32_t r[IN_SIZE / 4];
    uint32_t st48, st4c;
    uint32_t armed;                     /* bits 24..27 */
    uint64_t base;
    bool input_unit;
    QEMUTimer *vd;
    qemu_irq irq, frame;
    /* Sgc1 sync generator (input unit) */
    QEMUTimer *sg;
    bool sg_run;
    bool sg_b;                          /* next event is the frame end */
    bool sg_hold;                       /* frame end waits for the A ack */
    int64_t sg_t0;                      /* frame start, ns */
    int64_t sg_len, sg_a;               /* frame length, event A offset */
};

#define R(o) (s->r[(o) / 4])

static void pig_update(CamPigState *s)
{
    qemu_set_irq(s->irq, ((s->st48 & R(0x50)) | (s->st4c & R(0x54))) != 0);
}

/* end of an input frame: the Iif run bit drops (see header) */
static void iif_frame_end(CamPigState *s)
{
    if (R(0x410) & 1) {
        R(0x410) &= ~1u;
        trace_cxd_campig_iif_end(s->base);
    }
}

static void sg_frame_start(CamPigState *s, int64_t t0)
{
    uint32_t v = (R(0x220) >> 16) + 1, h = (R(0x220) & 0xFFFF) + 1;
    uint32_t l = MIN(R(0x224) & 0x3FFF, v - 1);

    if (R(0x220) == 0) {
        s->sg_len = (int64_t)FRAME_US * SCALE_US;
        s->sg_a = 0;
    } else {
        s->sg_len = muldiv64((uint64_t)v * h, NANOSECONDS_PER_SECOND, SG_HZ);
        s->sg_a = muldiv64((uint64_t)l * h, NANOSECONDS_PER_SECOND, SG_HZ);
    }
    s->sg_len = MAX(MIN(s->sg_len, 2 * NANOSECONDS_PER_SECOND),
                    SCALE_MS);
    s->sg_a = MIN(s->sg_a, s->sg_len);
    s->sg_t0 = t0;
    s->sg_b = false;
    s->sg_run = true;
    trace_cxd_campig_sg_start(s->base, R(0x210), v, h, l, s->sg_len / 1000);
    timer_mod(s->sg, t0 + s->sg_a);
}

static void sg_frame_end(CamPigState *s);

static void sg_event(void *opaque)
{
    CamPigState *s = opaque;

    if (!s->sg_b) {
        s->st48 |= 1;                   /* event A */
        s->sg_b = true;
        trace_cxd_campig_sg_event(s->base, 'A');
        pig_update(s);
        timer_mod(s->sg, s->sg_t0 + s->sg_len);
        return;
    }
    if ((s->st48 & R(0x50)) & 1) {
        /* event A not serviced yet: hold B (see sg_frame_end) */
        s->sg_hold = true;
        trace_cxd_campig_sg_event(s->base, 'h');
        return;
    }
    sg_frame_end(s);
}

/*
 * Frame end = event B.  The ISR 436A6904 dispatches B (+0x4C) before A
 * (+0x48) when both are pending, and B's handler 43A46E62 only runs the
 * callback that A's handler 436A75CC has just moved to +0x78.  A still
 * one-shot puts A one line before the end (+0x224 = V-1, 7.5 us for the
 * first lever exposure), shorter than the emulated interrupt latency, so
 * B is held while an enabled A is still pending and raised when the guest
 * acks it (W1C of +0x48 bit 0).  hypothesis: on the real camera the ISR
 * runs within that line; this hold only keeps the order A -> B that the
 * firmware relies on (docs/reference/RE_CAPTURE_COMPLETION.md §5.2 "Timing").
 */
static void sg_frame_end(CamPigState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    s->sg_hold = false;
    iif_frame_end(s);
    s->st4c |= 1;                       /* event B */
    trace_cxd_campig_sg_event(s->base, 'B');
    if (R(0x210) & 0x100) {
        sg_frame_start(s, MAX(s->sg_t0 + s->sg_len, now));
    } else {
        R(0x210) &= ~1u;                /* one-shot done */
        s->sg_run = false;
    }
    pig_update(s);
}

static void pig_vd(void *opaque)
{
    CamPigState *s = opaque;

    if (s->input_unit && !s->sg_run) {
        iif_frame_end(s);
    }
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
    case 0x410:
        if (s->input_unit) {
            return R(off) & ~(1u << 16);
        }
        break;
    case 0x47C:
        if (s->input_unit) {
            return R(off);
        }
        break;
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
        if (s->sg_hold && !(s->st48 & 1)) {
            sg_frame_end(s);
        }
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
    case 0x210:
        R(off) = v;
        if (s->input_unit) {
            if ((v & 1) && !s->sg_run) {
                sg_frame_start(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
            } else if (!(v & 1) && s->sg_run) {
                timer_del(s->sg);
                s->sg_run = s->sg_hold = false;
                trace_cxd_campig_sg_stop(s->base);
            }
        }
        break;
    case 0x200 ... 0x20C:
    case 0x218 ... 0x2FC:
        R(off) = v;
        break;
    case 0x410:
    case 0x47C:
        if (!s->input_unit) {
            goto unimp;
        }
        R(off) = v;
        break;
    case 0x00 ... 0x0C:
    case 0x14 ... 0x2C:
    case 0x38 ... 0x3C:
    case 0x50 ... 0x54:
        R(off) = v;
        break;
    default:
    unimp:
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
    timer_del(s->sg);
    s->sg_run = s->sg_b = s->sg_hold = false;
    timer_mod(s->vd, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              FRAME_US * SCALE_US);
}

static void pig_init(Object *obj)
{
    CamPigState *s = CXD_CAMPIG(obj);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_out(DEVICE(obj), &s->frame, 1);
    s->vd = timer_new_ns(QEMU_CLOCK_VIRTUAL, pig_vd, s);
    s->sg = timer_new_ns(QEMU_CLOCK_VIRTUAL, sg_event, s);
}

static void pig_realize(DeviceState *dev, Error **errp)
{
    CamPigState *s = CXD_CAMPIG(dev);
    memory_region_init_io(&s->mr, OBJECT(dev), &pig_ops, s, "campig",
                          s->input_unit ? IN_SIZE : PIG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mr);
}

static const Property pig_props[] = {
    DEFINE_PROP_UINT64("base", CamPigState, base, 0),   /* log only */
    /* 0xF3120000: Sgc1 sync generator + Iif (see header) */
    DEFINE_PROP_BOOL("input-unit", CamPigState, input_unit, false),
};

static void pig_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_legacy_reset(dc, pig_reset);
    device_class_set_props(dc, pig_props);
    dc->realize = pig_realize;
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

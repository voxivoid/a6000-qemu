/*
 * CXD90014 AIF DMA channel group (camera pipeline instances, a5000).
 *
 * The generic AIF driver of av-cam.bin 1.10 (pbl_aif_* at 43895C64..
 * 438967E0) addresses channel k through table 0x43B278D4
 * {group base, channel base, dir | nibble << 8}; a group is
 * base + 0x000 header, channels at base + 0x200 + 0x80 * k
 * (docs/RE_SENSOR_ISP.md §2.4, docs/RE_DISPLAY_SCANOUT.md §2).
 * One instance per group; VPH (0xF3441000) has its own model.
 *
 * Group header:
 *   +0x000  per-channel status nibbles, write-1-to-clear (read 43896484
 *           and 43AB98B6, acked with 0xF << 4k by 43AB98C6 and by the Pig
 *           "done" hook 438964B6).  Bit 0 of a nibble = transfer done OK
 *           (43AB8E64 treats a clear bit 0 as error 8).
 *   +0x004  error status, reads 0 (as in the VPH group)
 *   +0x008  per-channel interrupt enable nibbles, read-modify-write
 *           (43896442; e.g. 0xF3881008 = 0xF000 while channel 3 is
 *           waited for) [? enable meaning inferred]
 *   Interrupt: channel k's output (sysbus IRQ k) is asserted while
 *   (status nibble & enable nibble) != 0.  Only some channels are wired:
 *   the resize/RC groups' channel 1/3 completions are GIC 40..50 (av-cam
 *   ISRs 438CB804.. read and ack the nibble through objects whose status
 *   pointer / nibble index are e.g. {0xF3881000, 3} for IRQ 41).
 * Channel (+0x200 + 0x80 * k):
 *   +0x00  ctrl: bit0 = start / busy (set by 43896588, cleared by 43896614;
 *          pbl_aif_set_func returns -3 while it reads 1, 438965E8)
 *   +0x04  bit31 = running (43896650), bit30 / bit0 set / cleared by driver
 *   +0x08 .. +0x5C  address, size, offsets/counts (43896374, 438964EE,
 *          4389653A) -- read back as written
 * Completion model: a started channel is "running" (+0x04 bit31) until it
 * completes; then ctrl bit0 and +0x04 bit31 clear and status bit 0 of its
 * nibble sets.  With frame-sync=on
 * (front-end groups 0xF3002000 / 0xF3102000) the channel completes at the
 * next frame end of its CamCore unit (GPIO in 0, from cxd90014-campig);
 * otherwise after delay-us (hypothesis: the trigger of the develop /
 * resize engines is not decoded).  No data is moved: front-end channels
 * may point at the IIB (+0x20 = 0x6xxxxxxx), and image data is not
 * modelled.
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "trace.h"

#define TYPE_CXD_AIF "cxd90014-aif"
OBJECT_DECLARE_SIMPLE_TYPE(AifState, CXD_AIF)

#define AIF_MAXCH 8
#define CH(k)     (0x200 + 0x80 * (k))
#define AIF_SIZE  CH(AIF_MAXCH)

struct AifState {
    SysBusDevice parent_obj;
    MemoryRegion mr;
    uint32_t r[AIF_SIZE / 4];
    uint32_t busy;                  /* bit k = channel k started */
    uint64_t base;
    uint32_t nch;
    uint32_t delay_us;
    bool frame_sync;
    QEMUTimer *done;
    qemu_irq irq[AIF_MAXCH];
};

#define R(o) (s->r[(o) / 4])

static void aif_update(AifState *s)
{
    for (int k = 0; k < s->nch; k++) {
        qemu_set_irq(s->irq[k], (R(0) & R(8) & (0xfu << (4 * k))) != 0);
    }
}

static void aif_complete(AifState *s)
{
    if (!s->busy) {
        return;
    }
    trace_cxd_aif_done(s->base, s->busy);
    for (int k = 0; k < s->nch; k++) {
        if (s->busy & (1u << k)) {
            R(CH(k)) &= ~1u;
            R(CH(k) + 4) &= ~0x80000000u;
            R(0) |= 1u << (4 * k);
        }
    }
    s->busy = 0;
    aif_update(s);
}

static void aif_timer(void *opaque)
{
    aif_complete(opaque);
}

static void aif_frame(void *opaque, int n, int level)
{
    AifState *s = opaque;
    if (level && s->frame_sync) {
        aif_complete(s);
    }
}

static uint64_t aif_read(void *opaque, hwaddr off, unsigned size)
{
    AifState *s = opaque;

    if (off == 0x004) {
        return 0;
    }
    if (off == 0x000) {
        trace_cxd_aif_status(s->base, R(0));
    }
    if (off >= CH(s->nch) || (off >= 0x00C && off < CH(0))) {
        qemu_log_mask(LOG_UNIMP, "cxd90014-aif: rd 0x%08" HWADDR_PRIx
                      " -> 0x%x\n", s->base + off, R(off));
    }
    return R(off);
}

static void aif_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    AifState *s = opaque;

    if (off == 0x000) {
        R(off) &= ~v;
        aif_update(s);
        return;
    }
    if (off >= CH(s->nch) || (off >= 0x00C && off < CH(0))) {
        qemu_log_mask(LOG_UNIMP, "cxd90014-aif: wr 0x%08" HWADDR_PRIx
                      " = 0x%" PRIx64 "\n", s->base + off, v);
    }
    if (off >= CH(0) && off < CH(s->nch) && (off & 0x7f) == 0) {
        int k = (off - CH(0)) / 0x80;
        bool was = R(off) & 1;
        R(off) = v;
        if ((v & 1) && !was) {
            trace_cxd_aif_start(s->base, k, (uint32_t)v, R(off + 0x20),
                                R(off + 0x24));
            s->busy |= 1u << k;
            R(off + 4) |= 0x80000000u;
            if (!s->frame_sync) {
                timer_mod(s->done, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                          (int64_t)s->delay_us * SCALE_US);
            }
        } else if (!(v & 1) && was) {
            /* stopped by the driver (43896614) */
            s->busy &= ~(1u << k);
            R(off + 4) &= ~0x80000000u;
        }
        return;
    }
    if (off >= CH(0) && (off & 0x7f) == 4) {
        /* bit31 is the hardware "running" flag */
        R(off) = (v & ~0x80000000u) | (R(off) & 0x80000000u);
        return;
    }
    R(off) = v;
    if (off == 0x008) {
        aif_update(s);
    }
}

static const MemoryRegionOps aif_ops = {
    .read = aif_read, .write = aif_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void aif_reset(DeviceState *dev)
{
    AifState *s = CXD_AIF(dev);
    memset(s->r, 0, sizeof(s->r));
    s->busy = 0;
    timer_del(s->done);
    aif_update(s);
}

static void aif_realize(DeviceState *dev, Error **errp)
{
    AifState *s = CXD_AIF(dev);
    if (s->nch < 1 || s->nch > AIF_MAXCH) {
        error_setg(errp, "cxd90014-aif: nch must be 1..%d", AIF_MAXCH);
        return;
    }
    memory_region_init_io(&s->mr, OBJECT(dev), &aif_ops, s, "aif",
                          CH(s->nch));
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mr);
    for (int k = 0; k < AIF_MAXCH; k++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq[k]);
    }
}

static void aif_init(Object *obj)
{
    AifState *s = CXD_AIF(obj);
    qdev_init_gpio_in(DEVICE(obj), aif_frame, 1);
    s->done = timer_new_ns(QEMU_CLOCK_VIRTUAL, aif_timer, s);
}

static const Property aif_props[] = {
    DEFINE_PROP_UINT64("base", AifState, base, 0),      /* log only */
    DEFINE_PROP_UINT32("nch", AifState, nch, AIF_MAXCH),
    DEFINE_PROP_UINT32("delay-us", AifState, delay_us, 2000),
    DEFINE_PROP_BOOL("frame-sync", AifState, frame_sync, false),
};

static void aif_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = aif_realize;
    device_class_set_legacy_reset(dc, aif_reset);
    device_class_set_props(dc, aif_props);
}

static const TypeInfo aif_info = {
    .name = TYPE_CXD_AIF,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AifState),
    .instance_init = aif_init,
    .class_init = aif_class_init,
};

static void aif_register(void)
{
    type_register_static(&aif_info);
}
type_init(aif_register)

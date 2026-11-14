/*
 * CXD90014 camera timing-event block at PA 0xF3C30000 (a5000).
 *
 * Programming model from av-cam.bin 1.10 (docs/RE_ENGINE_TICK.md):
 *   +0xE0 free-running counter (rd 436ac5f4).  Clock: +0xE8 = 0x17 is
 *         written at init; taken as 24 MHz / (0x17 + 1) = 1 MHz [?] -- this
 *         matches the engine's own frame constant 0x8257 = 33367 us
 *         (29.97 fps) and its 1/8-frame step 0x104A.
 *   +0xE4 written 0 [?], +0xEC bit0 run.
 *   group A, 8 event channels, IRQ 83 (handler 436ac410):
 *     +0x04 status, +0x08 write-1-to-clear
 *     +0x0C bit k cleared on register [? meaning], +0x10 bit k enable
 *     +0x20 + 4k  counter captured at the event (callback argument, 436ac7d4)
 *     +0x40 + 4k  source id of channel k (436ac600)
 *     +0x60 + 4n  per-source mode (always 1 so far, 436ac644)
 *   group B, 10 counter comparators, IRQ 84 (handler 436ac440):
 *     +0xF4 status, +0xF8 enable (0x3ff), +0xFC write-1-to-clear, +0x100 [?]
 *     +0x110 + 8k arm (1) / disarm (0), +0x114 + 8k compare value
 *     (436ac81c; the handler disarms before running the callback, so a
 *     comparator fires once per arming)
 *
 * Event sources: the engine attaches its frame routine 4376c1e4 to source
 * 29 and 4376f181 / 4376c028 / 4376a07c to 28 / 30 / 23.  The ids are camera
 * timing signals; which signal each one is, is not known [?].  With no
 * sensor modelled, all enabled channels fire together once per engine frame
 * (33367 us).  The 1/8-frame "int_HS" steps are the comparators above.
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "qemu/timer.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_CXD_EVSEL "cxd90014-evsel"
OBJECT_DECLARE_SIMPLE_TYPE(EvselState, CXD_EVSEL)

#define EV_SIZE   0x1000
#define NCMP      10
#define FRAME_US  33367                 /* engine constant 0x8257 */

typedef struct EvselState EvselState;
typedef struct Cmp {
    EvselState *s;
    int k;
    QEMUTimer *t;
} Cmp;

struct EvselState {
    SysBusDevice parent_obj;
    MemoryRegion mr;
    uint32_t r[EV_SIZE / 4];
    uint32_t sta, stb;
    QEMUTimer *vd;
    Cmp cmp[NCMP];
    qemu_irq irq_a, irq_b;
};

#define R(o) (s->r[(o) / 4])

static uint32_t ev_counter(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000;   /* 1 MHz [?] */
}

static void ev_update(EvselState *s)
{
    qemu_set_irq(s->irq_a, (s->sta & R(0x10)) != 0);
    qemu_set_irq(s->irq_b, (s->stb & R(0xF8)) != 0);
}

static void ev_vd(void *opaque)
{
    EvselState *s = opaque;
    if (R(0xEC) & 1) {
        uint32_t en = R(0x10) & 0xff, c = ev_counter();
        for (int k = 0; k < 8; k++) {
            if (en & (1u << k)) {
                R(0x20 + 4 * k) = c;
            }
        }
        s->sta |= en;
        ev_update(s);
    }
    timer_mod(s->vd, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              FRAME_US * SCALE_US);
}

static void ev_cmp_fire(void *opaque)
{
    Cmp *c = opaque;
    EvselState *s = c->s;
    if (!(R(0x110 + 8 * c->k) & 1)) {
        return;                 /* disarmed meanwhile */
    }
    s->stb |= 1u << c->k;
    ev_update(s);
}

static void ev_cmp_arm(EvselState *s, int k)
{
    Cmp *c = &s->cmp[k];
    if (!(R(0x110 + 8 * k) & 1)) {
        timer_del(c->t);
        return;
    }
    /* signed distance on the wrapping counter; a target already passed
     * fires at once */
    int32_t d = (int32_t)(R(0x114 + 8 * k) - ev_counter());
    timer_mod(c->t, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              (d > 0 ? (int64_t)d * SCALE_US : 0));
}

static uint64_t ev_read(void *opaque, hwaddr off, unsigned size)
{
    EvselState *s = opaque;
    switch (off) {
    case 0x04:
        return s->sta;
    case 0xE0:
        return ev_counter();
    case 0xF4:
        return s->stb;
    }
    return R(off);
}

static void ev_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    EvselState *s = opaque;
    switch (off) {
    case 0x08:
        s->sta &= ~v;
        break;
    case 0xFC:
        s->stb &= ~v;
        break;
    default:
        R(off) = v;
        /*
         * Only the arm register (+0x110 + 8k) arms or disarms; a write to
         * the compare value (+0x114 + 8k) just latches.  The IRQ 84
         * handler disarms by writing a past compare value and then
         * +0x110 = 0; re-evaluating on the compare write fired the
         * comparator a second time (docs/RE_ENGINE_STABILITY.md §2).
         */
        if (off >= 0x110 && off < 0x110 + 8 * NCMP && !((off - 0x110) & 7)) {
            ev_cmp_arm(s, (off - 0x110) / 8);
        }
        break;
    }
    ev_update(s);
}

static const MemoryRegionOps ev_ops = {
    .read = ev_read, .write = ev_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void ev_reset(DeviceState *dev)
{
    EvselState *s = CXD_EVSEL(dev);
    memset(s->r, 0, sizeof(s->r));
    s->sta = s->stb = 0;
    for (int k = 0; k < NCMP; k++) {
        timer_del(s->cmp[k].t);
    }
    timer_mod(s->vd, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              FRAME_US * SCALE_US);
}

static void ev_init(Object *obj)
{
    EvselState *s = CXD_EVSEL(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    memory_region_init_io(&s->mr, obj, &ev_ops, s, "evsel", EV_SIZE);
    sysbus_init_mmio(sbd, &s->mr);
    sysbus_init_irq(sbd, &s->irq_a);
    sysbus_init_irq(sbd, &s->irq_b);
    s->vd = timer_new_ns(QEMU_CLOCK_VIRTUAL, ev_vd, s);
    for (int k = 0; k < NCMP; k++) {
        s->cmp[k] = (Cmp){ s, k,
                           timer_new_ns(QEMU_CLOCK_VIRTUAL, ev_cmp_fire,
                                        &s->cmp[k]) };
    }
}

static void ev_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_legacy_reset(dc, ev_reset);
}

static const TypeInfo ev_info = {
    .name = TYPE_CXD_EVSEL,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(EvselState),
    .instance_init = ev_init,
    .class_init = ev_class_init,
};

static void ev_register(void)
{
    type_register_static(&ev_info);
}
type_init(ev_register)

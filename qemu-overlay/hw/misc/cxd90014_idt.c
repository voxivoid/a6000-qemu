/*
 * CXD90014 IDT develop / FOPD control block (register analysis: docs/reference/).
 *
 * Spec: docs/reference/RE_RAW_DEVELOP.md §2 and §7.2.  Addresses below are av-cam.bin
 * VAs (load address 0x43541000, Thumb).
 *
 * PA 0xF3020400..0xF3020FFF (the same 4 KiB page as AIF group 0xF3020000,
 * channels 17-20, which keeps +0x000..+0x3FF), IRQ 72.  Register offsets
 * below are relative to the page base 0xF3020000.
 *
 *   +0x42C  status, write-1-to-clear (all bits).  ISR 436ACA24 reads it and
 *           tests only bits 25:24: 01 = "event 1" (acked with 0x0100003F),
 *           10 = "event 2" (acked with 0x02003F00, also clears and
 *           re-enables the AIF group 0xF3020000 nibbles), anything else =
 *           error (acked with 0x03000301).  Reset/abort helpers 436AD14A /
 *           436AD3CC write 0x03003F3F.  The model sets 0x01000001 for event
 *           1 and 0x02000100 for event 2 (hypothesis: the low 6 + 6 bits
 *           are not decoded; bit 24 / 25 summarise bits 0..5 / 8..13).
 *   +0x444  event enables (436ACB80, a deferred write while a job runs:
 *           the behaviour of an interrupt-enable register).  Bit 0 = event
 *           1, bit 1 = event 2 (hypothesis: inferred from 3 = both events
 *           on the FOPD kick 439739F0 and 0 in live view).
 *   +0xC00  run (436ACBE0).  A write with bit 0 going 0 -> 1 starts one
 *           develop frame when +0x444 & 3 != 0; live view writes it every
 *           frame with +0x444 = 0 and stays silent.  Bit 0 is not polled by
 *           software and is left as written (hypothesis: no self-clear).
 *   other   +0x400..+0x444, 0xF3020500.., 0xF3020600.., 0xF3020800..
 *           tables and the 0xF3020C00..+0xC7C job parameters (439AAB38..)
 *           read back as written (software reads only +0x420 of this block,
 *           as a read-modify-write).
 *
 * IRQ 72 level = ((st & 0x01000000) && (en & 1)) ||
 *                ((st & 0x02000000) && (en & 2)).
 *
 * Frame timing (hypothesis: the real duration is one pass over the RAW,
 * 5504 x 3656 pixels; firmware only needs event 1 before event 2, both
 * after the run write): event 1 event1-us after the run write; event 2 at
 * the next completion of AIF group 0xF3020000 (GPIO in 0, pulsed by that
 * group's cxd90014-aif "done" output: ch17 IIB input / ch18 output of the
 * FOPD job, RE_RAW_DEVELOP.md §2.4), never before event 1, or after
 * fallback-us if no group completion arrives.  Event 2 leads, via IdtEv
 * 43983798 -> 43983020 -> the callback stored by KickFOPD at 0x43E1D590
 * (43A59E6A), to set_flg(22, 0x400) for task 115's F22_wait(0x440)
 * (4385B82A).
 *
 * No image data is moved (the FOPD output at bus 0x1EC00000 is not
 * written).  Open questions: low status bits, the event 1 meaning, real
 * timing, the 2-pass variants (IDT states 3/4, 43983020).
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

#define TYPE_CXD_IDT "cxd90014-idt"
OBJECT_DECLARE_SIMPLE_TYPE(CxdIdtState, CXD_IDT)

#define IDT_FIRST   0x400           /* offsets relative to 0xF3020000 */
#define IDT_END     0x1000
#define IDT_STATUS  0x42C
#define IDT_ENABLE  0x444
#define IDT_RUN     0xC00

#define IDT_EV1     0x01000000u
#define IDT_EV2     0x02000000u
#define IDT_EV1_SET 0x01000001u     /* hypothesis: low bits, see header */
#define IDT_EV2_SET 0x02000100u

struct CxdIdtState {
    SysBusDevice parent_obj;
    MemoryRegion mr;
    uint32_t r[IDT_END / 4];
    uint64_t base;
    uint32_t event1_us;
    uint32_t fallback_us;
    bool running;                   /* frame started, event 2 not yet */
    bool ev1_done;
    bool aif_done;                  /* group completion seen this frame */
    uint32_t frames;
    QEMUTimer *t_ev1;
    QEMUTimer *t_fallback;
    qemu_irq irq;
};

#define R(o) (s->r[(o) / 4])

static void idt_update(CxdIdtState *s)
{
    uint32_t st = R(IDT_STATUS), en = R(IDT_ENABLE);

    qemu_set_irq(s->irq, ((st & IDT_EV1) && (en & 1)) ||
                         ((st & IDT_EV2) && (en & 2)));
}

static void idt_event1(CxdIdtState *s)
{
    s->ev1_done = true;
    timer_del(s->t_ev1);
    if (R(IDT_ENABLE) & 1) {
        R(IDT_STATUS) |= IDT_EV1_SET;
    }
    trace_cxd_idt_event(s->frames, 1, R(IDT_STATUS), R(IDT_ENABLE));
    idt_update(s);
}

static void idt_event2(CxdIdtState *s, bool fallback)
{
    if (!s->ev1_done) {
        idt_event1(s);              /* event 1 always comes first */
    }
    s->running = false;
    timer_del(s->t_fallback);
    if (R(IDT_ENABLE) & 2) {
        R(IDT_STATUS) |= IDT_EV2_SET;
    }
    trace_cxd_idt_event(s->frames, fallback ? 3 : 2, R(IDT_STATUS),
                        R(IDT_ENABLE));
    idt_update(s);
}

static void idt_ev1_timer(void *opaque)
{
    CxdIdtState *s = opaque;

    if (!s->running || s->ev1_done) {
        return;
    }
    idt_event1(s);
    if (s->aif_done) {
        idt_event2(s, false);       /* the group finished before event 1 */
    }
}

static void idt_fallback_timer(void *opaque)
{
    CxdIdtState *s = opaque;

    if (s->running) {
        idt_event2(s, true);
    }
}

/* GPIO in 0: AIF group 0xF3020000 completed (pulse) */
static void idt_aif_done(void *opaque, int n, int level)
{
    CxdIdtState *s = opaque;

    if (!level || !s->running) {
        return;
    }
    s->aif_done = true;
    if (s->ev1_done) {
        idt_event2(s, false);
    }
}

static void idt_start(CxdIdtState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    s->frames++;
    s->running = true;
    s->ev1_done = false;
    s->aif_done = false;
    trace_cxd_idt_start(s->frames, R(IDT_ENABLE), R(0xC0C), R(0x418));
    timer_mod(s->t_ev1, now + (int64_t)s->event1_us * SCALE_US);
    timer_mod(s->t_fallback, now + (int64_t)s->fallback_us * SCALE_US);
}

static bool idt_known(hwaddr w)
{
    return (w >= 0x400 && w <= IDT_ENABLE) ||
           (w >= 0x500 && w < 0xC80);   /* 0x500/0x600/0x800 tables, job */
}

static uint64_t idt_read(void *opaque, hwaddr off, unsigned size)
{
    CxdIdtState *s = opaque;
    hwaddr w = (off + IDT_FIRST) & ~3;
    unsigned sh = (off & 3) * 8;
    uint32_t v = R(w);

    if (!idt_known(w)) {
        qemu_log_mask(LOG_UNIMP, "cxd90014-idt: rd 0x%08" HWADDR_PRIx
                      " -> 0x%x\n", s->base + w, v);
    }
    return size == 4 ? v : (v >> sh) & ((1u << (size * 8)) - 1);
}

static void idt_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    CxdIdtState *s = opaque;
    hwaddr w = (off + IDT_FIRST) & ~3;
    unsigned sh = (off & 3) * 8;
    uint32_t m = size == 4 ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1) << sh;
    uint32_t v = (uint32_t)val << sh;
    bool was;

    trace_cxd_idt_wr((uint32_t)(s->base + w), v, m);
    switch (w) {
    case IDT_STATUS:                /* W1C */
        R(w) &= ~(v & m);
        idt_update(s);
        return;
    case IDT_ENABLE:
        R(w) = (R(w) & ~m) | (v & m);
        idt_update(s);
        return;
    case IDT_RUN:
        was = R(w) & 1;
        R(w) = (R(w) & ~m) | (v & m);
        if (!was && (R(w) & 1) && (R(IDT_ENABLE) & 3)) {
            idt_start(s);
        }
        return;
    }
    if (!idt_known(w)) {
        qemu_log_mask(LOG_UNIMP, "cxd90014-idt: wr 0x%08" HWADDR_PRIx
                      " = 0x%x (mask 0x%x)\n", s->base + w, v, m);
    }
    R(w) = (R(w) & ~m) | (v & m);
}

static const MemoryRegionOps idt_ops = {
    .read = idt_read,
    .write = idt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void idt_reset(DeviceState *dev)
{
    CxdIdtState *s = CXD_IDT(dev);

    memset(s->r, 0, sizeof(s->r));
    s->running = s->ev1_done = s->aif_done = false;
    timer_del(s->t_ev1);
    timer_del(s->t_fallback);
    idt_update(s);
}

static void idt_init(Object *obj)
{
    CxdIdtState *s = CXD_IDT(obj);

    memory_region_init_io(&s->mr, obj, &idt_ops, s, "cxd90014-idt",
                          IDT_END - IDT_FIRST);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mr);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_in(DEVICE(obj), idt_aif_done, 1);
    s->t_ev1 = timer_new_ns(QEMU_CLOCK_VIRTUAL, idt_ev1_timer, s);
    s->t_fallback = timer_new_ns(QEMU_CLOCK_VIRTUAL, idt_fallback_timer, s);
}

static const Property idt_props[] = {
    DEFINE_PROP_UINT64("base", CxdIdtState, base, 0xF3020000), /* log */
    /* hypothesis: timing, see header */
    DEFINE_PROP_UINT32("event1-us", CxdIdtState, event1_us, 100),
    DEFINE_PROP_UINT32("fallback-us", CxdIdtState, fallback_us, 5000),
};

static void idt_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, idt_reset);
    device_class_set_props(dc, idt_props);
}

static const TypeInfo idt_info = {
    .name = TYPE_CXD_IDT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CxdIdtState),
    .instance_init = idt_init,
    .class_init = idt_class_init,
};

static void idt_register(void)
{
    type_register_static(&idt_info);
}
type_init(idt_register)

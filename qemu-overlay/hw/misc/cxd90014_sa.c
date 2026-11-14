/*
 * CXD90014 SA DSP core, "null SA" model (a5000, fw 1.10).
 *
 * Spec: docs/RE_SA_DSP.md §5 (context: RE_LIVEVIEW_REQUEST.md §3-§4,
 * RE_SD_MEDIA_SERVICE.md §3.3 / §7.2).  Addresses below are av-cam.bin
 * VAs (load address 0x43541000).
 *
 * Four instances, one per core (av-cam accessors 43923E74..43924660):
 *   core 0  0xF34A0000  IRQ A/B/C = GIC 51 / 52 / 53  ID "ZMA0V1.0SA5_V1.0"
 *   core 1  0xF34B0000  IRQ A/B/C = GIC 54 / 55 / 56  ID "ZMA1V1.0SA5_V1.0"
 *   core 2  0xF34C0000  IRQ A/B/C = GIC 61 / 62 / 63  ID "SA0_V1.0SA5_V1.0"
 *   core 3  0xF35C0000  IRQ A/B/C = GIC 64 / 65 / 66  ID "SA1_V1.0SA5_V1.0"
 * Region size 0x1000 (hypothesis: the highest offset av-cam uses is +0x22C).
 * The clock/reset block 0xF3400000 stays on the catch-all register file and
 * does not gate this model (RE_SA_DSP §5.1, [?] which register gates).
 *
 * Registers (per core):
 *   +0x000         RW latch, bit0 = run enable (43923E74)
 *   +0x004         RW latch, bit0 = GO (43923EE0).  Writing bit0 = 1 while
 *                  +0x000 bit0 = 1 starts a "function" (below).  Bit0 reads
 *                  1 while running and 0 after completion (hypothesis: the
 *                  read site 43923EAC was not traced; 0 after completion is
 *                  the safe choice, RE_SA_DSP §5.2)
 *   +0x008 +0x00C  RW latches (+0x008 = 0x80000000 for DRAM programs,
 *   +0x01C +0x030  43923F18; +0x030 = program entry PA, 43924070)
 *   +0x084 +0x088  read 0 (only printed with the result, 439240A0/D4)
 *   +0x0F0..+0x0FF read-only 16-byte core ID, compared at task-44 init
 *                  (4391B334); 4 little-endian words of the ASCII string
 *   +0x100/140/200 source A/B/C status byte: read 0 (ISR 4391A4C4 copies it
 *                  into the event)
 *   +0x104/144/204 source A/B/C interrupt factor (byte).  Write = clear the
 *                  written bits (W1C): the ISR writes back the value it read,
 *                  the arm step 43925FB4 writes 0xFF.  The 0xFF must not read
 *                  back, or the 0x41 result carries int_fact 0xFF and ZIT
 *                  rejects it (RE_SA_DSP §5.2 pitfall)
 *   +0x108/148/208 source A/B/C factor enable mask (byte), RW latch.  Arming
 *                  writes 0xFF, the ISR writes 0 (43924214)
 *   +0x110..+0x11C, +0x150..+0x15C, +0x210..+0x21C
 *                  host->SA mailboxes A/B/C, RW latches (43924284)
 *   +0x120..+0x12C, +0x160..+0x16C, +0x220..+0x22C
 *                  SA->host mailboxes A/B/C: read 0 (copied into the 0x41
 *                  result by 43AC864E, only printed on error)
 *   anything else  RW latch + LOG_UNIMP
 * IRQ line X = (factor_X & mask_X & 0xFF) != 0, level (RE_SA_DSP §1.3).
 *
 * Start behaviour (HLE, AI_WORKFLOW §1.5: the SA program ISA is unknown):
 * a GO write logs the entry PA, +0x008 and the source-A host->SA mailbox
 * (trace event cxd_sa_start plus a LOG_UNIMP line, so later SA clients can
 * be identified), then arms a one-shot timer.  The completion cannot happen
 * inside the MMIO write: the ISR posts to SA manager task 44, which is the
 * task doing the write.  On expiry factor A |= 0x01 ("finished": int_fact 1,
 * SDF WrapSaCalcBase 436532D0, ZIT 4398EA3C) and GO bit0 clears.  +0x000 is
 * left as written (hypothesis, RE_SA_DSP §5.3 [?]).  Sources B and C are
 * wired but never raised.  The model never reads or writes guest RAM: every
 * result message is built by av-cam (4391D3BC).
 *
 * Open questions (RE_SA_DSP §7): real run time (the 100 us default is a
 * placeholder), semantics of +0x004 read-back, +0x008/+0x00C/+0x01C,
 * +0x084/+0x088 and the status bytes; which clients use sources B/C.
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

#define TYPE_CXD_SA "cxd90014-sa"
OBJECT_DECLARE_SIMPLE_TYPE(CxdSaState, CXD_SA)

#define SA_SIZE     0x1000
#define SA_NSRC     3               /* sources A, B, C */
#define SA_IDLEN    16

#define SA_RUN      0x000
#define SA_GO       0x004
#define SA_PROG     0x008
#define SA_ENTRY    0x030
#define SA_ID       0x0F0
#define SA_MBOX_A   0x110

/* per-source register block: A = +0x100, B = +0x140, C = +0x200 */
static const hwaddr sa_src_base[SA_NSRC] = { 0x100, 0x140, 0x200 };

struct CxdSaState {
    SysBusDevice parent_obj;
    MemoryRegion mr;
    QEMUTimer *done;
    qemu_irq irq[SA_NSRC];

    uint32_t r[SA_SIZE / 4];        /* latches */
    uint8_t factor[SA_NSRC];
    uint8_t mask[SA_NSRC];
    bool running;
    uint32_t starts;

    /* properties */
    uint32_t index;
    uint64_t base;                  /* log only */
    char *core_id;
    uint32_t delay_us;
    uint8_t id[SA_IDLEN];
};

#define R(o) (s->r[(o) / 4])

static void sa_update(CxdSaState *s)
{
    for (int i = 0; i < SA_NSRC; i++) {
        qemu_set_irq(s->irq[i], (s->factor[i] & s->mask[i]) != 0);
    }
}

/* Classify a word offset: source index and register within it, or -1. */
static int sa_src_reg(hwaddr w, hwaddr *reg)
{
    for (int i = 0; i < SA_NSRC; i++) {
        if (w >= sa_src_base[i] && w < sa_src_base[i] + 0x30) {
            *reg = w - sa_src_base[i];
            return i;
        }
    }
    return -1;
}

static void sa_done(void *opaque)
{
    CxdSaState *s = opaque;

    s->running = false;
    R(SA_GO) &= ~1u;
    s->factor[0] |= 0x01;           /* source A: "finished" */
    trace_cxd_sa_done(s->index, s->factor[0], s->mask[0]);
    sa_update(s);
}

static void sa_start(CxdSaState *s)
{
    s->starts++;
    trace_cxd_sa_start(s->index, s->starts, R(SA_ENTRY), R(SA_PROG),
                       R(SA_MBOX_A), R(SA_MBOX_A + 4), R(SA_MBOX_A + 8),
                       R(SA_MBOX_A + 12));
    qemu_log_mask(LOG_UNIMP, "cxd90014-sa%u: start #%u entry 0x%08x "
                  "+0x008 0x%08x mbox %08x %08x %08x %08x (null SA: "
                  "completes with factor A 0x01, no data)\n",
                  s->index, s->starts, R(SA_ENTRY), R(SA_PROG),
                  R(SA_MBOX_A), R(SA_MBOX_A + 4), R(SA_MBOX_A + 8),
                  R(SA_MBOX_A + 12));
    s->running = true;
    timer_mod(s->done, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              (int64_t)s->delay_us * SCALE_US);
}

static uint32_t sa_read_word(CxdSaState *s, hwaddr w)
{
    hwaddr reg;
    int src;

    switch (w) {
    case SA_RUN:
    case SA_GO:
    case SA_PROG:
    case 0x00C:
    case 0x01C:
    case SA_ENTRY:
        return R(w);
    case 0x084:
    case 0x088:
        return 0;
    case SA_ID ... SA_ID + 0xC:
        return ldl_le_p(&s->id[w - SA_ID]);
    }
    src = sa_src_reg(w, &reg);
    if (src >= 0) {
        switch (reg) {
        case 0x00:                  /* status byte */
            return 0;
        case 0x04:
            return s->factor[src];
        case 0x08:
            return s->mask[src];
        case 0x10 ... 0x1C:         /* host->SA mailbox */
            return R(w);
        case 0x20 ... 0x2C:         /* SA->host mailbox */
            return 0;
        }
    }
    qemu_log_mask(LOG_UNIMP, "cxd90014-sa%u: rd 0x%08" HWADDR_PRIx
                  " -> 0x%x\n", s->index, s->base + w, R(w));
    return R(w);
}

static void sa_write_word(CxdSaState *s, hwaddr w, uint32_t v, uint32_t m)
{
    hwaddr reg;
    int src;

    switch (w) {
    case SA_GO:
        R(w) = (R(w) & ~m) | (v & m);
        if ((m & 1) && (v & 1) && !s->running) {
            if (R(SA_RUN) & 1) {
                sa_start(s);
            } else {
                qemu_log_mask(LOG_UNIMP, "cxd90014-sa%u: GO while +0x000 "
                              "bit0 = 0 (ignored)\n", s->index);
            }
        }
        return;
    case SA_RUN:
    case SA_PROG:
    case 0x00C:
    case 0x01C:
    case SA_ENTRY:
        R(w) = (R(w) & ~m) | (v & m);
        return;
    }
    src = sa_src_reg(w, &reg);
    if (src >= 0) {
        switch (reg) {
        case 0x04:                  /* factor: W1C of the low byte */
            s->factor[src] &= ~(v & m & 0xFF);
            sa_update(s);
            return;
        case 0x08:
            s->mask[src] = (s->mask[src] & ~(m & 0xFF)) | (v & m & 0xFF);
            sa_update(s);
            return;
        case 0x10 ... 0x1C:
            R(w) = (R(w) & ~m) | (v & m);
            return;
        }
    }
    qemu_log_mask(LOG_UNIMP, "cxd90014-sa%u: wr 0x%08" HWADDR_PRIx
                  " = 0x%x (mask 0x%x)\n", s->index, s->base + w, v, m);
    R(w) = (R(w) & ~m) | (v & m);
}

static uint64_t sa_read(void *opaque, hwaddr off, unsigned size)
{
    CxdSaState *s = opaque;
    unsigned sh = (off & 3) * 8;
    uint32_t v = sa_read_word(s, off & ~3);

    return size == 4 ? v : (v >> sh) & ((1u << (size * 8)) - 1);
}

static void sa_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    CxdSaState *s = opaque;
    unsigned sh = (off & 3) * 8;
    uint32_t m = size == 4 ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1) << sh;

    sa_write_word(s, off & ~3, (uint32_t)v << sh, m);
}

static const MemoryRegionOps sa_ops = {
    .read = sa_read, .write = sa_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void sa_reset(DeviceState *dev)
{
    CxdSaState *s = CXD_SA(dev);

    timer_del(s->done);
    memset(s->r, 0, sizeof(s->r));
    memset(s->factor, 0, sizeof(s->factor));
    memset(s->mask, 0, sizeof(s->mask));
    s->running = false;
    sa_update(s);
}

static void sa_realize(DeviceState *dev, Error **errp)
{
    CxdSaState *s = CXD_SA(dev);

    if (!s->core_id || strlen(s->core_id) != SA_IDLEN) {
        error_setg(errp, "cxd90014-sa: core-id must be %d characters",
                   SA_IDLEN);
        return;
    }
    memcpy(s->id, s->core_id, SA_IDLEN);
}

static void sa_init(Object *obj)
{
    CxdSaState *s = CXD_SA(obj);

    memory_region_init_io(&s->mr, obj, &sa_ops, s, "cxd90014-sa", SA_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mr);
    for (int i = 0; i < SA_NSRC; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[i]);
    }
    s->done = timer_new_ns(QEMU_CLOCK_VIRTUAL, sa_done, s);
}

static const Property sa_props[] = {
    DEFINE_PROP_UINT32("index", CxdSaState, index, 0),
    DEFINE_PROP_UINT64("base", CxdSaState, base, 0),    /* log only */
    DEFINE_PROP_STRING("core-id", CxdSaState, core_id),
    /* hypothesis: real SA run time unknown (RE_SA_DSP §7 q1) */
    DEFINE_PROP_UINT32("delay-us", CxdSaState, delay_us, 100),
};

static void sa_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = sa_realize;
    device_class_set_legacy_reset(dc, sa_reset);
    device_class_set_props(dc, sa_props);
}

static const TypeInfo sa_info = {
    .name = TYPE_CXD_SA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CxdSaState),
    .instance_init = sa_init,
    .class_init = sa_class_init,
};

static void sa_register(void)
{
    type_register_static(&sa_info);
}
type_init(sa_register)

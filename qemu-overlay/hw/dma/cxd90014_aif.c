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
 *
 * Debug option (not hardware), docs/RE_LIVEVIEW_IMAGE.md §4 option (a):
 * lv-source=bars|ramp|file:PATH writes one 8-bit luma frame into guest
 * DRAM when channel lv-ch completes, just before its status nibble / IRQ
 * are raised.  The board sets lv-ch = 3 only on the resize group
 * 0xF3881000 (ch26 = RC output = the ch51 live-view buffer); every other
 * instance has lv-ch = -1 and never writes.  The frame follows the
 * channel's own address generator (+0x20 address, +0x24 bytes per line,
 * +0x28 skip after each line, +0x2C lines - 1, outer loops +0x30..+0x44).
 * file: is a binary PGM (P5, maxval <= 255), scaled nearest-neighbour to
 * the programmed geometry.  Default (empty): nothing is written.
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/address-spaces.h"
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
    /* debug live-view source (see header) */
    char *lv_source;
    int32_t lv_ch;
    int lv_mode;                    /* LV_* */
    uint8_t *lv_img;                /* file: luma, lv_w x lv_h */
    uint32_t lv_w, lv_h;
    uint32_t lv_frame;
    bool lv_c_warned;
};

enum { LV_NONE, LV_BARS, LV_RAMP, LV_FILE };

#define R(o) (s->r[(o) / 4])

static void aif_update(AifState *s)
{
    for (int k = 0; k < s->nch; k++) {
        qemu_set_irq(s->irq[k], (R(0) & R(8) & (0xfu << (4 * k))) != 0);
    }
}

/* luma of the 75 % colour bars (BT.601, full range, black 0x10) */
static const uint8_t lv_bars[8] = { 191, 170, 134, 112, 79, 57, 22, 16 };

static uint8_t lv_sample(AifState *s, uint32_t x, uint32_t y,
                         uint32_t w, uint32_t h)
{
    switch (s->lv_mode) {
    case LV_BARS:
        return lv_bars[(uint64_t)x * 8 / w];
    case LV_RAMP:
        /* diagonal gradient that moves 4 levels per frame */
        return (uint8_t)(((uint64_t)x * 256 / w + (uint64_t)y * 64 / h +
                          s->lv_frame * 4) & 0xff);
    case LV_FILE:
        return s->lv_img[(uint64_t)y * s->lv_h / h * s->lv_w +
                         (uint64_t)x * s->lv_w / w];
    }
    return 0;
}

/*
 * Write one luma frame along channel k's address generator.  Address
 * generator (docs/RE_LIVEVIEW_IMAGE.md §1.2): after each transfer of +0x24
 * bytes the address advances by +0x24 + ofs0, count0 + 1 times; level n
 * (1..3) repeats level n-1 countn + 1 times and adds ofsn after each
 * repetition.  hypothesis: the outer-loop rule (counts 1..3 are 0 in live
 * view, so only the inner loop is exercised).
 * Bus address A is seen by the CPU at 0x80000000 + A (the RC job gets
 * 0x8CF34000, ch26 is programmed 0x0CF34000; docs/RE_LIVEVIEW_IMAGE.md
 * §2.2).  hypothesis: the bus -> CPU alias.
 */
static void aif_lv_inject(AifState *s, int k)
{
    uint32_t ctrl = R(CH(k)), a = R(CH(k) + 0x20), sz = R(CH(k) + 0x24);
    int32_t ofs[4];
    uint32_t cnt[4];

    for (int n = 0; n < 4; n++) {
        ofs[n] = (int32_t)R(CH(k) + 0x28 + 8 * n);
        cnt[n] = R(CH(k) + 0x2C + 8 * n) + 1;
    }
    /* IIB mode (ctrl bit11) or non-DRAM bus address: not a memory frame */
    if ((ctrl & 0x800) || a >= 0x20000000 || !sz || sz > 16384) {
        return;
    }
    uint64_t lines = (uint64_t)cnt[0] * cnt[1] * cnt[2] * cnt[3];
    if (lines > 16384) {
        return;
    }
    trace_cxd_aif_inject(s->base, k, a, sz, (uint32_t)lines);

    g_autofree uint8_t *row = g_malloc(sz);
    uint64_t addr = 0x80000000ull + a;
    uint32_t y = 0;
    for (uint32_t i3 = 0; i3 < cnt[3]; i3++) {
        for (uint32_t i2 = 0; i2 < cnt[2]; i2++) {
            for (uint32_t i1 = 0; i1 < cnt[1]; i1++) {
                for (uint32_t i0 = 0; i0 < cnt[0]; i0++, y++) {
                    for (uint32_t x = 0; x < sz; x++) {
                        row[x] = lv_sample(s, x, y, sz, (uint32_t)lines);
                    }
                    address_space_write(&address_space_memory, addr,
                                        MEMTXATTRS_UNSPECIFIED, row, sz);
                    addr += sz + (int64_t)ofs[0];
                }
                addr += (int64_t)ofs[1];
            }
            addr += (int64_t)ofs[2];
        }
        addr += (int64_t)ofs[3];
    }
    s->lv_frame++;
}

static void aif_complete(AifState *s)
{
    if (!s->busy) {
        return;
    }
    trace_cxd_aif_done(s->base, s->busy);
    if (s->lv_mode != LV_NONE && s->lv_ch >= 0 && s->lv_ch < s->nch &&
        (s->busy & (1u << s->lv_ch))) {
        /* chroma is not invented: report a second write channel (ch25 =
         * nibble 1 of the RC group, never started in live view) once */
        if ((s->busy & 2) && !(R(CH(1)) & 0x800) && !s->lv_c_warned) {
            s->lv_c_warned = true;
            qemu_log_mask(LOG_UNIMP, "cxd90014-aif: 0x%08" PRIx64
                          " ch1 (C plane?) started with the live-view "
                          "channel, addr 0x%08x size 0x%x: not filled\n",
                          s->base, R(CH(1) + 0x20), R(CH(1) + 0x24));
        }
        aif_lv_inject(s, s->lv_ch);
    }
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

/* binary PGM: "P5" ws width ws height ws maxval single-ws data (# comments) */
static bool lv_load_pgm(AifState *s, const char *path, Error **errp)
{
    g_autofree char *buf = NULL;
    gsize len;
    g_autoptr(GError) gerr = NULL;
    unsigned long v[3];
    size_t p = 2;

    if (!g_file_get_contents(path, &buf, &len, &gerr)) {
        error_setg(errp, "cxd90014-aif: lv-source: %s", gerr->message);
        return false;
    }
    if (len < 2 || buf[0] != 'P' || buf[1] != '5') {
        error_setg(errp, "cxd90014-aif: lv-source: '%s' is not a binary "
                   "PGM (P5)", path);
        return false;
    }
    for (int i = 0; i < 3; i++) {
        for (;;) {
            while (p < len && g_ascii_isspace(buf[p])) {
                p++;
            }
            if (p < len && buf[p] == '#') {
                while (p < len && buf[p] != '\n') {
                    p++;
                }
                continue;
            }
            break;
        }
        if (p >= len || !g_ascii_isdigit(buf[p])) {
            error_setg(errp, "cxd90014-aif: lv-source: '%s': bad PGM "
                       "header", path);
            return false;
        }
        v[i] = 0;
        while (p < len && g_ascii_isdigit(buf[p]) && v[i] < 100000) {
            v[i] = v[i] * 10 + (buf[p++] - '0');
        }
    }
    if (p >= len || !g_ascii_isspace(buf[p])) {
        error_setg(errp, "cxd90014-aif: lv-source: '%s': bad PGM header",
                   path);
        return false;
    }
    p++;
    if (!v[0] || !v[1] || v[0] > 16384 || v[1] > 16384) {
        error_setg(errp, "cxd90014-aif: lv-source: '%s': bad size %lux%lu",
                   path, v[0], v[1]);
        return false;
    }
    if (!v[2] || v[2] > 255) {
        error_setg(errp, "cxd90014-aif: lv-source: '%s': maxval %lu "
                   "(only 8-bit PGM is supported)", path, v[2]);
        return false;
    }
    if (len - p < (size_t)v[0] * v[1]) {
        error_setg(errp, "cxd90014-aif: lv-source: '%s': truncated "
                   "(%zu of %lu data bytes)", path, len - p, v[0] * v[1]);
        return false;
    }
    s->lv_w = v[0];
    s->lv_h = v[1];
    s->lv_img = g_malloc((size_t)s->lv_w * s->lv_h);
    for (size_t i = 0; i < (size_t)s->lv_w * s->lv_h; i++) {
        s->lv_img[i] = MIN(255u, (uint8_t)buf[p + i] * 255u / v[2]);
    }
    return true;
}

static void aif_realize(DeviceState *dev, Error **errp)
{
    AifState *s = CXD_AIF(dev);
    if (s->nch < 1 || s->nch > AIF_MAXCH) {
        error_setg(errp, "cxd90014-aif: nch must be 1..%d", AIF_MAXCH);
        return;
    }
    s->lv_mode = LV_NONE;
    if (s->lv_source && *s->lv_source) {
        const char *path;
        if (!strcmp(s->lv_source, "bars")) {
            s->lv_mode = LV_BARS;
        } else if (!strcmp(s->lv_source, "ramp")) {
            s->lv_mode = LV_RAMP;
        } else if (strstart(s->lv_source, "file:", &path) && *path) {
            s->lv_mode = LV_FILE;
            /* -global reaches every group: load only where it is used */
            if (s->lv_ch >= 0 && !lv_load_pgm(s, path, errp)) {
                return;
            }
        } else {
            error_setg(errp, "cxd90014-aif: lv-source must be empty, "
                       "'bars', 'ramp' or 'file:PATH' (got '%s')",
                       s->lv_source);
            return;
        }
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
    /* debug, not hardware: live-view frame source (see header) */
    DEFINE_PROP_STRING("lv-source", AifState, lv_source),
    DEFINE_PROP_INT32("lv-ch", AifState, lv_ch, -1),  /* set by the board */
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

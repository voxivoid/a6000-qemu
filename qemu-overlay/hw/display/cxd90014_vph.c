/*
 * CXD90014 video output: VPH display controller + SYSV frame timer (a5000).
 *
 *   VPH  PA 0xF3440000..0xF3446FFF  (docs/RE_DISPLAY_SCANOUT.md)
 *        0x028 = 0x111 clock enable (we start vsync once it is non-zero)
 *        0x144 bit0 panel YUV layer on, bit1 OSD [?]; 0x404 bit0 OSD 4 B/px
 *        AIF DMA ch51..58 @ 0xF3441200 + 0x80*k: +0x00 bit0 start/busy
 *        (cleared by hardware at end of frame), +0x04 bit31 running,
 *        +0x20 start address, +0x24 bytes per line, +0x2C lines-1,
 *        +0x48 signed pitch.  ch51 = panel YUV (live view), ch53 = panel OSD.
 *        IRQ 101 / 102 = output vsync (both pulsed; which one is the panel is
 *        unknown), IRQ 79 = status at +0xA0 (not raised).
 *   SYSV PA 0xF3402000 (docs/RE_AVCAM_IRQ_BOOT.md): +0x98 frame counter,
 *        +0x3C period (av-cam writes 64800); IRQ 98 every frame.
 *
 * Every 1/60 s: SYSV counter++ and IRQ 98 pulse; if VPH is clocked, IRQ
 * 101/102 pulses, AIF ch51..58 start bits cleared, and the panel is composed
 * (YUV 4:2:2 under the OSD) into QEMU graphic console 0.
 * OSD at 16 bpp = RGBA4444 (A low nibble); the YUV packing is a hypothesis.
 *
 * Debug option (not hardware): pattern=bars composes 75% colour bars in
 * place of the guest's YUV buffer whenever the panel YUV layer is enabled
 * (-global cxd90014-vph.pattern=bars).  Nothing is written to guest memory,
 * so the firmware sees the same machine with or without it.
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "ui/console.h"
#include "system/address-spaces.h"
#include "qom/object.h"

#define TYPE_CXD_VPH "cxd90014-vph"
OBJECT_DECLARE_SIMPLE_TYPE(VphState, CXD_VPH)

#define VPH_SIZE  0x7000
#define AIF(k)    (0x1200 + 0x80 * (k))     /* k = 0 (ch51) .. 7 (ch58) */

struct VphState {
    SysBusDevice parent_obj;
    MemoryRegion vph_mr, sysv_mr;
    uint32_t vph[VPH_SIZE / 4];
    uint32_t sysv[0x1000 / 4];
    uint32_t frame;
    QEMUTimer *tick, *lower;
    qemu_irq irq_sysv, irq_v101, irq_v102;
    QemuConsole *con;
    uint32_t w, h;
    char *pattern;                  /* debug: "bars" */
};

#define R(o) (s->vph[(o) / 4])

static void vph_lower(void *opaque)
{
    VphState *s = opaque;
    qemu_set_irq(s->irq_sysv, 0);
    qemu_set_irq(s->irq_v101, 0);
    qemu_set_irq(s->irq_v102, 0);
}

static void vph_tick(void *opaque)
{
    VphState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    s->frame++;
    qemu_set_irq(s->irq_sysv, 1);
    /* output vsync runs whether or not VPH is configured yet: the VDF task
     * waits for IRQ 101/102 before it programs anything */
    for (int k = 0; k < 8; k++) {
        R(AIF(k)) &= ~1u;
    }
    qemu_set_irq(s->irq_v101, 1);
    qemu_set_irq(s->irq_v102, 1);
    timer_mod(s->lower, now + 200 * SCALE_US);  /* no ack register: pulse */
    timer_mod(s->tick, now + NANOSECONDS_PER_SECOND / 60);
}

static inline uint8_t clip(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

static void vph_update(void *opaque)
{
    VphState *s = opaque;
    DisplaySurface *surf = qemu_console_surface(s->con);
    uint32_t W = s->w, H = s->h;
    if (!surf || surface_width(surf) != W || surface_height(surf) != H) {
        qemu_console_resize(s->con, W, H);
        surf = qemu_console_surface(s->con);
    }
    uint32_t *dst = surface_data(surf);
    memset(dst, 0, (size_t)W * H * 4);

    /* YUV layer (ch51), packed YUYV 4:2:2 [?] */
    if ((R(0x144) & 1) && s->pattern && !strcmp(s->pattern, "bars")) {
        static const uint32_t bars[8] = {
            0xbfbfbf, 0xbfbf00, 0x00bfbf, 0x00bf00,
            0xbf00bf, 0xbf0000, 0x0000bf, 0x101010,
        };
        uint32_t lines = MIN(R(AIF(0) + 0x2C) + 1, H);
        for (uint32_t y = 0; y < lines; y++) {
            for (uint32_t x = 0; x < W; x++) {
                dst[y * W + x] = 0xff000000u | bars[x * 8 / W];
            }
        }
    } else if (R(0x144) & 1) {
        uint32_t base = R(AIF(0) + 0x20), lb = R(AIF(0) + 0x24);
        uint32_t lines = R(AIF(0) + 0x2C) + 1;
        int32_t pitch = (int16_t)R(AIF(0) + 0x48);
        if (!pitch) {
            pitch = lb;
        }
        if (base && lb && lines < 4096) {
            g_autofree uint8_t *row = g_malloc(lb);
            for (uint32_t y = 0; y < MIN(lines, H); y++) {
                address_space_read(&address_space_memory, base + y * pitch,
                                   MEMTXATTRS_UNSPECIFIED, row, lb);
                for (uint32_t x = 0; x + 1 < MIN(lb / 2, W); x += 2) {
                    int y0 = row[2 * x], u = row[2 * x + 1] - 128;
                    int y1 = row[2 * x + 2], v = row[2 * x + 3] - 128;
                    for (int i = 0; i < 2; i++) {
                        int yy = i ? y1 : y0;
                        dst[y * W + x + i] = 0xff000000u |
                            clip(yy + 1.402 * v) << 16 |
                            clip(yy - 0.344 * u - 0.714 * v) << 8 |
                            clip(yy + 1.772 * u);
                    }
                }
            }
        }
    }
    /* OSD layer (ch53) */
    uint32_t base = R(AIF(2) + 0x20), lb = R(AIF(2) + 0x24);
    uint32_t lines = R(AIF(2) + 0x2C) + 1;
    int32_t pitch = (int16_t)R(AIF(2) + 0x48);
    unsigned bpp = (R(0x404) & 1) ? 4 : 2;
    if (!pitch) {
        pitch = lb;
    }
    if (base && lb && lines < 4096) {
        g_autofree uint8_t *row = g_malloc(lb);
        for (uint32_t y = 0; y < MIN(lines, H); y++) {
            address_space_read(&address_space_memory, base + y * pitch,
                               MEMTXATTRS_UNSPECIFIED, row, lb);
            for (uint32_t x = 0; x < MIN(lb / bpp, W); x++) {
                uint8_t r, g, b, a;
                if (bpp == 4) {
                    r = row[4 * x]; g = row[4 * x + 1]; b = row[4 * x + 2];
                    a = row[4 * x + 3];
                } else {
                    /* RGBA4444, A in the low nibble: the UI renders with
                     * P3D colour format 4 and PPF in/out format 4 into
                     * the panel buffers (docs/RE_GPU_P3D_CMD.md) */
                    uint16_t v = row[2 * x] | row[2 * x + 1] << 8;
                    r = (v >> 12) * 17; g = ((v >> 8) & 15) * 17;
                    b = ((v >> 4) & 15) * 17; a = (v & 15) * 17;
                }
                uint32_t *d = &dst[y * W + x];
                uint8_t dr = *d >> 16, dg = *d >> 8, db = *d;
                *d = 0xff000000u | ((r * a + dr * (255 - a)) / 255) << 16 |
                     ((g * a + dg * (255 - a)) / 255) << 8 |
                     ((b * a + db * (255 - a)) / 255);
            }
        }
    }
    dpy_gfx_update_full(s->con);
}

static void vph_invalidate(void *opaque)
{
}

static const GraphicHwOps vph_gfx_ops = {
    .invalidate = vph_invalidate,
    .gfx_update = vph_update,
};

static uint64_t vph_read(void *opaque, hwaddr off, unsigned size)
{
    VphState *s = opaque;
    if (off >= AIF(0) && off < AIF(8) && (off & 0x7f) == 0x04) {
        return R(off) & ~0x80000000u;          /* never "still running" */
    }
    if (off == 0x1004 || off == 0x2004 || off == 0x3004) {
        return 0;                              /* error status */
    }
    return R(off);
}

static void vph_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    VphState *s = opaque;
    R(off) = v;
}

static uint64_t sysv_read(void *opaque, hwaddr off, unsigned size)
{
    VphState *s = opaque;
    if (off == 0x98) {
        return s->frame;
    }
    if (off == 0x28) {
        /*
         * bit0 = frame phase, follows the frame counter parity.  In its 2V
         * frame mode av-cam's VD_Pre (4376CFDC, phase read by 43AB5486)
         * only hands out module requests on frames with bit0 = 1; a
         * constant 0 stalled the mode sequencer at step 12 whenever the
         * first mode request came after the 2V switch
         * (docs/RE_ENGINE_STABILITY.md §1).  hypothesis: polarity/phase
         * relative to the engine frame event.
         */
        return (s->sysv[off / 4] & ~1u) | (s->frame & 1);
    }
    return s->sysv[off / 4];
}

static void sysv_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    VphState *s = opaque;
    s->sysv[off / 4] = v;
}

static const MemoryRegionOps vph_ops = {
    .read = vph_read, .write = vph_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};
static const MemoryRegionOps sysv_ops = {
    .read = sysv_read, .write = sysv_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void vph_reset(DeviceState *dev)
{
    VphState *s = CXD_VPH(dev);
    memset(s->vph, 0, sizeof(s->vph));
    memset(s->sysv, 0, sizeof(s->sysv));
    s->frame = 0;
    timer_mod(s->tick, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / 60);
}

static void vph_realize(DeviceState *dev, Error **errp)
{
    VphState *s = CXD_VPH(dev);
    s->w = 640;
    s->h = 480;
    s->con = graphic_console_init(dev, 0, &vph_gfx_ops, s);
    qemu_console_resize(s->con, s->w, s->h);
}

static void vph_init(Object *obj)
{
    VphState *s = CXD_VPH(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    memory_region_init_io(&s->vph_mr, obj, &vph_ops, s, "vph", VPH_SIZE);
    memory_region_init_io(&s->sysv_mr, obj, &sysv_ops, s, "sysv", 0x1000);
    sysbus_init_mmio(sbd, &s->vph_mr);
    sysbus_init_mmio(sbd, &s->sysv_mr);
    sysbus_init_irq(sbd, &s->irq_sysv);
    sysbus_init_irq(sbd, &s->irq_v101);
    sysbus_init_irq(sbd, &s->irq_v102);
    s->tick = timer_new_ns(QEMU_CLOCK_VIRTUAL, vph_tick, s);
    s->lower = timer_new_ns(QEMU_CLOCK_VIRTUAL, vph_lower, s);
}

static const Property vph_props[] = {
    DEFINE_PROP_STRING("pattern", VphState, pattern),
};

static void vph_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = vph_realize;
    device_class_set_props(dc, vph_props);
    device_class_set_legacy_reset(dc, vph_reset);
}

static const TypeInfo vph_info = {
    .name = TYPE_CXD_VPH,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(VphState),
    .instance_init = vph_init,
    .class_init = vph_class_init,
};

static void vph_register(void)
{
    type_register_static(&vph_info);
}
type_init(vph_register)

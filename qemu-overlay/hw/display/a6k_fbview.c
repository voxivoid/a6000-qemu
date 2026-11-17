/*
 * a6k-fbview — host window onto guest frame buffers (sony-a6000).
 *
 * Until the CXD90014 display controller (VDF scan-out, programmed by
 * av-cam.bin) is modelled, this device shows a guest-physical buffer in a
 * QEMU graphic console so the camera UI can be seen.  It is a debug view, not
 * hardware: it has no MMIO and the firmware cannot tell it exists.
 *
 * Property "fb" = "addr,width,height,stride,fmt[;addr,...]" (layers are drawn
 * bottom to top, alpha-blended).  fmt: argb8888 | rgba8888 | rgb565 | argb4444.
 * HMP: "qom-set /machine/peripheral-anon/... fb ..." or the machine option.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "qemu/cutils.h"
#include "qemu/module.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "system/address-spaces.h"
#include "qom/object.h"

#define TYPE_A6K_FBVIEW "a6k-fbview"
OBJECT_DECLARE_SIMPLE_TYPE(FbviewState, A6K_FBVIEW)

#define MAXL 4
typedef struct Layer {
    uint64_t addr;
    uint32_t w, h, stride;
    int fmt;            /* 0 argb8888, 1 rgba8888, 2 rgb565, 3 argb4444 */
} Layer;

struct FbviewState {
    SysBusDevice parent_obj;
    QemuConsole *con;
    char *spec;
    Layer l[MAXL];
    int nl;
    uint32_t w, h;
};

static int fmt_of(const char *f)
{
    return !strcmp(f, "rgba8888") ? 1 : !strcmp(f, "rgb565") ? 2 :
           !strcmp(f, "argb4444") ? 3 : 0;
}

static void parse(FbviewState *s)
{
    s->nl = 0;
    s->w = 640; s->h = 480;
    if (!s->spec) {
        return;
    }
    g_auto(GStrv) ls = g_strsplit(s->spec, ";", MAXL);
    for (int i = 0; ls[i] && s->nl < MAXL; i++) {
        g_auto(GStrv) f = g_strsplit(ls[i], ",", 5);
        if (g_strv_length(f) < 5) {
            continue;
        }
        Layer *l = &s->l[s->nl];
        uint64_t v;
        qemu_strtou64(f[0], NULL, 0, &l->addr);
        qemu_strtou64(f[1], NULL, 0, &v); l->w = v;
        qemu_strtou64(f[2], NULL, 0, &v); l->h = v;
        qemu_strtou64(f[3], NULL, 0, &v); l->stride = v;
        l->fmt = fmt_of(f[4]);
        if (s->nl == 0) {
            s->w = l->w; s->h = l->h;
        }
        s->nl++;
    }
}

static void px(const uint8_t *p, int fmt, uint8_t *r, uint8_t *g, uint8_t *b,
               uint8_t *a)
{
    uint16_t v = p[0] | p[1] << 8;
    switch (fmt) {
    case 1: *r = p[3]; *g = p[2]; *b = p[1]; *a = p[0]; break;
    case 2: *r = (v >> 11) << 3; *g = ((v >> 5) & 63) << 2;
            *b = (v & 31) << 3; *a = 255; break;
    case 3: *a = (v >> 12) * 17; *r = ((v >> 8) & 15) * 17;
            *g = ((v >> 4) & 15) * 17; *b = (v & 15) * 17; break;
    default: *b = p[0]; *g = p[1]; *r = p[2]; *a = p[3]; break;
    }
}

static void fb_update(void *opaque)
{
    FbviewState *s = opaque;
    DisplaySurface *surf = qemu_console_surface(s->con);
    if (!surf || surface_width(surf) != s->w || surface_height(surf) != s->h) {
        qemu_console_resize(s->con, s->w, s->h);
        surf = qemu_console_surface(s->con);
    }
    uint32_t *dst = surface_data(surf);
    memset(dst, 0, (size_t)s->w * s->h * 4);
    for (int i = 0; i < s->nl; i++) {
        Layer *l = &s->l[i];
        unsigned bpp = l->fmt >= 2 ? 2 : 4;
        size_t len = (size_t)l->stride * l->h;
        g_autofree uint8_t *buf = g_malloc(len);
        address_space_read(&address_space_memory, l->addr,
                           MEMTXATTRS_UNSPECIFIED, buf, len);
        for (uint32_t y = 0; y < MIN(l->h, s->h); y++) {
            for (uint32_t x = 0; x < MIN(l->w, s->w); x++) {
                uint8_t r, g, b, a;
                px(buf + (size_t)y * l->stride + x * bpp, l->fmt, &r, &g, &b, &a);
                if (i == 0) {
                    a = 255;
                }
                uint32_t *d = &dst[y * s->w + x];
                uint8_t dr = *d >> 16, dg = *d >> 8, db = *d;
                dr = (r * a + dr * (255 - a)) / 255;
                dg = (g * a + dg * (255 - a)) / 255;
                db = (b * a + db * (255 - a)) / 255;
                *d = 0xff000000u | dr << 16 | dg << 8 | db;
            }
        }
    }
    dpy_gfx_update_full(s->con);
}

static void fb_invalidate(void *opaque)
{
}

static const GraphicHwOps fb_ops = {
    .invalidate = fb_invalidate,
    .gfx_update = fb_update,
};

static char *get_fb(Object *o, Error **errp)
{
    return g_strdup(A6K_FBVIEW(o)->spec);
}

static void set_fb(Object *o, const char *v, Error **errp)
{
    FbviewState *s = A6K_FBVIEW(o);
    g_free(s->spec);
    s->spec = g_strdup(v);
    parse(s);
}

static void fb_realize(DeviceState *dev, Error **errp)
{
    FbviewState *s = A6K_FBVIEW(dev);
    parse(s);
    s->con = graphic_console_init(dev, 0, &fb_ops, s);
    qemu_console_resize(s->con, s->w, s->h);
}

static void fb_init(Object *obj)
{
    object_property_add_str(obj, "fb", get_fb, set_fb);
}

static void fb_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = fb_realize;
}

static const TypeInfo fb_info = {
    .name = TYPE_A6K_FBVIEW,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(FbviewState),
    .instance_init = fb_init,
    .class_init = fb_class_init,
};

static void fb_register(void)
{
    type_register_static(&fb_info);
}
type_init(fb_register)

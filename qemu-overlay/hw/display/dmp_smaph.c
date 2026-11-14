/*
 * DMP SMAPH-S30 ("SUGILITE"/"GPE") GPU of the CXD90014 (a5000).
 * PA 0xF1000000, 16 KiB, GIC IRQ 76 (level high).
 *
 * Interface from grm_gles.ko (sugilite_interrupt / sugilite_register_init)
 * and libObj.so rm_* (docs/RE_DISPLAY_GPU.md §2):
 *   +0x10/+0x14/+0x18/+0x1C  PSC0 memory fill: start>>3, end>>3, value,
 *                            ctrl (bit0 start, bits8-9 width: 0=16, 1=24,
 *                            2=32 bit) -> status bit26, ack: +0x1C = 0
 *   +0xC00..+0xC28           PPF display transfer / texture copy, kick
 *                            +0xC18 != 0 -> status bit30, ack: +0xC18 = 0
 *   +0x1804 / +0x1800        P3D command list address / size+kick
 *                            -> status bit31, ack: +0x1000 = 0
 *   +0x34                    IRQ status (the ISR BUG()s if it reads 0)
 *
 * PSC fill, PPF transfers and P3D command lists are executed.  PPF follows
 * the PICA200 layout (8x8 Morton tiles) with addresses >>2 [hypothesis, see
 * docs/RE_DISPLAY_GPU.md].
 *
 * P3D (docs/RE_GPU_P3D_CMD.md): the list at +0x1804 holds +0x1800 8-byte
 * entries [header][value], built by libObj sub_5FC750 / sub_5FC790 /
 * sub_5FC720:
 *   header bit0       call: value = PA of a sub-list, bits 3-11 = entries
 *   header bit2       burst: bits 3-11 = words - 1, extra words follow
 *                     (padded to 8 bytes); bit1 = register auto-increment
 *   header bits 12-15 byte enable, bits 16-27 register id
 * Register writes update a 4096-entry P3D register file; the draw kick
 * (reg 0x202) runs vertex fetch -> vertex program -> primitive assembly ->
 * rasterisation -> fragment program -> fragment ops in software.  The
 * programmable stages call dmp_shader_run() (hw/display/dmp_shader.c).
 * Fragment ops: alpha test, stencil test/ops, depth test/write (D16, D24,
 * D24S8), blend, colour mask.  Textures: nearest/bilinear, mip levels with
 * a derivative-based LOD, edge/border/repeat/mirror wrap.  Points and
 * lines are rasterised directly (the geometry-stage programs that expand
 * them on the real GPU are not decoded).  Not modelled: ETC1, dither,
 * logic op, near-plane clipping, geometry programs.
 */

#include "qemu/osdep.h"
#include <math.h>
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "system/address-spaces.h"
#include "hw/display/dmp_shader.h"
#include "trace.h"

#define TYPE_DMP_SMAPH "dmp-smaph-s30"
OBJECT_DECLARE_SIMPLE_TYPE(SmaphState, DMP_SMAPH)

#define ST_P3D  0x80000000u
#define ST_PPF  0x40000000u
#define ST_PSC0 0x04000000u

#define P3D_NREG 0x1000

struct SmaphState {
    SysBusDevice parent_obj;
    MemoryRegion mr;
    uint32_t reg[0x4000 / 4];
    uint32_t status;
    qemu_irq irq;
    /* P3D */
    uint32_t p3d[P3D_NREG];
    DmpShaderProg *prog;        /* shared code / opdesc / uniform memory */
    uint32_t code_idx, opd_idx, unif_idx;
    uint64_t ndraw, npix;
    /* texture LOD: coordinate of fetch k at (x+1, y) / (x, y+1) */
    unsigned tex_rec;           /* 0 = normal run, 1/2 = recording dx/dy */
    unsigned tex_call;          /* fetch counter of the current run */
    bool tex_have_deriv;
    float tex_coord[2][16][2];
};
#define P3D_TEX_REC 16

static void smaph_update(SmaphState *s)
{
    qemu_set_irq(s->irq, s->status != 0);
}

static void psc_fill(SmaphState *s)
{
    hwaddr a = (hwaddr)s->reg[0x10 / 4] << 3, e = (hwaddr)s->reg[0x14 / 4] << 3;
    uint32_t v = s->reg[0x18 / 4], w = (s->reg[0x1C / 4] >> 8) & 3;
    unsigned bpp = w == 2 ? 4 : w == 1 ? 3 : 2;
    if (e <= a || e - a > 64 * MiB) {
        return;
    }
    g_autofree uint8_t *buf = g_malloc(e - a);
    for (hwaddr i = 0; i + bpp <= e - a; i += bpp) {
        for (unsigned b = 0; b < bpp; b++) {
            buf[i + b] = v >> (8 * b);
        }
    }
    address_space_write(&address_space_memory, a, MEMTXATTRS_UNSPECIFIED,
                        buf, e - a);
    trace_dmp_smaph_job("psc-fill", a, e - a);
}

static const unsigned ppf_bpp[8] = { 4, 3, 2, 2, 2, 4, 4, 4 };

static void px_to_rgba(const uint8_t *p, unsigned fmt, uint8_t o[4])
{
    uint16_t v = p[0] | p[1] << 8;
    switch (fmt) {
    case 0: o[0] = p[3]; o[1] = p[2]; o[2] = p[1]; o[3] = p[0]; break;
    case 1: o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = 0xff; break;
    case 2: o[0] = (v >> 11) << 3; o[1] = ((v >> 5) & 63) << 2;
            o[2] = (v & 31) << 3; o[3] = 0xff; break;
    case 3: o[0] = (v >> 11) << 3; o[1] = ((v >> 6) & 31) << 3;
            o[2] = ((v >> 1) & 31) << 3; o[3] = v & 1 ? 0xff : 0; break;
    default: o[0] = (v >> 12) << 4; o[1] = ((v >> 8) & 15) << 4;
            o[2] = ((v >> 4) & 15) << 4; o[3] = (v & 15) << 4; break;
    }
}

static void rgba_to_px(const uint8_t i[4], unsigned fmt, uint8_t *p)
{
    uint16_t v;
    switch (fmt) {
    case 0: p[0] = i[3]; p[1] = i[2]; p[2] = i[1]; p[3] = i[0]; return;
    case 1: p[0] = i[2]; p[1] = i[1]; p[2] = i[0]; return;
    case 2: v = (i[0] >> 3) << 11 | (i[1] >> 2) << 5 | i[2] >> 3; break;
    case 3: v = (i[0] >> 3) << 11 | (i[1] >> 3) << 6 | (i[2] >> 3) << 1 |
                (i[3] >> 7); break;
    default: v = (i[0] >> 4) << 12 | (i[1] >> 4) << 8 | (i[2] >> 4) << 4 |
                 (i[3] >> 4); break;
    }
    p[0] = v; p[1] = v >> 8;
}

/* PICA200 8x8 tile, Morton order inside the tile */
static size_t tiled_index(unsigned x, unsigned y, unsigned w)
{
    unsigned m = 0;
    for (int b = 0; b < 3; b++) {
        m |= ((x >> b) & 1) << (2 * b) | ((y >> b) & 1) << (2 * b + 1);
    }
    return ((size_t)(y / 8) * (w / 8) + x / 8) * 64 + m;
}

static void ppf_run(SmaphState *s)
{
    hwaddr src = (hwaddr)s->reg[0xC00 / 4] << 2, dst = (hwaddr)s->reg[0xC04 / 4] << 2;
    uint32_t flags = s->reg[0xC10 / 4];
    AddressSpace *as = &address_space_memory;

    if (flags & 8) {                    /* texture copy: plain byte copy */
        uint32_t size = s->reg[0xC20 / 4];
        if (size && size < 64 * MiB) {
            g_autofree uint8_t *b = g_malloc(size);
            address_space_read(as, src, MEMTXATTRS_UNSPECIFIED, b, size);
            address_space_write(as, dst, MEMTXATTRS_UNSPECIFIED, b, size);
        }
        trace_dmp_smaph_job("ppf-copy", dst, size);
        return;
    }
    unsigned iw = s->reg[0xC08 / 4] & 0xffff, ih = s->reg[0xC08 / 4] >> 16;
    unsigned ow = s->reg[0xC0C / 4] & 0xffff, oh = s->reg[0xC0C / 4] >> 16;
    unsigned ifmt = (flags >> 8) & 7, ofmt = (flags >> 12) & 7;
    bool to_tiled = flags & 2, flip = flags & 1;
    unsigned w = MIN(iw, ow), h = MIN(ih, oh);
    if (!w || !h || w > 4096 || h > 4096) {
        return;
    }
    size_t ib = (size_t)iw * ih * ppf_bpp[ifmt], ob = (size_t)ow * oh * ppf_bpp[ofmt];
    g_autofree uint8_t *in = g_malloc(ib);
    g_autofree uint8_t *out = g_malloc0(ob);
    address_space_read(as, src, MEMTXATTRS_UNSPECIFIED, in, ib);
    address_space_read(as, dst, MEMTXATTRS_UNSPECIFIED, out, ob);
    for (unsigned y = 0; y < h; y++) {
        for (unsigned x = 0; x < w; x++) {
            unsigned sy = flip ? h - 1 - y : y;
            size_t si = to_tiled ? (size_t)sy * iw + x : tiled_index(x, sy, iw);
            size_t di = to_tiled ? tiled_index(x, y, ow) : (size_t)y * ow + x;
            uint8_t px[4];
            px_to_rgba(in + si * ppf_bpp[ifmt], ifmt, px);
            rgba_to_px(px, ofmt, out + di * ppf_bpp[ofmt]);
        }
    }
    address_space_write(as, dst, MEMTXATTRS_UNSPECIFIED, out, ob);
    trace_dmp_smaph_job("ppf-transfer", dst, ob);
}

/*
 * ---------------------------------------------------------------------------
 * P3D: command-list processor and software renderer
 * (register map: docs/RE_GPU_P3D_CMD.md; evidence = libObj.so functions)
 * ---------------------------------------------------------------------------
 */

/* Until hw/display/dmp_shader.c is linked in, nothing can be shaded. */
bool __attribute__((weak))
dmp_shader_run(DmpShaderStage stage, const DmpShaderProg *prog,
               const DmpVec4 *in, unsigned nin, DmpVec4 *out, unsigned nout)
{
    return false;
}

/*
 * Which shader registers carry what.  hypothesis: vertex output o0 is the
 * clip-space position, every vertex output k is interpolated into fragment
 * input k, fragment output V1 is the colour (RGBA, 0..1; V0 = depth, per
 * dmp_shader.c).
 */
#define P3D_VS_OUT_POS   0
#define P3D_FS_OUT_COLOR 1
#define P3D_NVARY        DMP_SH_NUM_OUTPUT

#define P3D_MAX_VERTS    (1u << 20)
#define P3D_MAX_DEPTH    4

typedef struct P3dVtx {
    DmpVec4 o[P3D_NVARY];
    float wx, wy, wz, iw;           /* window x/y, depth 0..1, 1/w */
    bool ok;
} P3dVtx;

typedef struct P3dTarget {
    hwaddr pa;
    unsigned w, h, bpp, fmt;        /* fmt = PPF/PICA pixel format code */
    uint8_t *buf;                   /* shadow of the whole buffer */
    bool dirty;
    int sx0, sy0, sx1, sy1;         /* scissor, exclusive max */
    /* depth/stencil buffer (regs 0x10D-0x111), NULL if none */
    hwaddr zpa;
    unsigned zfmt, zbpp;            /* 0x10F: 0 D16, 2 D24, 3 D24S8 */
    uint8_t *zbuf;
    bool zdirty;
} P3dTarget;

static inline float p3d_f(uint32_t v)
{
    union { uint32_t u; float f; } c = { .u = v };
    return c.f;
}

static void p3d_write(SmaphState *s, unsigned reg, uint32_t v, unsigned mask);

/*
 * Execute one command list.  n = number of 8-byte entries (+0x1800 or
 * header bits 3-11 of a call entry, libObj sub_611080 tail:
 * `(8 * count) | 1`).
 */
static void p3d_run_list(SmaphState *s, hwaddr pa, uint32_t n, int depth)
{
    if (!n) {
        return;
    }
    if (n > 0x100000 || depth > P3D_MAX_DEPTH) {
        qemu_log_mask(LOG_GUEST_ERROR, "dmp-smaph: P3D list 0x%" HWADDR_PRIx
                      " n=%u depth=%d rejected\n", pa, n, depth);
        return;
    }
    g_autofree uint32_t *w = g_new(uint32_t, 2 * n);
    address_space_read(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED,
                       w, 8 * n);
    for (uint32_t i = 0; i < n;) {
        uint32_t hdr = le32_to_cpu(w[2 * i]), val = le32_to_cpu(w[2 * i + 1]);
        unsigned reg = (hdr >> 16) & 0xfff, mask = (hdr >> 12) & 0xf;
        i++;
        if (hdr & 1) {                          /* call sub-list */
            p3d_run_list(s, val, (hdr >> 3) & 0x1ff, depth + 1);
            continue;
        }
        if (hdr & 4) {                          /* burst (sub_5FC790) */
            unsigned extra = (hdr >> 3) & 0x1ff;
            bool inc = hdr & 2;
            p3d_write(s, reg, val, mask);
            for (unsigned k = 0; k < extra; k++) {
                uint64_t wi = 2 * (uint64_t)i + k;
                if (wi >= 2 * (uint64_t)n) {
                    break;
                }
                p3d_write(s, inc ? reg + 1 + k : reg, le32_to_cpu(w[wi]),
                          mask);
            }
            i += (extra + 1) / 2;
            continue;
        }
        if (hdr & 0xff8) {
            qemu_log_mask(LOG_UNIMP, "dmp-smaph: P3D header 0x%08x\n", hdr);
        }
        if (mask) {
            p3d_write(s, reg, val, mask);
        }
    }
}

/* ---- vertex fetch (regs 0x203-0x225, libObj sub_5FD990) ---- */

static float p3d_fetch_comp(const uint8_t *p, unsigned type)
{
    bool norm = type & 8;
    switch (type & 7) {
    case 0: return norm ? MAX((int8_t)p[0] / 127.0f, -1.0f) : (int8_t)p[0];
    case 1: return norm ? p[0] / 255.0f : p[0];
    case 2: {
        int16_t v = (int16_t)lduw_le_p(p);
        return norm ? MAX(v / 32767.0f, -1.0f) : v;
    }
    case 7: {
        uint16_t v = lduw_le_p(p);
        return norm ? v / 65535.0f : v;
    }
    case 3: return p3d_f(ldl_le_p(p));
    default: return 0;
    }
}

static unsigned p3d_type_size(unsigned type)
{
    switch (type & 7) {
    case 0: case 1: return 1;
    case 2: case 7: return 2;
    default: return 4;
    }
}

static unsigned nib(const uint32_t *r, unsigned base, unsigned i)
{
    return (r[base + i / 8] >> (4 * (i % 8))) & 0xf;
}

/* Fill the vertex-program input registers for vertex index idx. */
static void p3d_fetch_vertex(SmaphState *s, uint32_t idx, DmpVec4 *in)
{
    const uint32_t *r = s->p3d;
    unsigned nbuf = (r[0x20D] & 0xf) + 1;

    for (unsigned i = 0; i < DMP_SH_NUM_INPUT; i++) {
        in[i] = (DmpVec4){ { 0, 0, 0, 1 } };
    }
    for (unsigned b = 0; b < nbuf; b++) {
        unsigned nattr = nib(r, 0x20E, b) + 1, first = nib(r, 0x210, b);
        unsigned stride = ((r[0x212 + b / 4] >> (8 * (b % 4))) & 0xff) + 1;
        bool constant = r[0x20D] & (0x10000u << b);
        hwaddr base = r[0x216 + b] + (constant ? 0 : (hwaddr)idx * stride);
        uint8_t vb[256];

        address_space_read(&address_space_memory, base,
                           MEMTXATTRS_UNSPECIFIED, vb, stride);
        for (unsigned a = 0; a < nattr; a++) {
            unsigned slot = first + a;
            if (slot >= 16) {
                break;
            }
            unsigned type = nib(r, 0x206, slot);
            unsigned ncomp = ((r[0x208] >> (2 * slot)) & 3) + 1;
            unsigned off = (r[0x209 + slot / 4] >> (8 * (slot % 4))) & 0xff;
            unsigned ireg = nib(r, 0x204, slot);
            unsigned sz = p3d_type_size(type);
            for (unsigned c = 0; c < ncomp; c++) {
                if (off + (c + 1) * sz <= stride) {
                    in[ireg].v[c] = p3d_fetch_comp(vb + off + c * sz, type);
                }
            }
        }
    }
}

/* ---- textures (regs 0x580-0x5E4, libObj sub_5FCED0 / sub_5FD100) ---- */

static unsigned p3d_tex_bpp(unsigned fmt)
{
    switch (fmt) {
    case 0: case 25: return 4;
    case 1: return 3;
    case 7: case 8: return 1;
    case 12: return 0;
    default: return 2;
    }
}

/*
 * Mip level geometry.  0x586+11u = allocated (8-aligned) size of level 0,
 * 0x589+11u = PA >> 3 of level 0.  hypothesis (PICA200 layout, no stock
 * list uses mipmaps yet): level k follows level k-1 in memory, allocated
 * size = max(8, aligned size of level 0 >> k) in each direction.
 */
static hwaddr p3d_tex_level(const uint32_t *t, unsigned level,
                            unsigned *aw, unsigned *ah)
{
    unsigned bpp = p3d_tex_bpp(t[10] & 0x1f);
    hwaddr base = (hwaddr)t[4] << 3;

    *aw = t[1] >> 16;
    *ah = t[1] & 0xffff;
    for (unsigned k = 0; k < level && bpp; k++) {
        base += (hwaddr)*aw * *ah * bpp;
        *aw = MAX(*aw >> 1, 8);
        *ah = MAX(*ah >> 1, 8);
    }
    return base;
}

/*
 * Texel (x, y) of a mip level of unit; x or y < 0 = border colour
 * (0x585+11u = r << 24 | g << 16 | b << 8 | a, sub_5FD100).
 */
static void p3d_texel(SmaphState *s, unsigned unit, unsigned level, int x,
                      int y, float rgba[4])
{
    const uint32_t *t = &s->p3d[0x585 + 11 * unit];
    unsigned aw, ah;
    unsigned fmt = t[10] & 0x1f;
    hwaddr base = p3d_tex_level(t, level, &aw, &ah);
    size_t i;
    uint8_t p[4] = { 0 };
    unsigned bpp = p3d_tex_bpp(fmt);

    if (x < 0 || y < 0) {
        for (int c = 0; c < 4; c++) {
            rgba[c] = ((t[0] >> (24 - 8 * c)) & 0xff) / 255.0f;
        }
        return;
    }
    if (!aw || !ah) {
        rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0;
        return;
    }
    x = MIN(x, (int)aw - 1);
    y = MIN(y, (int)ah - 1);
    /* bit12: texture object +48 (sub_637740: "not tiled") [hypothesis] */
    i = (t[10] & 0x1000) ? (size_t)y * aw + x : tiled_index(x, y, aw);
    if (bpp) {
        address_space_read(&address_space_memory, base + i * bpp,
                           MEMTXATTRS_UNSPECIFIED, p, bpp);
    }
    uint16_t v = p[0] | p[1] << 8;
    float r = 0, g = 0, b = 0, a = 1;
    switch (fmt) {
    case 0:     /* PICA RGBA8: bytes A B G R */
        r = p[3] / 255.0f; g = p[2] / 255.0f; b = p[1] / 255.0f;
        a = p[0] / 255.0f;
        break;
    case 25:    /* GL_RGBA/UNSIGNED_BYTE: bytes R G B A [hypothesis] */
        r = p[0] / 255.0f; g = p[1] / 255.0f; b = p[2] / 255.0f;
        a = p[3] / 255.0f;
        break;
    case 1:
        r = p[2] / 255.0f; g = p[1] / 255.0f; b = p[0] / 255.0f;
        break;
    case 2:
        r = (v >> 11) / 31.0f; g = ((v >> 6) & 31) / 31.0f;
        b = ((v >> 1) & 31) / 31.0f; a = v & 1;
        break;
    case 3:
        r = (v >> 11) / 31.0f; g = ((v >> 5) & 63) / 63.0f;
        b = (v & 31) / 31.0f;
        break;
    case 4:
        r = (v >> 12) / 15.0f; g = ((v >> 8) & 15) / 15.0f;
        b = ((v >> 4) & 15) / 15.0f; a = (v & 15) / 15.0f;
        break;
    case 5:     /* LA8: byte0 = A, byte1 = L (PICA IA8) [hypothesis] */
        r = g = b = p[1] / 255.0f; a = p[0] / 255.0f;
        break;
    case 7:
        r = g = b = p[0] / 255.0f;
        break;
    case 8:
        r = g = b = 0; a = p[0] / 255.0f;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "dmp-smaph: texture format %u\n", fmt);
        r = b = 1; g = 0;
        break;
    }
    rgba[0] = r; rgba[1] = g; rgba[2] = b; rgba[3] = a;
}

/*
 * Wrap mode of 0x5DD+u bits 12-13 (S) / 8-9 (T), from sub_5FCED0:
 * 0 GL_CLAMP_TO_EDGE, 1 GL_CLAMP_TO_BORDER, 2 GL_REPEAT, 3 GL_MIRRORED_REPEAT.
 * Returns -1 for a border texel.
 */
static int p3d_wrap(int c, unsigned n, unsigned mode)
{
    switch (mode) {
    case 1:                                         /* border */
        return c < 0 || c >= (int)n ? -1 : c;
    case 2:                                         /* repeat */
        c %= (int)n;
        return c < 0 ? c + n : c;
    case 3: {                                       /* mirrored repeat */
        int p = c % (2 * (int)n);
        p = p < 0 ? p + 2 * n : p;
        return p < (int)n ? p : 2 * n - 1 - p;
    }
    default:                                        /* clamp to edge */
        return MIN(MAX(c, 0), (int)n - 1);
    }
}

/* nearest / bilinear sample of one mip level */
static void p3d_tex_filter(SmaphState *s, unsigned unit, unsigned level,
                           bool lin, const float coord[4], float rgba[4])
{
    const uint32_t *t = &s->p3d[0x585 + 11 * unit];
    uint32_t smp = s->p3d[0x5DD + unit];
    unsigned w = t[2] >> 16, h = t[2] & 0xffff;
    unsigned ws = (smp >> 12) & 3, wt = (smp >> 8) & 3;

    if (!w || !h) {
        w = t[1] >> 16;
        h = t[1] & 0xffff;
    }
    w = MAX(w >> level, 1);
    h = MAX(h >> level, 1);
    float u = coord[0] * w, v = coord[1] * h;
    if (!lin) {
        p3d_texel(s, unit, level, p3d_wrap(floorf(u), w, ws),
                  p3d_wrap(floorf(v), h, wt), rgba);
        return;
    }
    float fu = u - 0.5f, fv = v - 0.5f;
    int x0 = floorf(fu), y0 = floorf(fv);
    float ax = fu - x0, ay = fv - y0, c[4][4];
    for (int k = 0; k < 4; k++) {
        p3d_texel(s, unit, level, p3d_wrap(x0 + (k & 1), w, ws),
                  p3d_wrap(y0 + (k >> 1), h, wt), c[k]);
    }
    for (int j = 0; j < 4; j++) {
        float top = c[0][j] + (c[1][j] - c[0][j]) * ax;
        float bot = c[2][j] + (c[3][j] - c[2][j]) * ax;
        rgba[j] = top + (bot - top) * ay;
    }
}

/* max mip level of unit (0x588+11u bits 24-27, sub_5FD100); 0 = no mips */
static unsigned p3d_tex_maxlevel(SmaphState *s, unsigned unit)
{
    return (s->p3d[0x588 + 11 * unit] >> 24) & 0xf;
}

/*
 * Texture fetch callback of the shader interpreter (opcode 0x20).
 *
 * Sampler 0x5DD+u (sub_5FCED0): bit1 = mag LINEAR, bit2 = min LINEAR
 * (GL_LINEAR, GL_LINEAR_MIPMAP_*), bit24 = linear between mip levels
 * (GL_*_MIPMAP_LINEAR).  GL_*_MIPMAP_NEAREST sets neither bit 24; the level
 * range is in 0x588+11u: bits 16-19 min level, 24-27 max level (both 0
 * for the non-mipmap min filters), bits 0-12 LOD bias, signed 5.8 fixed
 * point (sub_5FD100: (bias + 16) * 256 folded to 13 bits).
 *
 * The level of detail needs screen-space derivatives.  For draws that
 * have a unit with max level > 0 the rasteriser runs the fragment program
 * once more at (x+1, y) and (x, y+1) with s->tex_rec set; those runs only
 * record the coordinate of every fetch (call order k), and the real run
 * then derives d(u,v)/dx and d(u,v)/dy per fetch (p3d_shade_pixel).
 */
static void p3d_tex_sample(void *opaque, unsigned unit, const float coord[4],
                           float rgba[4])
{
    SmaphState *s = opaque;
    unsigned k = s->tex_call++;

    if (unit >= 8) {
        rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0;
        return;
    }
    uint32_t smp = s->p3d[0x5DD + unit];
    uint32_t lod = s->p3d[0x588 + 11 * unit];
    unsigned maxl = (lod >> 24) & 0xf, minl = MIN((lod >> 16) & 0xf, maxl);

    if (s->tex_rec && k < P3D_TEX_REC) {
        s->tex_coord[s->tex_rec - 1][k][0] = coord[0];
        s->tex_coord[s->tex_rec - 1][k][1] = coord[1];
    }
    if (s->tex_rec || !maxl || k >= P3D_TEX_REC || !s->tex_have_deriv) {
        /* no mip chain (or a recording pass): level 0, mag/min filter */
        p3d_tex_filter(s, unit, minl, smp & (maxl ? 4 : 2), coord, rgba);
        return;
    }
    const uint32_t *t = &s->p3d[0x585 + 11 * unit];
    float w = t[2] >> 16, h = t[2] & 0xffff;
    float dux = (s->tex_coord[0][k][0] - coord[0]) * w;
    float dvx = (s->tex_coord[0][k][1] - coord[1]) * h;
    float duy = (s->tex_coord[1][k][0] - coord[0]) * w;
    float dvy = (s->tex_coord[1][k][1] - coord[1]) * h;
    float rho = MAX(sqrtf(dux * dux + dvx * dvx),
                    sqrtf(duy * duy + dvy * dvy));
    int bias = (int)(lod & 0x1fff) - ((lod & 0x1000) ? 0x2000 : 0);
    float lambda = (rho > 0 ? log2f(rho) : -128.0f) + bias / 256.0f;

    if (lambda <= 0) {                          /* magnification */
        p3d_tex_filter(s, unit, minl, smp & 2, coord, rgba);
        return;
    }
    lambda = MIN(MAX(lambda + minl, (float)minl), (float)maxl);
    if (!(smp & 0x1000000)) {                   /* *_MIPMAP_NEAREST */
        unsigned l = MIN((unsigned)(lambda + 0.5f), maxl);
        p3d_tex_filter(s, unit, l, smp & 4, coord, rgba);
        return;
    }
    unsigned l0 = (unsigned)lambda, l1 = MIN(l0 + 1, maxl);
    float f = lambda - l0, c0[4], c1[4];
    p3d_tex_filter(s, unit, l0, smp & 4, coord, c0);
    p3d_tex_filter(s, unit, l1, smp & 4, coord, c1);
    for (int j = 0; j < 4; j++) {
        rgba[j] = c0[j] + (c1[j] - c0[j]) * f;
    }
}

/* ---- colour buffer and fragment operations ---- */

/*
 * Colour format: reg 0x110 bits 16-18 (sub_600EB0 `v13[2] << 16`; codes
 * from sub_613820 / readback sub_633E00): 0 RGBA8 (PICA word order), 2
 * RGB5A1, 3 RGB565, 4 RGBA4, 5 RGBA8 in GL byte order (R,G,B,A).  The UI's
 * 640x480 OSD target uses 4 and is PPF-transferred with input format 4.
 */
#define P3D_FMT_RGBA8_GL 5

static void p3d_target_format(SmaphState *s, P3dTarget *t)
{
    t->fmt = (s->p3d[0x110] >> 16) & 7;
    switch (t->fmt) {
    case 2: case 3: case 4:
        t->bpp = 2;
        break;
    case 0: case P3D_FMT_RGBA8_GL:
        t->bpp = 4;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "dmp-smaph: colour format %u\n", t->fmt);
        t->fmt = 0;
        t->bpp = 4;
        break;
    }
}

static void p3d_px_read(const uint8_t *p, unsigned fmt, uint8_t o[4])
{
    /* PPF code 2 is RGB565, P3D code 2 is RGB5A1: swap 2 <-> 3 */
    if (fmt == P3D_FMT_RGBA8_GL) {
        memcpy(o, p, 4);
    } else {
        px_to_rgba(p, fmt == 2 ? 3 : fmt == 3 ? 2 : fmt, o);
    }
}

static void p3d_px_write(const uint8_t i[4], unsigned fmt, uint8_t *p)
{
    if (fmt == P3D_FMT_RGBA8_GL) {
        memcpy(p, i, 4);
    } else {
        rgba_to_px(i, fmt == 2 ? 3 : fmt == 3 ? 2 : fmt, p);
    }
}

/* compare functions of 0x104 / 0x105 / 0x107 (sub_600EB0) */
static bool p3d_compare(unsigned f, unsigned a, unsigned ref)
{
    switch (f & 7) {
    case 0: return false;
    case 1: return true;
    case 2: return a == ref;
    case 3: return a != ref;
    case 4: return a < ref;
    case 5: return a <= ref;
    case 6: return a > ref;
    default: return a >= ref;
    }
}

/* PICA200 blend factor encoding (reg 0x101) [hypothesis, see RE doc] */
static float p3d_blend_factor(unsigned f, const float *sc, const float *dc,
                              const float *cc, int ch)
{
    switch (f) {
    case 0: return 0;
    case 1: return 1;
    case 2: return sc[ch];
    case 3: return 1 - sc[ch];
    case 4: return dc[ch];
    case 5: return 1 - dc[ch];
    case 6: return sc[3];
    case 7: return 1 - sc[3];
    case 8: return dc[3];
    case 9: return 1 - dc[3];
    case 10: return cc[ch];
    case 11: return 1 - cc[ch];
    case 12: return cc[3];
    case 13: return 1 - cc[3];
    case 14: return ch == 3 ? 1 : MIN(sc[3], 1 - dc[3]);
    default: return 1;
    }
}

static float p3d_blend_eq(unsigned eq, float s, float d)
{
    switch (eq) {
    case 1: return s - d;
    case 2: return d - s;
    case 3: return MIN(s, d);
    case 4: return MAX(s, d);
    default: return s + d;
    }
}

/*
 * Stencil operations of reg 0x106 (sub_600EB0 maps GL_KEEP 0, GL_ZERO 1,
 * GL_REPLACE 2, GL_INCR 3, GL_DECR 4, GL_INVERT 5, GL_INCR_WRAP 6,
 * GL_DECR_WRAP 7).
 */
static unsigned p3d_stencil_op(unsigned op, unsigned v, unsigned ref)
{
    switch (op & 7) {
    case 1: return 0;
    case 2: return ref;
    case 3: return v < 255 ? v + 1 : 255;
    case 4: return v ? v - 1 : 0;
    case 5: return ~v & 0xff;
    case 6: return (v + 1) & 0xff;
    case 7: return (v - 1) & 0xff;
    default: return v;
    }
}

/*
 * Depth / stencil buffer access.  Layout follows the colour buffer (8x8
 * Morton tiles, row 0 = bottom); D16 = 16-bit LE, D24 = 3 bytes LE, D24S8 =
 * 32-bit LE word with depth in bits 0-23 and stencil in bits 24-31.
 * hypothesis: PICA200 depth buffer layout, not yet seen in a stock list.
 */
static uint32_t p3d_zs_read(const P3dTarget *t, size_t i)
{
    const uint8_t *p = t->zbuf + i * t->zbpp;
    switch (t->zbpp) {
    case 2: return lduw_le_p(p);
    case 3: return p[0] | p[1] << 8 | p[2] << 16;
    default: return ldl_le_p(p);
    }
}

static void p3d_zs_write(P3dTarget *t, size_t i, uint32_t v)
{
    uint8_t *p = t->zbuf + i * t->zbpp;
    switch (t->zbpp) {
    case 2: stw_le_p(p, v); break;
    case 3: p[0] = v; p[1] = v >> 8; p[2] = v >> 16; break;
    default: stl_le_p(p, v); break;
    }
    t->zdirty = true;
}

/*
 * Stencil and depth test for the fragment at tiled index i with window
 * depth z (0..1).  Returns false if the fragment is discarded.  Register
 * meanings from sub_600EB0 (see docs/RE_GPU_P3D_CMD.md §3.2):
 *   0x105 bit0 enable, 4-6 func, 8-15 value mask, 16-23 ref, 24-31 write mask
 *   0x106 0-2 sfail, 4-6 zfail, 8-10 zpass
 *   0x107 bit0 depth test, 4-6 func, bit12 depth write
 *   0x10D / 0x10E bit0 stencil, bit1 depth read / write enable
 */
static bool p3d_depth_stencil(SmaphState *s, P3dTarget *t, size_t i, float z)
{
    const uint32_t *r = s->p3d;
    bool has_st = t->zfmt == 3;
    bool st_en = has_st && (r[0x105] & 1);
    bool z_en = r[0x107] & 1;
    unsigned zbits = t->zbpp == 2 ? 16 : 24;
    uint32_t zmax = (1u << zbits) - 1;
    uint32_t old, val, zold, zf;
    unsigned st = 0, nst, smask, sref, swmask;
    bool pass_z;

    if (!t->zbuf || (!st_en && !z_en)) {
        return true;
    }
    old = p3d_zs_read(t, i);
    val = old;
    zold = old & zmax;
    z = MIN(MAX(z, 0.0f), 1.0f);
    zf = (uint32_t)(z * zmax + 0.5f);
    if (st_en) {
        st = old >> 24;
        smask = (r[0x105] >> 8) & 0xff;
        sref = (r[0x105] >> 16) & 0xff;
        swmask = r[0x105] >> 24;
        if (!(r[0x10E] & 1)) {
            swmask = 0;
        }
        if (!p3d_compare(r[0x105] >> 4, sref & smask, st & smask)) {
            nst = p3d_stencil_op(r[0x106], st, sref);
            val = (val & ~(swmask << 24)) | ((nst & swmask) << 24);
            if (val != old) {
                p3d_zs_write(t, i, val);
            }
            return false;
        }
    }
    pass_z = !z_en || p3d_compare(r[0x107] >> 4, zf, zold);
    if (st_en) {
        nst = p3d_stencil_op(r[0x106] >> (pass_z ? 8 : 4), st, sref);
        val = (val & ~(swmask << 24)) | ((nst & swmask) << 24);
    }
    if (pass_z && z_en && (r[0x107] & 0x1000) && (r[0x10E] & 2)) {
        val = (val & ~zmax) | zf;
    }
    if (val != old) {
        p3d_zs_write(t, i, val);
    }
    return pass_z;
}

static void p3d_fragment_ops(SmaphState *s, P3dTarget *t, const uint8_t src[4],
                             size_t pi, float z)
{
    const uint32_t *r = s->p3d;
    uint8_t dst[4], res[4], *px = t->buf + pi * t->bpp;
    float sc[4], dc[4], cc[4], o[4];
    uint32_t bf = r[0x101], bc = r[0x103];
    unsigned wmask = (r[0x107] >> 8) & 0xf;     /* RGBA enables */

    if ((r[0x104] & 1) &&
        !p3d_compare(r[0x104] >> 4, src[3], (r[0x104] >> 8) & 0xff)) {
        return;                                 /* alpha test */
    }
    if (!p3d_depth_stencil(s, t, pi, z)) {
        return;
    }
    if (!(r[0x10C] & 0xf)) {
        return;                                 /* colour writes off */
    }
    p3d_px_read(px, t->fmt, dst);
    if (r[0x100] & 0x100) {                     /* blend enabled */
        for (int c = 0; c < 4; c++) {
            sc[c] = src[c] / 255.0f;
            dc[c] = dst[c] / 255.0f;
            cc[c] = ((bc >> (8 * c)) & 0xff) / 255.0f;
        }
        for (int c = 0; c < 4; c++) {
            unsigned eq = c < 3 ? bf & 7 : (bf >> 8) & 7;
            unsigned fs = c < 3 ? (bf >> 16) & 15 : (bf >> 24) & 15;
            unsigned fd = c < 3 ? (bf >> 20) & 15 : (bf >> 28) & 15;
            o[c] = p3d_blend_eq(eq, sc[c] * p3d_blend_factor(fs, sc, dc, cc, c),
                                dc[c] * p3d_blend_factor(fd, sc, dc, cc, c));
            o[c] = MIN(MAX(o[c], 0.0f), 1.0f);
            res[c] = (uint8_t)(o[c] * 255.0f + 0.5f);
        }
    } else {
        memcpy(res, src, 4);
    }
    for (int c = 0; c < 4; c++) {
        if (!(wmask & (1 << c))) {
            res[c] = dst[c];
        }
    }
    p3d_px_write(res, t->fmt, px);
    t->dirty = true;
}

/*
 * Colour buffer: PA = reg 0x112 << 4, size = reg 0x113 (w | h << 16), 8x8
 * Morton tiled like the PICA200 (the PPF display transfer that follows reads
 * it with the tiled->linear flag).  Row 0 is the bottom (GL window y), the
 * PPF flips it (flag bit0).
 */
static bool p3d_target_open(SmaphState *s, P3dTarget *t)
{
    const uint32_t *r = s->p3d;
    t->pa = (hwaddr)r[0x112] << 4;
    t->w = r[0x113] & 0x1fff;
    t->h = (r[0x113] >> 16) & 0x1fff;
    p3d_target_format(s, t);
    if (!t->pa || !t->w || !t->h || t->w > 4096 || t->h > 4096) {
        return false;
    }
    t->buf = g_malloc((size_t)t->w * t->h * t->bpp);
    address_space_read(&address_space_memory, t->pa, MEMTXATTRS_UNSPECIFIED,
                       t->buf, (size_t)t->w * t->h * t->bpp);
    t->dirty = false;

    /* scissor, rasteriser coordinates = 2 * pixel - 0x49 (sub_600EB0) */
    int cx2 = r[0x49] & 0xffff, cy2 = r[0x49] >> 16;
    int x0 = (int16_t)r[0x47], y0 = (int16_t)(r[0x47] >> 16);
    int x1 = (int16_t)r[0x48], y1 = (int16_t)(r[0x48] >> 16);
    t->sx0 = MAX((x0 + cx2) / 2, 0);
    t->sy0 = MAX((y0 + cy2) / 2, 0);
    t->sx1 = MIN((x1 + cx2) / 2, (int)t->w);
    t->sy1 = MIN((y1 + cy2) / 2, (int)t->h);

    /*
     * Depth/stencil buffer: 0x111 = PA >> 4, 0x10F = format, same size as
     * the colour buffer.  Only opened when a test that needs it is on.
     */
    t->zbuf = NULL;
    t->zdirty = false;
    t->zfmt = r[0x10F] & 3;
    t->zbpp = t->zfmt == 0 ? 2 : t->zfmt == 2 ? 3 : 4;
    t->zpa = (hwaddr)r[0x111] << 4;
    if (t->zpa && (r[0x10D] & 3) &&
        ((r[0x107] & 1) || (t->zfmt == 3 && (r[0x105] & 1)))) {
        if (t->zfmt == 1) {
            qemu_log_mask(LOG_UNIMP, "dmp-smaph: depth format 1\n");
        }
        t->zbuf = g_malloc((size_t)t->w * t->h * t->zbpp);
        address_space_read(&address_space_memory, t->zpa,
                           MEMTXATTRS_UNSPECIFIED, t->zbuf,
                           (size_t)t->w * t->h * t->zbpp);
    }
    return true;
}

static void p3d_target_close(P3dTarget *t)
{
    if (t->dirty) {
        address_space_write(&address_space_memory, t->pa,
                            MEMTXATTRS_UNSPECIFIED, t->buf,
                            (size_t)t->w * t->h * t->bpp);
    }
    g_free(t->buf);
    t->buf = NULL;
    if (t->zdirty) {
        address_space_write(&address_space_memory, t->zpa,
                            MEMTXATTRS_UNSPECIFIED, t->zbuf,
                            (size_t)t->w * t->h * t->zbpp);
    }
    g_free(t->zbuf);
    t->zbuf = NULL;
}

/* ---- rasteriser ---- */

typedef struct P3dDraw {
    SmaphState *s;
    P3dTarget t;
    uint32_t fs_entry;
    unsigned cull;              /* reg 0x41 bits 0-1: 1 cull CCW, 2 cull CW */
    bool lod;                   /* a bound unit has mip levels (LOD needed) */
} P3dDraw;

/*
 * Perspective-correct varyings at window point (px, py) from barycentric
 * weights; input 0 becomes the window position (x, y, z, 1/w) like
 * gl_FragCoord.  Returns false for a degenerate point (sum of 1/w = 0).
 */
static bool p3d_interp(const P3dVtx *a, const P3dVtx *b, const P3dVtx *c,
                       float w0, float w1, float w2, float px, float py,
                       DmpVec4 *vary)
{
    float p0 = w0 * a->iw, p1 = w1 * b->iw, p2 = w2 * c->iw;
    float ps = p0 + p1 + p2;

    if (ps == 0 || ps != ps) {
        return false;
    }
    p0 /= ps; p1 /= ps; p2 /= ps;
    for (int k = 0; k < P3D_NVARY; k++) {
        for (int j = 0; j < 4; j++) {
            vary[k].v[j] = a->o[k].v[j] * p0 + b->o[k].v[j] * p1 +
                           c->o[k].v[j] * p2;
        }
    }
    vary[P3D_VS_OUT_POS].v[0] = px;
    vary[P3D_VS_OUT_POS].v[1] = py;
    vary[P3D_VS_OUT_POS].v[2] = w0 * a->wz + w1 * b->wz + w2 * c->wz;
    vary[P3D_VS_OUT_POS].v[3] = ps;
    return true;
}

/*
 * Run the fragment program for pixel (x, y).  ndx / ndy are the varyings
 * at (x+1, y) / (x, y+1) when the draw needs a texture LOD (else NULL):
 * they are run first in recording mode (see p3d_tex_sample).
 */
static void p3d_shade_pixel(P3dDraw *d, int x, int y, const DmpVec4 *vary,
                            const DmpVec4 *ndx, const DmpVec4 *ndy)
{
    SmaphState *s = d->s;
    DmpVec4 out[DMP_SH_NUM_OUTPUT];
    uint8_t src[4];
    float z;

    s->prog->entry = d->fs_entry;
    s->tex_have_deriv = false;
    if (ndx && ndy) {
        for (unsigned r = 1; r <= 2; r++) {
            memset(out, 0, sizeof(out));
            s->tex_rec = r;
            s->tex_call = 0;
            dmp_shader_run(DMP_SH_FRAGMENT, s->prog, r == 1 ? ndx : ndy,
                           P3D_NVARY, out, DMP_SH_NUM_OUTPUT);
        }
        s->tex_rec = 0;
        s->tex_have_deriv = true;
    }
    memset(out, 0, sizeof(out));
    s->tex_call = 0;
    if (!dmp_shader_run(DMP_SH_FRAGMENT, s->prog, vary, P3D_NVARY,
                        out, DMP_SH_NUM_OUTPUT)) {
        return;
    }
    for (int c = 0; c < 4; c++) {
        float f = out[P3D_FS_OUT_COLOR].v[c];
        f = f != f ? 0 : MIN(MAX(f, 0.0f), 1.0f);
        src[c] = (uint8_t)(f * 255.0f + 0.5f);
    }
    /*
     * Fragment depth: FS output 0 starts as input 0 = (x, y, z_window, 1/w);
     * the GLES2 depth prologue (entry not skipped by +9) rewrites it.
     * hypothesis: the prologue result is in component z.  Polygon offset
     * 0x4A is in 1/16777000 units (sub_600EB0: units / 65535 * 16777000).
     */
    z = out[P3D_VS_OUT_POS].v[2] + (s->p3d[0x4A] & 0xffffff) / 16777000.0f;
    p3d_fragment_ops(s, &d->t, src, tiled_index(x, y, d->t.w), z);
    s->npix++;
}

static void p3d_triangle(P3dDraw *d, const P3dVtx *a, const P3dVtx *b,
                         const P3dVtx *c)
{
    if (!a->ok || !b->ok || !c->ok) {
        return;
    }
    float area = (b->wx - a->wx) * (c->wy - a->wy) -
                 (c->wx - a->wx) * (b->wy - a->wy);
    if (area == 0 || area != area) {
        return;
    }
    /* area > 0: counter-clockwise in GL window coordinates (y up) */
    if (((d->cull & 3) == 1 && area > 0) || ((d->cull & 3) == 2 && area < 0)) {
        return;
    }
    int x0 = MAX((int)floorf(MIN(MIN(a->wx, b->wx), c->wx)), d->t.sx0);
    int x1 = MIN((int)ceilf(MAX(MAX(a->wx, b->wx), c->wx)), d->t.sx1);
    int y0 = MAX((int)floorf(MIN(MIN(a->wy, b->wy), c->wy)), d->t.sy0);
    int y1 = MIN((int)ceilf(MAX(MAX(a->wy, b->wy), c->wy)), d->t.sy1);
    float inv = 1.0f / area;
    DmpVec4 vary[P3D_NVARY], vdx[P3D_NVARY], vdy[P3D_NVARY];

#define BARY(px, py, w0, w1) \
    float w0 = ((b->wx - (px)) * (c->wy - (py)) - \
                (c->wx - (px)) * (b->wy - (py))) * inv; \
    float w1 = ((c->wx - (px)) * (a->wy - (py)) - \
                (a->wx - (px)) * (c->wy - (py))) * inv
    for (int y = y0; y < y1; y++) {
        float py = y + 0.5f;
        for (int x = x0; x < x1; x++) {
            float px = x + 0.5f;
            BARY(px, py, w0, w1);
            float w2 = 1.0f - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) {
                continue;
            }
            /* top-left-ish tie break: drop pixels exactly on a far edge */
            if ((w0 == 0 && area * (c->wy - b->wy) < 0) ||
                (w1 == 0 && area * (a->wy - c->wy) < 0) ||
                (w2 == 0 && area * (b->wy - a->wy) < 0)) {
                continue;
            }
            if (!p3d_interp(a, b, c, w0, w1, w2, px, py, vary)) {
                continue;
            }
            if (d->lod) {
                /* neighbours may lie outside: plane extrapolation */
                BARY(px + 1, py, u0, u1);
                BARY(px, py + 1, v0, v1);
                if (p3d_interp(a, b, c, u0, u1, 1 - u0 - u1, px + 1, py,
                               vdx) &&
                    p3d_interp(a, b, c, v0, v1, 1 - v0 - v1, px, py + 1,
                               vdy)) {
                    p3d_shade_pixel(d, x, y, vary, vdx, vdy);
                    continue;
                }
            }
            p3d_shade_pixel(d, x, y, vary, NULL, NULL);
        }
    }
#undef BARY
}

static bool p3d_in_scissor(P3dDraw *d, int x, int y)
{
    return x >= d->t.sx0 && x < d->t.sx1 && y >= d->t.sy0 && y < d->t.sy1;
}

/*
 * Points and lines.  The driver sends GL_POINTS / GL_LINES / GL_LINE_STRIP
 * through the geometry stage (0x400 = 0x1000300, 0x405 from the GS output
 * count; sub_5FD990) with the driver-internal programs
 * dmp_point_shader_prog / dmp_line_shader_prog, which expand them to
 * triangles.  Those programs and the GS opcodes are not decoded yet, so
 * hypothesis/fallback: rasterise them here directly from the vertex
 * program outputs, 1 pixel wide (point size and line width are not
 * known), point varyings constant, line varyings interpolated
 * perspective-correct along the line.  No stock UI list uses this.
 */
static void p3d_point(P3dDraw *d, const P3dVtx *a)
{
    DmpVec4 vary[P3D_NVARY];
    int x = floorf(a->wx), y = floorf(a->wy);

    if (!a->ok || !p3d_in_scissor(d, x, y)) {
        return;
    }
    memcpy(vary, a->o, sizeof(vary));
    vary[P3D_VS_OUT_POS] = (DmpVec4){ { x + 0.5f, y + 0.5f, a->wz, a->iw } };
    p3d_shade_pixel(d, x, y, vary, NULL, NULL);
}

static void p3d_line(P3dDraw *d, const P3dVtx *a, const P3dVtx *b,
                     bool last)
{
    DmpVec4 vary[P3D_NVARY];
    float dx, dy;
    int n;

    if (!a->ok || !b->ok) {
        return;
    }
    dx = b->wx - a->wx;
    dy = b->wy - a->wy;
    n = (int)ceilf(MAX(fabsf(dx), fabsf(dy)));
    if (n > 8192) {
        return;
    }
    /* one fragment per step along the major axis (GL "diamond exit"
     * approximated); the end point belongs to the next segment */
    for (int i = 0; i < n + (last ? 1 : 0); i++) {
        float f = n ? (float)i / n : 0;
        float px = a->wx + dx * f, py = a->wy + dy * f;
        int x = floorf(px), y = floorf(py);
        if (!p3d_in_scissor(d, x, y) ||
            !p3d_interp(a, b, a, 1 - f, f, 0, x + 0.5f, y + 0.5f, vary)) {
            continue;
        }
        p3d_shade_pixel(d, x, y, vary, NULL, NULL);
    }
}

static void p3d_vertex(SmaphState *s, uint32_t idx, P3dVtx *v)
{
    const uint32_t *r = s->p3d;
    DmpVec4 in[DMP_SH_NUM_INPUT];

    memset(v, 0, sizeof(*v));
    p3d_fetch_vertex(s, idx, in);
    s->prog->entry = r[0x442];
    if (!dmp_shader_run(DMP_SH_VERTEX, s->prog, in, DMP_SH_NUM_INPUT,
                        v->o, P3D_NVARY)) {
        return;
    }
    const float *p = v->o[P3D_VS_OUT_POS].v;
    if (!(p[3] > 0)) {
        return;             /* no near-plane clipping [not implemented] */
    }
    /* viewport: 0x42/0x43 = w/2, h/2 (float), centre = 0x49 / 2 */
    float hw = p3d_f(r[0x42]), hh = p3d_f(r[0x43]);
    float cx = (r[0x49] & 0xffff) * 0.5f, cy = (r[0x49] >> 16) * 0.5f;
    v->iw = 1.0f / p[3];
    v->wx = cx + p[0] * v->iw * hw;
    v->wy = cy + p[1] * v->iw * hh;
    v->wz = p[2] * v->iw * 0.5f + 0.5f;
    v->ok = true;
}

static void p3d_draw(SmaphState *s, uint32_t kick)
{
    const uint32_t *r = s->p3d;
    uint32_t count = kick & 0x0fffffff;
    bool indexed = kick & 0x80000000u;
    unsigned isz = (kick & 0x20000000u) ? 4 : (kick & 0x10000000u) ? 2 : 1;
    unsigned topo = r[0x404] & 3;
    unsigned prim = r[0x401];
    P3dDraw d = { .s = s, .fs_entry = r[0x443], .cull = r[0x41] };

    s->ndraw++;
    if (!count || count > P3D_MAX_VERTS) {
        return;
    }
    if (!p3d_target_open(s, &d.t)) {
        qemu_log_mask(LOG_GUEST_ERROR, "dmp-smaph: draw without target\n");
        return;
    }
    /*
     * 0x401 (sub_5FD990, GL mode -> value): 0 GL_POINTS, 0x10 GL_LINES,
     * 0x802 GL_LINE_STRIP, 0x3020 GL_TRIANGLES, 0x20 strip / fan.
     */
    if (prim != 0x3020 && prim != 0x20 && prim != 0 && prim != 0x10 &&
        prim != 0x802) {
        qemu_log_mask(LOG_UNIMP, "dmp-smaph: primitive 0x%x\n", prim);
        p3d_target_close(&d.t);
        return;
    }
    for (unsigned u = 0; u < 3; u++) {
        if (p3d_tex_maxlevel(s, u)) {
            d.lod = true;
        }
    }
    for (unsigned i = 0; i < 64; i++) {
        s->prog->extra[i] = r[0x440 + i];
    }
    s->prog->tex_sample = p3d_tex_sample;
    s->prog->tex_opaque = s;

    g_autofree uint8_t *ib = NULL;
    if (indexed) {
        ib = g_malloc((size_t)count * isz);
        address_space_read(&address_space_memory, r[0x203],
                           MEMTXATTRS_UNSPECIFIED, ib, (size_t)count * isz);
    }
    g_autofree P3dVtx *vt = g_new(P3dVtx, count);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = !indexed ? r[0x203] + i :
                       isz == 4 ? ldl_le_p(ib + 4 * i) :
                       isz == 2 ? lduw_le_p(ib + 2 * i) : ib[i];
        uint32_t j;
        /* reuse an identical index from the last few (post-T&L cache) */
        for (j = i > 8 ? i - 8 : 0; j < i; j++) {
            uint32_t jdx = !indexed ? r[0x203] + j :
                           isz == 4 ? ldl_le_p(ib + 4 * j) :
                           isz == 2 ? lduw_le_p(ib + 2 * j) : ib[j];
            if (jdx == idx) {
                vt[i] = vt[j];
                break;
            }
        }
        if (j == i) {
            p3d_vertex(s, idx, &vt[i]);
        }
    }
    if (prim == 0 || prim == 0x10 || prim == 0x802) {
        static bool logged;
        if (!logged) {
            logged = true;
            qemu_log_mask(LOG_UNIMP, "dmp-smaph: point/line primitive 0x%x "
                          "rasterised without the geometry stage\n", prim);
        }
    }
    if (prim == 0) {
        for (uint32_t i = 0; i < count; i++) {
            p3d_point(&d, &vt[i]);
        }
        topo = 3;
    } else if (prim == 0x10 || prim == 0x802) {
        for (uint32_t i = 0; i + 1 < count; i += topo == 1 ? 1 : 2) {
            p3d_line(&d, &vt[i], &vt[i + 1], false);
        }
        topo = 3;
    }
    switch (topo) {
    case 3:
        break;
    case 0:
        for (uint32_t i = 0; i + 2 < count; i += 3) {
            p3d_triangle(&d, &vt[i], &vt[i + 1], &vt[i + 2]);
        }
        break;
    case 1:
        for (uint32_t i = 0; i + 2 < count; i++) {
            if (i & 1) {
                p3d_triangle(&d, &vt[i + 1], &vt[i], &vt[i + 2]);
            } else {
                p3d_triangle(&d, &vt[i], &vt[i + 1], &vt[i + 2]);
            }
        }
        break;
    case 2:
        for (uint32_t i = 1; i + 1 < count; i++) {
            p3d_triangle(&d, &vt[0], &vt[i], &vt[i + 1]);
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "dmp-smaph: topology %u\n", topo);
        break;
    }
    trace_dmp_smaph_job("p3d-draw", d.t.pa, count);
    p3d_target_close(&d.t);
}

static void p3d_write(SmaphState *s, unsigned reg, uint32_t v, unsigned mask)
{
    DmpShaderProg *pr = s->prog;

    if (reg >= P3D_NREG) {
        return;
    }
    if (mask != 0xf) {
        uint32_t m = 0;
        for (int b = 0; b < 4; b++) {
            m |= mask & (1 << b) ? 0xffu << (8 * b) : 0;
        }
        v = (s->p3d[reg] & ~m) | (v & m);
    }
    s->p3d[reg] = v;
    switch (reg) {
    case 0x202:                         /* draw kick (sub_5FD990) */
        p3d_draw(s, v);
        break;
    case 0x450:                         /* float uniform index */
        s->unif_idx = v;
        break;
    case 0x454:                         /* uniform x,y,z,w = 0x451..0x454 */
        if (s->unif_idx < DMP_SH_NUM_FUNIF) {
            for (int c = 0; c < 4; c++) {
                pr->funif[s->unif_idx].v[c] = p3d_f(s->p3d[0x451 + c]);
            }
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "dmp-smaph: uniform %u\n",
                          s->unif_idx);
        }
        s->unif_idx++;                  /* hypothesis: auto-increment */
        break;
    case 0x458:                         /* program load address (instr.) */
        s->code_idx = v * 2;
        break;
    case 0x459:                         /* program word */
        if (s->code_idx < DMP_SH_MAX_CODE) {
            pr->code[s->code_idx++] = v;
            pr->ncode = MAX(pr->ncode, s->code_idx);
        }
        break;
    case 0x460:                         /* operand descriptor index */
        s->opd_idx = v;
        break;
    case 0x461:
        if (s->opd_idx < DMP_SH_MAX_OPDESC) {
            pr->opdesc[s->opd_idx++] = v;
            pr->nopdesc = MAX(pr->nopdesc, s->opd_idx);
        }
        break;
    }
}

static uint64_t smaph_read(void *opaque, hwaddr off, unsigned size)
{
    SmaphState *s = opaque;
    if (off == 0x34) {
        return s->status;
    }
    return s->reg[off / 4];
}

static void smaph_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    SmaphState *s = opaque;
    s->reg[off / 4] = v;
    switch (off) {
    case 0x1C:
        if (v & 1) {
            psc_fill(s);
            s->status |= ST_PSC0;
        } else if (v == 0) {
            s->status &= ~ST_PSC0;
        }
        break;
    case 0xC18:
        if (v) {
            ppf_run(s);
            s->status |= ST_PPF;
        } else {
            s->status &= ~ST_PPF;
        }
        break;
    case 0x1800:
        trace_dmp_smaph_job("p3d-list", s->reg[0x1804 / 4], v);
        p3d_run_list(s, s->reg[0x1804 / 4], v, 0);
        s->status |= ST_P3D;
        break;
    case 0x1000:
        s->status &= ~ST_P3D;
        break;
    case 0x3C:
    case 0x44:
        s->status &= ~0xFu;
        break;
    }
    smaph_update(s);
}

static const MemoryRegionOps smaph_ops = {
    .read = smaph_read, .write = smaph_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void smaph_reset(DeviceState *dev)
{
    SmaphState *s = DMP_SMAPH(dev);
    memset(s->reg, 0, sizeof(s->reg));
    memset(s->p3d, 0, sizeof(s->p3d));
    memset(s->prog, 0, sizeof(*s->prog));
    s->code_idx = s->opd_idx = s->unif_idx = 0;
    s->status = 0;
}

static void smaph_init(Object *obj)
{
    SmaphState *s = DMP_SMAPH(obj);
    memory_region_init_io(&s->mr, obj, &smaph_ops, s, "dmp-smaph", 0x4000);
    s->prog = g_new0(DmpShaderProg, 1);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mr);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void smaph_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), smaph_reset);
}

static const TypeInfo smaph_info = {
    .name = TYPE_DMP_SMAPH,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SmaphState),
    .instance_init = smaph_init,
    .class_init = smaph_class_init,
};

static void smaph_register(void)
{
    type_register_static(&smaph_info);
}
type_init(smaph_register)

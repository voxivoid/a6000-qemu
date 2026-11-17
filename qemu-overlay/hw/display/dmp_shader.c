/*
 * DMP SMAPH-S30 ("SUGILITE") programmable shader unit -- interpreter.
 *
 * One unified core runs both the vertex and the fragment program out of a
 * single instruction memory (P3D reg 0x458 = load address in instructions,
 * 0x459 = data port, two 32-bit words per instruction), a single operand-
 * descriptor table (0x460 index / 0x461 data, 128 entries) and one float
 * constant file (0x450 index / 0x451..0x454 x,y,z,w as IEEE f32).  VS entry
 * = reg 0x442, FS entry = reg 0x443 (the caller puts it in prog->entry).
 * Evidence, field table and what is hypothesis: docs/reference/RE_GPU_P3D_SHADER.md.
 *
 * Instruction (w0 = first word written to 0x459, w1 = second):
 *   w0[31:28] src1 index[3:0], w1[2:0] src1 index[6:4]
 *   w0[27:24] src1 select: bit1 = V file, bit0 = Y file, bit3 = +a0
 *   w1[7]     src1 from the constant file U (and: no Y copy of the result)
 *   w0[23:21] src2 file (0 X, 1 Y, 2 V, 4 U)   w0[20:12] src2 index
 *   w0[6:0]   operand descriptor index
 *   w1[27:20] opcode   w1[19] dst = output file O
 *   w1[11:8]  dst index   (w1[28], w1[5]: Y-copy control, see sh_write)
 *   opcode 0 = control: NOP (all zero), w1[15:12] 1 CALL, 2 RET, 3 JMP,
 *   4 END, 0xd marker; opcodes 0x24/0x3d = conditional branch to w0.
 * Operand descriptor:
 *   [29:26] write mask (bit 26 = x)   [25:21] src1 modifier, [20:13] src1
 *   swizzle, [12:8] src2 modifier, [7:0] src2 swizzle (2 bits/comp, x low)
 *   modifier: bit 4 negate; low 3 bits: 3 = ROM vector, 2 = w forced to 1
 *   (op 0x02: w dropped)
 *
 * Register files (see the RE doc for the evidence and the open points):
 *   V  input file (VS attributes / FS varyings), read-only
 *   O  output file (dst w1[19]); starts as a copy of V.  VS o0 = clip
 *      position; FS o0 = depth, o1 = colour.  In the FS an output write is
 *      also visible in X (GLES1 fragment programs read it back)
 *   X  general file, initialised from the constant file
 *   Y  second view of the general file (src1 select bit 0, src2 file 1)
 *   U  constant file, read-only
 *   ROM constant vector (0, 0.5, 1, 2)   hypothesis: y component unknown
 */

#ifndef DMP_SHADER_STANDALONE
#include "qemu/osdep.h"
#include "qemu/log.h"
#include <math.h>
#else
#include <math.h>
#include <stdio.h>
#include <string.h>
#define LOG_UNIMP 0
#define qemu_log_mask(m, ...) fprintf(stderr, __VA_ARGS__)
#endif
#include "hw/display/dmp_shader.h"

#define SH_NREG       DMP_SH_NUM_FUNIF
#define SH_NV         16
#define SH_MAX_STEPS  20000
#define SH_STACK      8

static const float sh_rom[4] = { 0.0f, 0.5f, 1.0f, 2.0f };

typedef struct ShCtx {
    DmpVec4 x[SH_NREG];
    DmpVec4 y[SH_NREG];
    DmpVec4 v[SH_NV];           /* inputs (attributes / varyings) */
    DmpVec4 o[SH_NV];           /* outputs */
    const DmpShaderProg *p;
    bool fs;
    int a0;                     /* address register (op 0x11) */
    bool cond[4];               /* comparison flags (op 0x1e) */
} ShCtx;

static bool sh_unimp(uint32_t a, uint32_t w0, uint32_t w1, const char *why)
{
    static uint32_t seen[32];
    static unsigned nseen;
    uint32_t key = (w1 >> 20) & 0xff ? w1 & 0x0ff00000u : w1 & 0xff000u;

    for (unsigned i = 0; i < nseen; i++) {
        if (seen[i] == key) {
            return false;
        }
    }
    if (nseen < 32) {
        seen[nseen++] = key;
    }
    qemu_log_mask(LOG_UNIMP, "dmp-shader: %s at %u: %08x %08x (op 0x%02x)\n",
                  why, a, w0, w1, (w1 >> 20) & 0xff);
    return false;
}

static void sh_swz(const float *src, unsigned swz, unsigned mod, float *o)
{
    const float *s = (mod & 7) == 3 ? sh_rom : src;

    for (int c = 0; c < 4; c++) {
        float f = s[(swz >> (2 * c)) & 3];
        o[c] = (mod & 0x10) ? -f : f;
    }
    if ((mod & 7) == 2) {
        o[3] = 1.0f;                        /* hypothesis: homogeneous */
    }
}

static const float *sh_src1(ShCtx *c, uint32_t w0, uint32_t w1)
{
    unsigned sel = (w0 >> 24) & 0xf;
    int idx = (w0 >> 28) | ((w1 & 7) << 4);

    if (sel & 8) {
        idx += c->a0;                       /* relative addressing */
    }
    idx &= SH_NREG - 1;
    if (sel & 2) {
        return c->v[idx & (SH_NV - 1)].v;
    }
    if (w1 & 0x80) {
        return c->p->funif[idx].v;
    }
    return (sel & 1) ? c->y[idx].v : c->x[idx].v;
}

static const float *sh_src2(ShCtx *c, uint32_t w0)
{
    unsigned idx = ((w0 >> 12) & 0x1ff) & (SH_NREG - 1);

    switch ((w0 >> 21) & 7) {
    case 1:
        return c->y[idx].v;
    case 2:
        return c->v[idx & (SH_NV - 1)].v;
    case 4:
        return c->p->funif[idx].v;
    default:
        return c->x[idx].v;
    }
}

static void sh_store(float *d, unsigned mask, const float *res)
{
    for (int k = 0; k < 4; k++) {
        if (mask & (1u << k)) {
            d[k] = res[k];
        }
    }
}

static void sh_write(ShCtx *c, uint32_t w1, unsigned mask, const float *res)
{
    unsigned idx = (w1 >> 8) & 0xf;

    if (w1 & 0x00080000u) {                 /* output file */
        sh_store(c->o[idx].v, mask, res);
        if (c->fs) {
            /* hypothesis: in the FS, V k is also visible as X k */
            sh_store(c->x[idx].v, mask, res);
        }
        return;
    }
    sh_store(c->x[idx].v, mask, res);
    /*
     * hypothesis (fitted to the libObj programs, doc §6.3): w1 bit 7, or
     * w1 bit 28 without w1 bit 5, writes X only; other writes update Y too.
     */
    if (!(w1 & 0x80) && (!(w1 & 0x10000000u) || (w1 & 0x20))) {
        sh_store(c->y[idx].v, mask, res);
    }
}

/*
 * comparison src1 OP src2.  hypothesis: 3 = GT and 4 = EQ are fitted to the
 * GLES1 package flag tests (doc §7); 0/1/2/5 are guesses.
 */
static bool sh_cmp(unsigned mode, float a, float b, bool *ok)
{
    *ok = true;
    switch (mode) {
    case 0: return a != b;
    case 1: return a < b;
    case 2: return a <= b;
    case 3: return a > b;
    case 4: return a == b;
    case 5: return a >= b;
    }
    *ok = false;
    return false;
}

/*
 * branch condition w1[19:12]: 2 bits per component, x in the top pair.
 * hypothesis: bit 1 of a pair = ignore this component, bit 0 = required
 * flag value; all non-ignored components must match (AND).
 */
static bool sh_cond(ShCtx *c, uint32_t w1)
{
    bool r = true;

    for (int k = 0; k < 4; k++) {
        unsigned f = (w1 >> (18 - 2 * k)) & 3;
        if (!(f & 2) && c->cond[k] != (bool)(f & 1)) {
            r = false;
        }
    }
    return (w1 & 0x400) ? !r : r;   /* hypothesis: w1[10] inverts */
}

bool dmp_shader_run(DmpShaderStage stage, const DmpShaderProg *prog,
                    const DmpVec4 *in, unsigned nin,
                    DmpVec4 *out, unsigned nout)
{
    ShCtx c;
    unsigned pc = prog->entry, sp = 0, stack[SH_STACK];

    c.p = prog;
    c.fs = stage == DMP_SH_FRAGMENT;
    c.a0 = 0;
    memset(c.cond, 0, sizeof(c.cond));
    memcpy(c.x, prog->funif, sizeof(c.x));
    memcpy(c.y, prog->funif, sizeof(c.y));
    memset(c.v, 0, sizeof(c.v));
    for (unsigned i = 0; i < nin && i < SH_NV; i++) {
        c.v[i] = in[i];
        /* hypothesis: FS varying k also appears in X/Y k (k >= 1) */
        if (c.fs && i >= 1) {
            c.x[i] = in[i];
            c.y[i] = in[i];
        }
    }

    /* outputs start as a copy of the inputs (FS: o1.rgb = varying 1 when
     * only o1.w is written, gemini FS 0xe80f92) */
    memcpy(c.o, c.v, sizeof(c.o));

    for (unsigned step = 0; step < SH_MAX_STEPS; step++) {
        if (2 * pc + 1 >= DMP_SH_MAX_CODE) {
            return sh_unimp(pc, 0, 0, "pc out of range");
        }
        uint32_t w0 = prog->code[2 * pc], w1 = prog->code[2 * pc + 1];
        unsigned op = (w1 >> 20) & 0xff;
        unsigned next = pc + 1;

        if (op == 0) {
            switch ((w1 >> 12) & 0xff) {
            case 0x0:
                if (w0 == 0 && w1 == 0) {
                    break;                      /* NOP */
                }
                return sh_unimp(pc, w0, w1, "control op");
            case 0x1:                           /* CALL w0 */
                if (sp >= SH_STACK) {
                    return sh_unimp(pc, w0, w1, "call stack overflow");
                }
                stack[sp++] = next;
                next = w0;
                break;
            case 0x2:                           /* RET */
                if (!sp) {
                    goto done;
                }
                next = stack[--sp];
                break;
            case 0x3:                           /* JMP w0 */
                next = w0;
                break;
            case 0x4:                           /* END */
                goto done;
            case 0xd:                           /* block marker [?] */
                break;
            default:
                return sh_unimp(pc, w0, w1, "control op");
            }
            pc = next;
            continue;
        }
        if (op == 0x24 || op == 0x3d) {        /* conditional branch */
            pc = sh_cond(&c, w1) ? w0 : next;
            continue;
        }

        uint32_t d = prog->opdesc[w0 & 0x7f];
        unsigned mask = (d >> 26) & 0xf;
        float a[4], b[4], r[4];
        bool ok;

        sh_swz(sh_src1(&c, w0, w1), (d >> 13) & 0xff, (d >> 21) & 0x1f, a);
        sh_swz(sh_src2(&c, w0), d & 0xff, (d >> 8) & 0x1f, b);

        switch (op) {
        case 0x0c:                              /* MOV */
            memcpy(r, a, sizeof(r));
            break;
        case 0x01:                              /* ADD */
            for (int k = 0; k < 4; k++) {
                r[k] = a[k] + b[k];
            }
            break;
        case 0x03:                              /* MUL */
            for (int k = 0; k < 4; k++) {
                r[k] = a[k] * b[k];
            }
            break;
        case 0x02:                              /* horizontal sum (DP) */
            r[0] = a[0] + a[1] + a[2];
            if ((((d >> 21) & 7) != 2)) {
                r[0] += a[3];
            }
            r[1] = r[2] = r[3] = r[0];
            break;
        case 0x04:                              /* RCP, hypothesis */
            r[0] = r[1] = r[2] = r[3] = 1.0f / a[0];
            break;
        case 0x05:                              /* RSQ */
            r[0] = r[1] = r[2] = r[3] = 1.0f / sqrtf(a[0]);
            break;
        case 0x07:                              /* EX2, hypothesis */
            r[0] = r[1] = r[2] = r[3] = exp2f(a[0]);
            break;
        case 0x08:                              /* LG2, hypothesis */
            r[0] = r[1] = r[2] = r[3] = log2f(a[0]);
            break;
        case 0x11:                              /* MOVA: a0 = src1, hyp. */
            for (int k = 0; k < 4; k++) {
                if (mask & (1u << k)) {
                    c.a0 = (int)a[k];
                    break;
                }
            }
            pc = next;
            continue;
        case 0x1c:                              /* MAX */
            for (int k = 0; k < 4; k++) {
                r[k] = a[k] > b[k] ? a[k] : b[k];
            }
            break;
        case 0x1d:                              /* MIN */
            for (int k = 0; k < 4; k++) {
                r[k] = a[k] < b[k] ? a[k] : b[k];
            }
            break;
        case 0x1e:                              /* CMP -> cond[] */
            for (int k = 0; k < 4; k++) {
                c.cond[k] = sh_cmp((w1 >> 16) & 7, a[k], b[k], &ok);
                if (!ok) {
                    return sh_unimp(pc, w0, w1, "compare mode");
                }
            }
            pc = next;
            continue;
        case 0x20:                              /* TEX */
            if (prog->tex_sample) {
                prog->tex_sample(prog->tex_opaque, (w1 >> 14) & 7, a, r);
            } else {
                r[0] = r[1] = r[2] = r[3] = 0.0f;
            }
            break;
        case 0x2b:                              /* colour out + END, hyp. */
            sh_store(c.o[1].v, mask, a);
            goto done;
        default:
            return sh_unimp(pc, w0, w1, "opcode");
        }
        sh_write(&c, w1, mask, r);
#ifdef DMP_SHADER_TRACE
        fprintf(stderr, "  %4u %08x %08x op%02x m%x a(%g %g %g %g) "
                "b(%g %g %g %g) -> %c%u (%g %g %g %g)\n", pc, w0, w1, op,
                mask, a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3],
                (w1 & 0x80000u) ? 'V' : 'X', (w1 >> 8) & 0xf,
                r[0], r[1], r[2], r[3]);
#endif
        pc = next;
    }
    return sh_unimp(pc, 0, 0, "step limit");
done:
    for (unsigned i = 0; i < nout && i < DMP_SH_NUM_OUTPUT && i < SH_NV; i++) {
        out[i] = c.o[i];
    }
    return true;
}

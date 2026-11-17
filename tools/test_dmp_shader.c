/*
 * Host-side test for hw/display/dmp_shader.c (DMP SMAPH-S30 shader ISA).
 *
 * Loads the real program binaries (glProgramBinaryOES format 0x6000,
 * magic 0xBEAF) embedded in the stock libObj.so, places them in the shader
 * memory exactly like libGLESv2_dmp.so pg_deploy_program_binary does, sets
 * uniforms/inputs and checks the outputs against what the GLSL source (for
 * the "gemini" programs, source text is in libObj.so too) or the uniform
 * names imply.  The firmware file is only read as data.
 *
 * Build (from the repository root):
 *   cc -O1 -Wall -DDMP_SHADER_STANDALONE -Iemulator/qemu/include \
 *      -o /tmp/test_dmp_shader emulator/tools/test_dmp_shader.c \
 *      emulator/qemu/hw/display/dmp_shader.c -lm
 *   /tmp/test_dmp_shader rootfs_usr/lib/libObj.so \
 *     firmware/unpacked/firmware.tar_unpacked/0700_part_image/dev/\
 *     nflasha16_unpacked_unpacked/lib/egl/libGLESv1_CM_dmp.so
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hw/display/dmp_shader.h"

static unsigned char *img;
static size_t img_len;
static int nfail, npass;

/* ---- program-binary reader (layout: docs/reference/RE_GPU_P3D_SHADER.md §3) ---- */

typedef struct Rd { const unsigned char *b; size_t o; } Rd;
static unsigned u8(Rd *r) { return r->b[r->o++]; }
static unsigned u32(Rd *r)
{
    unsigned v = r->b[r->o] | r->b[r->o + 1] << 8 | r->b[r->o + 2] << 16 |
                 (unsigned)r->b[r->o + 3] << 24;
    r->o += 4;
    return v;
}

typedef struct Uni { char name[32]; unsigned type, n, addr, comp; } Uni;
typedef struct Bin {
    unsigned vs_entry, fs_entry;   /* fs_entry already past any prefix */
    Uni u[16];
    unsigned nu;
} Bin;

static void rd_syms(const unsigned char *b, unsigned off, Bin *bi)
{
    Rd r = { b, off };
    u32(&r);
    unsigned n = u32(&r);
    for (unsigned k = 0; k < n && bi->nu < 16; k++) {
        unsigned ln = u32(&r);
        Uni *u = &bi->u[bi->nu++];
        snprintf(u->name, sizeof(u->name), "%.*s", (int)ln, b + r.o);
        r.o += ln + 1;
        u->type = u32(&r);
        u->n = u32(&r);
        u->addr = u32(&r);
        u->comp = u32(&r);
    }
}

static void put_code(DmpShaderProg *p, unsigned at, Rd *r, unsigned n)
{
    for (unsigned k = 0; k < 2 * n; k++) {
        p->code[2 * at + k] = u32(r);
    }
}

/* Deploy the binary at file offset off into p; returns 0 on success. */
static int load_bin(size_t off, DmpShaderProg *p, Bin *bi)
{
    const unsigned char *b = img + off;
    Rd r = { b, 0 };

    memset(p, 0, sizeof(*p));
    memset(bi, 0, sizeof(*bi));
    if (b[0] != 0xaf || b[1] != 0xbe) {
        return -1;
    }
    r.o = 2;
    if (u32(&r) != 0x6000) {
        return -1;
    }
    unsigned type = u8(&r);
    r.o += 2;
    if (type == 1) {                       /* 2D-API program, no prefix */
        unsigned off6[6];
        for (int k = 0; k < 6; k++) {
            off6[k] = u32(&r);
        }
        Rd s = { b, off6[0] };
        u32(&s);
        bi->vs_entry = u32(&s) + 56;
        u32(&s); u32(&s); u32(&s);
        bi->fs_entry = u32(&s) + 56;
        for (int k = 0; k < 6; k++) {
            u32(&s);
        }
        unsigned n = u32(&s);
        put_code(p, 56, &s, n);
        Rd w = { b, off6[1] };
        u32(&w);
        n = u32(&w);
        for (unsigned k = 0; k < n; k++) {
            unsigned a = u32(&w);
            p->opdesc[a & 127] = u32(&w);
        }
        rd_syms(b, off6[3], bi);
        return 0;
    }
    if (type != 0) {
        return -1;
    }
    unsigned off4[4];
    for (int k = 0; k < 4; k++) {
        off4[k] = u32(&r);
    }
    Rd s = { b, off4[0] };
    u32(&s);
    u8(&s);                                /* flags */
    for (int sh = 0; sh < 2; sh++) {
        u8(&s); u8(&s); u8(&s);            /* inputs, outputs, ctx */
        unsigned cofs = u32(&s), n = u32(&s);
        unsigned at = cofs + 56 + (sh ? 9 : 0);  /* FS: 9-instr prefix */
        put_code(p, at, &s, n);
        if (sh == 0) {
            bi->vs_entry = cofs + 56;
        } else {
            bi->fs_entry = at;   /* driver skips the prefix by default */
        }
        for (int bank = 0; bank < 2; bank++) {
            unsigned o = u32(&s), m = u32(&s);
            for (unsigned k = 0; k < m; k++) {
                p->opdesc[(o + k + 64 * bank) & 127] = u32(&s);
            }
        }
        unsigned ko = u32(&s), nk = u32(&s);
        for (unsigned k = 0; k < nk; k++) {
            for (int c = 0; c < 4; c++) {
                unsigned v = u32(&s);
                memcpy(&p->funif[ko + k].v[c], &v, 4);
            }
        }
    }
    rd_syms(b, off4[1], bi);
    return 0;
}

static Uni *uni(Bin *bi, const char *name)
{
    for (unsigned k = 0; k < bi->nu; k++) {
        if (!strcmp(bi->u[k].name, name)) {
            return &bi->u[k];
        }
    }
    fprintf(stderr, "uniform %s not found\n", name);
    exit(2);
}

/* set n components starting at the uniform's (addr, comp) */
static void set_u(DmpShaderProg *p, Bin *bi, const char *name, unsigned elem,
                  const float *v, unsigned n)
{
    Uni *u = uni(bi, name);
    for (unsigned c = 0; c < n; c++) {
        p->funif[u->addr + elem].v[u->comp + c] = v[c];
    }
}

/* ---- checking ---- */

static int near(float a, float b) { return fabsf(a - b) < 1e-4f; }

static void check(const char *what, const float *got, const float *exp,
                  unsigned mask)
{
    int ok = 1;
    for (int c = 0; c < 4; c++) {
        if ((mask >> c & 1) && !near(got[c], exp[c])) {
            ok = 0;
        }
    }
    printf("%s %-44s got (%g %g %g %g) want (%g %g %g %g)%s\n",
           ok ? "PASS" : "FAIL", what, got[0], got[1], got[2], got[3],
           exp[0], exp[1], exp[2], exp[3], mask == 0xf ? "" : " [masked]");
    ok ? npass++ : nfail++;
}

static unsigned ntex;           /* fetch callbacks since the last run() */

static void fake_tex(void *opaque, unsigned unit, const float *t, float *o)
{
    (void)opaque;
    ntex++;
    o[0] = t[0];
    o[1] = t[1];
    o[2] = 0.25f + unit;
    o[3] = 0.75f;
}

static DmpShaderProg P;
static Bin B;

static int run(DmpShaderStage st, unsigned entry, const DmpVec4 *in,
               unsigned nin, DmpVec4 *out)
{
    P.entry = entry;
    P.tex_sample = fake_tex;
    ntex = 0;
    memset(out, 0, sizeof(DmpVec4) * DMP_SH_NUM_OUTPUT);
    if (!dmp_shader_run(st, &P, in, nin, out, DMP_SH_NUM_OUTPUT)) {
        printf("FAIL run returned false (entry %u)\n", entry);
        nfail++;
        return 0;
    }
    return 1;
}

#define V4(a, b, c, d) ((DmpVec4){ { a, b, c, d } })

static void need(size_t off)
{
    if (off + 16 > img_len || load_bin(off, &P, &B)) {
        fprintf(stderr, "no program binary at 0x%zx\n", off);
        exit(2);
    }
}

/* gemini "inPosition/mtxL2W" VS + selectable FS (libObj 0xe7ed54...) */
static void t_gemini(void)
{
    DmpVec4 in[16] = { 0 }, o[16], f[16];
    static const float iss[2] = { 1.0f / 640, 1.0f / 480 };

    need(0xe7ed54);
    set_u(&P, &B, "invScreenSize", 0, iss, 2);
    in[0] = V4(0.1f, 0.2f, 0.3f, 0.4f);           /* inColor */
    in[1] = V4(100, 50, 1, 1);                    /* inPosition (vec3) */
    in[2] = V4(0.25f, 0.75f, 0, 1);               /* inUV */
    in[3] = V4(1, 0, 20, 1);                      /* mtxL2W_0 */
    in[4] = V4(0, 1, 10, 1);                      /* mtxL2W_1 */
    if (run(DMP_SH_VERTEX, B.vs_entry, in, 5, o)) {
        float x = 120.0f, y = 60.0f;
        check("gemini VS gl_Position", o[0].v,
              (float[]){ x * 2 / 640 - 1, -(y * 2 / 480 - 1), 0, 1 }, 0xf);
        check("gemini VS varColor", o[1].v, in[0].v, 0xf);
        check("gemini VS varUV", o[2].v, in[2].v, 0x3);
    }

    /* YUV->RGB FS of the same binary: yuv2rgb[12] at R8..R19 .w */
    static const float m[12] = { 1, 0, 1.402f, -0.701f,
                                 1, -0.344f, -0.714f, 0.529f,
                                 1, 1.772f, 0, -0.886f };
    for (int k = 0; k < 12; k++) {
        set_u(&P, &B, "yuv2rgb", k, &m[k], 1);
    }
    f[0] = V4(0, 0, 0.5f, 1);
    f[1] = in[0];
    f[2] = V4(0.6f, 0.3f, 0, 1);       /* fake_tex returns (s, t, .25, .75) */
    if (run(DMP_SH_FRAGMENT, B.fs_entry, f, 3, o)) {
        /*
         * texel = (s, t, .25, .75) = (0.6, 0.3, 0.25).  Each matrix column
         * multiplies one texel component: col0 * t.y + col1 * t.z +
         * col2 * t.x + col3 -- with BT.601 numbers that is (Cr, Y, Cb)
         * texel order.  Alpha = 1 (ROM).
         */
        float t[3] = { 0.6f, 0.3f, 0.25f }, e[4] = { 0, 0, 0, 1 };
        for (int r = 0; r < 3; r++) {
            e[r] = m[4 * r] * t[1] + m[4 * r + 1] * t[2] +
                   m[4 * r + 2] * t[0] + m[4 * r + 3];
        }
        check("gemini YUV->RGB FS", o[1].v, e, 0xf);
    }

    struct { size_t off; const char *what; int kind; } fs[] = {
        { 0xe7f8f7, "gemini FS varColor", 0 },
        { 0xe80352, "gemini FS texture2D", 1 },
        { 0xe80a08, "gemini FS tex*varColor", 2 },
        { 0xe80ccd, "gemini FS tex+varColor", 3 },
        { 0xe80f92, "gemini FS rgb=col, a=tex.a*col.a", 4 },
    };
    for (unsigned k = 0; k < sizeof(fs) / sizeof(fs[0]); k++) {
        need(fs[k].off);
        f[0] = V4(0, 0, 0.5f, 1);
        f[1] = V4(0.1f, 0.2f, 0.3f, 0.4f);
        f[2] = V4(0.6f, 0.3f, 0, 1);
        float tx[4] = { 0.6f, 0.3f, 0.25f, 0.75f }, e[4];
        for (int c = 0; c < 4; c++) {
            float col = f[1].v[c];
            e[c] = fs[k].kind == 0 ? col : fs[k].kind == 1 ? tx[c] :
                   fs[k].kind == 2 ? tx[c] * col :
                   fs[k].kind == 3 ? tx[c] + col :
                   c < 3 ? col : tx[3] * col;
        }
        if (run(DMP_SH_FRAGMENT, B.fs_entry, f, 3, o)) {
            check(fs[k].what, o[1].v, e, 0xf);
        }
    }
}

/* simple GLES2 programs with source in libObj (0xe813c4 / 0xe814c4) */
static void t_simple(void)
{
    DmpVec4 in[16] = { 0 }, o[16];
    static const float fc[4] = { 0.5f, 0.25f, 1, 0.8f };

    need(0xe813c4);                 /* gl_Position = a_position; = filterColor */
    set_u(&P, &B, "filterColor", 0, fc, 4);
    in[0] = V4(0.3f, -0.4f, 0.1f, 1);
    if (run(DMP_SH_VERTEX, B.vs_entry, in, 1, o)) {
        check("plain VS gl_Position", o[0].v, in[0].v, 0xf);
    }
    if (run(DMP_SH_FRAGMENT, B.fs_entry, in, 1, o)) {
        check("plain FS = filterColor", o[1].v, fc, 0xf);
    }

    need(0xe814c4);                 /* + v_texCoord, tex * filterColor */
    set_u(&P, &B, "filterColor", 0, fc, 4);
    in[1] = V4(0.125f, 0.625f, 0, 1);
    if (run(DMP_SH_VERTEX, B.vs_entry, in, 2, o)) {
        check("tex VS gl_Position", o[0].v, in[0].v, 0xf);
        check("tex VS v_texCoord", o[1].v, in[1].v, 0x3);
        DmpVec4 f[16] = { o[0], o[1] };
        DmpVec4 r[16];
        if (run(DMP_SH_FRAGMENT, B.fs_entry, f, 2, r)) {
            float e[4] = { 0.125f * fc[0], 0.625f * fc[1], 0.25f * fc[2],
                           0.75f * fc[3] };
            check("tex FS = texture2D*filterColor", r[1].v, e, 0xf);
            /* contract in dmp_shader.h: one tex_sample call per fetch
             * (the rasteriser's mip LOD pairs fetches by call order) */
            float nt[4] = { ntex, 0, 0, 0 }, one[4] = { 1, 0, 0, 0 };
            check("tex FS: one tex_sample call per fetch", nt, one, 1);
        }
    }
}

/* 2D renderer programs (gemini-ogl/src/2DRenderer.cpp, type-1 binaries) */
static void t_2d(void)
{
    DmpVec4 in[16] = { 0 }, o[16];
    static const float col[4] = { 0.9f, 0.1f, 0.2f, 0.5f };

    need(0xbcda74);                 /* a_position + u_color */
    set_u(&P, &B, "u_color", 0, col, 4);
    in[0] = V4(-0.5f, 0.5f, 0, 1);
    if (run(DMP_SH_VERTEX, B.vs_entry, in, 1, o)) {
        check("2D plain VS position", o[0].v, in[0].v, 0xf);
    }
    if (run(DMP_SH_FRAGMENT, B.fs_entry, o, 1, o)) {
        check("2D plain FS = u_color", o[1].v, col, 0xf);
    }

    /* u_epMatrix (vec2[3]): pos' = ep0*x + ep1*y + ep2 */
    static const float ep[3][2] = { { 0.5f, 0 }, { 0, -0.25f }, { 0.1f, 0.2f } };
    need(0xbcd560);
    for (int k = 0; k < 3; k++) {
        set_u(&P, &B, "u_epMatrix", k, ep[k], 2);
    }
    set_u(&P, &B, "u_color", 0, col, 4);
    in[0] = V4(1, 1, 0, 1);
    if (run(DMP_SH_VERTEX, B.vs_entry, in, 1, o)) {
        check("2D epMatrix VS position (x, y, 0, 1)", o[0].v,
              (float[]){ 0.5f + 0.1f, -0.25f + 0.2f, 0, 1 }, 0xf);
    }
    if (run(DMP_SH_FRAGMENT, B.fs_entry, o, 1, o)) {
        check("2D epMatrix FS = u_color", o[1].v, col, 0xf);
    }

    need(0xbcd6a4);                 /* textured; u_texMatrix vec2[2] */
    static const float tm[2][2] = { { 2, 3 }, { 0.5f, 0.25f } };
    for (int k = 0; k < 3; k++) {
        set_u(&P, &B, "u_epMatrix", k, ep[k], 2);
    }
    for (int k = 0; k < 2; k++) {
        set_u(&P, &B, "u_texMatrix", k, tm[k], 2);
    }
    in[0] = V4(1, 1, 0, 1);
    in[1] = V4(0.25f, 0.5f, 0, 1);
    if (run(DMP_SH_VERTEX, B.vs_entry, in, 2, o)) {
        check("2D tex VS position.xy", o[0].v,
              (float[]){ 0.6f, -0.05f, 0, 1 }, 0x3);
        /* u_texMatrix[0] = scale, [1] = offset (componentwise) */
        check("2D tex VS texcoord*tm[0]+tm[1]", o[1].v,
              (float[]){ 0.25f * 2 + 0.5f, 0.5f * 3 + 0.25f, 0, 0 }, 0x3);
        DmpVec4 f[16] = { o[0], o[1] }, r[16];
        if (run(DMP_SH_FRAGMENT, B.fs_entry, f, 2, r)) {
            check("2D tex FS = texture2D(texcoord)", r[1].v,
                  (float[]){ o[1].v[0], o[1].v[1], 0.25f, 0.75f }, 0xf);
        }
    }
}


/*
 * GLES1 fixed-function vertex program: the 8496-byte package_bin_u
 * (libGLESv1_CM_dmp.so 0x3b810, "GPSD" v1.0) loaded at instruction 0, as the
 * stock compositor (Rigel, GLES1 context) uses it: P3D 0x442 = 0, 0x443 =
 * 0x1c8.  Uniform values below are the non-zero ones that influence this
 * path, taken from the emulator's command list of the composite draw.
 */
static void t_gles1(const char *path)
{
    FILE *fp = fopen(path, "rb");
    unsigned char *b = NULL;
    long n = 0;

    if (fp) {
        fseek(fp, 0, SEEK_END);
        n = ftell(fp);
        b = malloc(n);
        fseek(fp, 0, SEEK_SET);
        if (fread(b, 1, n, fp) != (size_t)n) {
            n = 0;
        }
        fclose(fp);
    }
    if (n < 0x3b810 + 8496 || memcmp(b + 0x3b810, "GPSD", 4)) {
        printf("FAIL no GLES1 package in %s\n", path);
        nfail++;
        return;
    }
    Rd r = { b + 0x3b810, 12 };
    unsigned ncode = u32(&r), nswz = u32(&r);
    u32(&r);
    memset(&P, 0, sizeof(P));
    for (unsigned k = 0; k < ncode; k++) {
        P.code[k] = u32(&r);
    }
    for (unsigned k = 0; k < nswz / 2; k++) {
        unsigned a = u32(&r);
        P.opdesc[a & 127] = u32(&r);
    }
    /* fragment program the GLES1 driver generated for TEXTURE_2D/REPLACE
     * (observed at 0x1c8 in the guest command list) */
    static const unsigned fs[26] = {
        0x10000000, 0x00c00040, 0, 0, 0, 0x00c80400, 0, 0,
        0x20000041, 0x02000540, 0x50000000, 0x20c00040, 0, 0x00c80400, 0, 0,
        0x40000000, 0x00c00040, 0, 0, 0, 0x00c80100, 0, 0, 0, 0x00004000 };
    memcpy(&P.code[2 * 456], fs, sizeof(fs));
    static const struct { unsigned i; float v[4]; } u[] = {
        { 0, { 1.0f / 320, 0, 0, -1 } }, { 1, { 0, -1.0f / 240, 0, 1 } },
        { 2, { 0, 0, -2, -1 } },          { 3, { 0, 0, 0, 1 } },
        { 19, { 1, 0, 0, 0 } },           { 25, { 0.2f, 0.2f, 0.2f, 1 } },
        { 109, { -1, 0, 0, 0 } },
    };
    for (unsigned k = 0; k < sizeof(u) / sizeof(u[0]); k++) {
        memcpy(P.funif[u[k].i].v, u[k].v, sizeof(u[k].v));
    }
    DmpVec4 in[16] = { 0 }, o[16], f[16];
    in[0] = V4(100, 50, 0, 1);              /* position (window pixels) */
    in[1] = V4(0.9f, 0.8f, 0.7f, 0.6f);     /* colour */
    in[2] = V4(0.25f, 0.75f, 0, 1);         /* texcoord 0 */
    if (run(DMP_SH_VERTEX, 0, in, 16, o)) {
        check("GLES1 VS position = MVP*pos", o[0].v,
              (float[]){ 100.0f / 320 - 1, 1 - 50.0f / 240, -1, 1 }, 0xf);
        check("GLES1 VS colour", o[1].v, in[1].v, 0xf);
        check("GLES1 VS texcoord0", o[2].v, in[2].v, 0x3);
        if (run(DMP_SH_FRAGMENT, 456, o, 16, f)) {
            check("GLES1 FS REPLACE = texture", f[1].v,
                  (float[]){ 0.25f, 0.75f, 0.25f, 0.75f }, 0xf);
        }
    }
    free(b);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s rootfs_usr/lib/libObj.so\n", argv[0]);
        return 2;
    }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) {
        perror(argv[1]);
        return 2;
    }
    fseek(fp, 0, SEEK_END);
    img_len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    img = malloc(img_len);
    if (fread(img, 1, img_len, fp) != img_len) {
        return 2;
    }
    fclose(fp);
    t_simple();
    t_gemini();
    t_2d();
    if (argc > 2) {
        t_gles1(argv[2]);
    }
    printf("%d passed, %d failed\n", npass, nfail);
    return nfail != 0;
}

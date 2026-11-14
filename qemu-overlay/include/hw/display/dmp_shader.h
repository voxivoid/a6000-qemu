/*
 * DMP SMAPH-S30 programmable shader units — interpreter interface (a5000).
 *
 * Contract between the P3D command-list decoder / rasteriser
 * (hw/display/dmp_smaph.c) and the shader interpreter
 * (hw/display/dmp_shader.c).  The command-list side collects what the
 * guest uploaded through P3D registers (program words, operand
 * descriptors, uniforms, attribute layout) into DmpShaderProg and calls
 * the interpreter per vertex / per fragment.  Field meanings are filled in
 * from docs/RE_GPU_P3D_SHADER.md; extend the structs as needed, but keep
 * both sides building.
 */
#ifndef HW_DISPLAY_DMP_SHADER_H
#define HW_DISPLAY_DMP_SHADER_H

#include <stdint.h>
#include <stdbool.h>

#define DMP_SH_MAX_CODE     4096    /* program words */
#define DMP_SH_MAX_OPDESC   128     /* operand descriptors */
#define DMP_SH_NUM_FUNIF    256     /* float4 uniforms: P3D reg 0x450 index
                                       reaches 0xC9 in the stock UI lists */
#define DMP_SH_NUM_IUNIF    4       /* int4 uniforms */
#define DMP_SH_NUM_INPUT    16
#define DMP_SH_NUM_OUTPUT   16

typedef struct DmpVec4 {
    float v[4];
} DmpVec4;

typedef struct DmpShaderProg {
    uint32_t code[DMP_SH_MAX_CODE];
    uint32_t ncode;
    uint32_t opdesc[DMP_SH_MAX_OPDESC];
    uint32_t nopdesc;
    uint32_t entry;                 /* first instruction index */
    DmpVec4 funif[DMP_SH_NUM_FUNIF];
    uint32_t iunif[DMP_SH_NUM_IUNIF][4];
    uint32_t bunif;                 /* bool uniforms, bit n */
    uint32_t in_map[DMP_SH_NUM_INPUT];   /* attribute -> input register */
    uint32_t out_map[DMP_SH_NUM_OUTPUT]; /* output register -> semantic */
    uint32_t extra[64];             /* unit-specific state, see the RE doc */
    /*
     * Texture fetch, provided by the rasteriser (dmp_smaph.c).  unit is the
     * texture unit / sampler number as encoded in the instruction, coord
     * the (s, t, r, q) operand; rgba receives the filtered texel as floats
     * in [0, 1].  NULL when no texture state is available.
     */
    void (*tex_sample)(void *opaque, unsigned unit, const float coord[4],
                       float rgba[4]);
    void *tex_opaque;
} DmpShaderProg;

typedef enum DmpShaderStage {
    DMP_SH_VERTEX = 0,
    DMP_SH_FRAGMENT = 1,
} DmpShaderStage;

/*
 * Run one invocation.  in[] are the input registers (vertex attributes or
 * interpolated varyings), out[] receive the output registers.  What the
 * interpreter uses from prog (docs/RE_GPU_P3D_SHADER.md §1):
 *   code[]   words written through P3D 0x459, indexed from address 0
 *            (0x458 = instruction address, i.e. word index / 2)
 *   opdesc[] 0x461 data at index 0x460 (0..127)
 *   funif[]  0x451..0x454 (raw IEEE f32) at index 0x450
 *   entry    VS: P3D 0x442, FS: P3D 0x443
 *   tex_sample/tex_opaque for opcode 0x20
 * Conventions: VS out[0] = clip position; FS in[k] = VS out[k]; FS
 * out[1] = colour (out[0] = depth from the 9-instruction prologue).
 * Not used: ncode, nopdesc, iunif, bunif, in_map, out_map, extra.  Returns
 * false on an undecodable instruction (the caller then skips the
 * primitive and logs once under LOG_UNIMP).
 */
bool dmp_shader_run(DmpShaderStage stage, const DmpShaderProg *prog,
                    const DmpVec4 *in, unsigned nin,
                    DmpVec4 *out, unsigned nout);

#endif

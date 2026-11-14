# RE: DMP SMAPH-S30 shader unit (instruction encoding · register files · upload)

Target: the programmable shader of the a5000 GPU (DMP SMAPH-S30, driver name SUGILITE). Implementation is
`qemu/hw/display/dmp_shader.c`, interface `qemu/include/hw/display/dmp_shader.h`,
host test `tools/test_dmp_shader.c`. For the command list format and P3D registers in general, see
`docs/RE_GPU_P3D_CMD.md`.

Notation: **[V]** = ran a real firmware program through the interpreter and confirmed the result matches what the GLSL source means.
**[S]** = confirmed from driver code (static analysis). **[?]** = hypothesis (stated with its evidence).

---------------------------------------------------------------------------------------------------
## 0. Summary
- The device has no GLSL compiler. `dmp_glslc_*` in `libGLESv2_dmp.so` is only a stub that dlopens `libDMPGlslc.so`,
  and that library is nowhere in the firmware [S]. So the stock UI uses **only precompiled binaries**.
  - GLES2: `glProgramBinaryOES(format 0x6000)`. libObj.so contains 19 (6 for the 2D renderer, 13 for gemini).
  - GLES1: shader package `package_bin_u` emulating fixed function (libGLESv1_CM_dmp.so 0x3b810, 8496 bytes).
    Rigel compositing (GLES1 context) loads this vertex program at address 0.
- **One unified core** runs both vertex and fragment programs. There is one instruction memory, one operand descriptor table,
  and one float constant file [S].
- Instructions are 64-bit (two 32-bit words). A **different ISA** from PICA200 (32-bit instructions). Common points are roughly the 7-bit
  descriptor index, mask/swizzle descriptors, compare flags + conditional branches, and an address register.
- All programs with available source compute as expected [V]. Expected values come from the GLSL source, or from uniform names when there is no source.
  This covers 7 gemini vertex/fragment programs, 4 2D renderer programs, and the compositing path of the GLES1 package.
  Opcodes and bits outside the verified scope are left as [?] (§7, §9).

## 1. Upload path (P3D registers)
Evidence: `libGLESv2_dmp.so` `pg_link_specify_program` (0x20430), `pg_deploy_program_binary` (0x2144c),
`store_data` (0x203e4), `cnv_update_prg_register` (0xf2e0), `__exe_store_const_reg` (0xc120), and GLES1
`__gl_shader_binary` (0x1e094). Register numbers are per command header `reg<<16 | 0xF000`.

| reg | Meaning | Evidence |
|---|---|---|
| 0x458 | code load address (in instructions) | `store_data(cmb,0,0x458, code_ofs+56)` [S] |
| 0x459 | code data. +1 word per write. One instruction is written twice, `w0` then `w1` | loop in the same function [S] |
| 0x460 | operand descriptor address (0..127) | lo bank = `swiz_ofs`, hi bank = `swiz_ofs + 64` [S] |
| 0x461 | descriptor data (address auto-increment) | [S] |
| 0x450 | float constant index | [S] |
| 0x451–0x454 | x, y, z, w. **Raw IEEE f32** (no PICA-style f24 packing) | `__exe_store_const_reg`: header `0x0450F026` + `addr` + 16 bytes `reg->data` [S] |
| 0x440 | `0x000F0004` or `0x000F0F04` [?] | `cnv_update_prg_register`, glClear list [S] |
| 0x441 | 0 or 0xFFFFFF00 [?] | same |
| 0x442 | **vertex program entry point** (instruction index) | glClear list `0x442=0x38` + measured [S] |
| 0x443 | **fragment entry point**. With default depth range and no polygon offset, `+9` skips the depth prologue | `cnv_update_prg_register` (`v19 = 9`) [S] |
| 0x445, 0x470–0x477 | usually 0. Integer/bool uniform candidates [?]. Not used by the tested programs | |

- **Instructions 0..55 are a reserved area**. GLES2 programs are loaded from `code_ofs + 56` [S].
  The GLES1 package occupies 947 instructions from address 0. In that case the GLES1 fragment program goes at 0x1C8 (456) (measured).
- The driver prepends a **9-instruction depth prologue** to GLES2 fragment programs.
  Constants `C.64.9960` / `C.127.10502` (18 words, descriptors 0x79–0x7F); the `gl_DepthRange` uniform
  goes in `c3` (`fischer_state.depth_range.addr = 3`) [S]. This prologue is not executed in the default state (+9).
- Constant file range: measured indices are 0..0xC9. GLES1 package constants are `c200 = (0,1,2,3)`, `c201 = (4,5,6,7)`,
  used as index tables [S]. The header has `DMP_SH_NUM_FUNIF = 256`.
- Bool uniforms are **converted by the driver to 0.0/1.0 floats and put in the constant file** (`pg_uniformfv`: for bool type
  `v != 0 → 1.0`) [S]. Feature branches in the GLES1 package are also decided by CMP on float constants (§8).

## 2. Program binary (`glProgramBinaryOES`, format 0x6000)
Per the parser in `pg_deploy_program_binary` [S]. All integers are LE, in an **unaligned** stream.

```
+0  u16 0xBEAF (byte-swapped 0xAFBE also accepted)  +2 u32 format = 0x6000  +6 u8 type  +7 u16 (ignored)
type 1 (2D API mode, pg->is_2d_api_mode = transpose_matrix = 1):
  +9 u32 off[6]  (each section starts with a u32 length)
  sec0: len, vs_entry(+56), vs_in, vs_out, vs_ctx, fs_entry(+56), fs_ctx, gs_entry, gs_out, gs_ctx,
        u32, u32, n, 2n code words (loaded at 0x458 = 56)
  sec1: len, n, n×(addr, desc)  → 0x460/0x461
  sec2: len, n, n×(addr, x, y, z, w) → 0x450..0x454
  sec3: uniform table / sec4: attribute table / sec5: link state
type 0 (plain GLES2):
  +9 u32 off[4];  sec0: len, u8 flags(bit0 one-hot, bit1 prefix-fs-out, bit2 frag_coord_z),
        2 shaders (VS, FS) × { u8 in, u8 out, u8 ctx, i32 code_ofs, u32 n, 2n words,
        i32 lo_ofs, u32 n, n descriptors, i32 hi_ofs(+64), u32 n, n descriptors,
        i32 const_ofs, u32 n, 4n words }
  sec1: uniform table, sec2: attribute table, sec3: link state
uniform entry: i32 namelen, name\0, u32 GL type, i32 array_size, i32 addr(constant register), i32 comp(start component)
```
- Uniforms are **packed per component**. E.g. `u_epMatrix` vec2[3] goes in c0..c2.xy, `u_texMatrix` vec2[2] in
  c0.zw/c1.zw, `u_minY` float in c2.w, `yuv2rgb` float[12] in .w of c8..c19.
- GLES1 package (`"GPSD"`, version 0x10000): word 2 = package id, 3 = code word count (1894), 4 = descriptor word count (214),
  5 = constant count, followed by code, (addr, desc) pairs, constants. Loaded at 0x458 = 0 [S].

## 3. Instruction encoding
`w0` = the word written to 0x459 first, `w1` = the second word.

| Field | Bits | Meaning | Status / evidence |
|---|---|---|---|
| desc | w0[6:0] | operand descriptor index (0..127) | [V] depth prologue uses 0x79..0x7F in order. Most instructions use distinct descs |
| src2 idx | w0[20:12] | src2 register number (9 bits) | [V] yuv2rgb c19 = `0x13`, GLES1 c104 / c201 (`0x0c9`) |
| src2 file | w0[23:21] | 0 = X, 1 = Y, 2 = V (input), 4 = U (constant) | [V] (§5) |
| src1 sel | w0[27:24] | bit1 = V (input), bit0 = Y, bit3 = +a0 (relative address). X if 0 | [V] bit3 is [?] (used only in the GLES1 matrix palette path, unverified) |
| src1 idx | w0[31:28] + w1[2:0] | low 4 bits + high 3 bits (×16) | [V] yuv2rgb c16–c18 = w1 bit0. GLES1 `c105..c108` = w1[2:1] = 3 |
| src1 = U | w1[7] | src1 read from constant file. Result's Y copy is not written | [V] 2D/GLES1 matrix multiply, FS `= filterColor` |
| dst idx | w1[11:8] | destination register | [V] |
| dst Y control | w1[28], w1[5] | §5.3 | [?] fitted result |
| dst = O | w1[19] | write to output file (`0x0C8` if op 0x0c) | [V] |
| (w1[18]) | | common in 2D/GLES1 code. No effect found [?] | |
| opcode | w1[27:20] | §7 | |
| w1[29] | | set on the first instruction reading a texture result (presumed wait marker) [?]. Ignored | |
| branch condition | w1[19:12] / w1[11:8] | §8 | [?] |

**NOP** = `w0 = w1 = 0`. The compiler fills result latency with NOPs (e.g. 3 NOPs after RSQ, 1 after TEX). The interpreter does not model latency.

## 4. Operand descriptors (0x461)

| Bits | Meaning | Evidence |
|---|---|---|
| [29:26] | write mask. bit26 = x … bit29 = w | [V] e80f92 `a = tex.a*col.a` is 0x20… (w only) |
| [25:21] | src1 modifier X | |
| [20:13] | src1 swizzle (2 bits per component, x lowest. identity = 0xE4) | [V] yuv2rgb `.wwww` scalar uniform |
| [12:8] | src2 modifier Y | |
| [7:0] | src2 swizzle | [V] |

Modifier (X/Y, 5 bits): bit4 = **negate** [V] (the `−1` in `xy*2 − 1`).
Low 3-bit values:
- 3 = replace the operand with the **ROM constant vector** [V].
- 2 = treat as homogeneous. w = 1, and op 0x02 (sum) excludes w [V].
  This makes `gl_Position.w = 1` in the 2D vertex program and gemini's `dot(vec3)` come out right.
- 0/1/4 = plain [?]. 4 is often on an unused src2; 1 has no case where an effect is visible.

ROM vector = **(0, 0.5?, 1, 2)** [V(x, z, w) / ? (y)]. Confirmed uses:
- the 2 and 1 in `gl_Position.xy * 2.0 − 1.0`
- `gl_Position.zw = (0, 1)`
- alpha = 1 in the YUV FS
- the (1,1,0,1) multiplied into the translation row of the 2D vertex program
- the 0 in CMP

## 5. Register files

| File | Read | Write | Initial value | Status |
|---|---|---|---|---|
| V input | src1 sel bit1, src2 file 2 | — | VS: vertex attributes, FS: interpolated VS output k → input k | [V] |
| O output | — | w1[19] | copy of input | [V] VS o0 = clip coords. FS **o1 = color**, o0 = depth (prologue) |
| X general | src1 sel 0, src2 file 0 | default | copy of constant file (uniforms and temporaries in one file; the compiler lays them out without overlap) | [V] |
| Y general (second copy) | src1 sel bit0, src2 file 1 | §5.3 | copy of constant file | [V] fitted result |
| U constant | src1 w1[7], src2 file 4 | read-only | 0x450..0x454 | [V] |
| a0 | src1 sel bit3 | op 0x11 | 0 | [?] |
| cond[4] | branches | op 0x1E | 0 | [?] |

### 5.1 Vertex programs
- Inputs and outputs are **separate files** [V]. The GLES1 package re-reads V0 (position) after writing o0.
- GLES1 attribute layout (measured + code): V0 position, V1 color, V2 texcoord 0, V3 texcoord 1, V4 normal,
  V6/V7 matrix palette weights/indices [?]. GLES1 outputs are o0 position, o1 color, o2 tex0, o3 tex1.
- gemini (type 0) attribute numbers equal the index in the binary's attribute table (inColor 0, inPosition 1, inUV 2, mtxL2W_0 3,
  mtxL2W_1 4) [V].

### 5.2 Fragment programs
- FS input k ≥ 1 goes not only into V but also into X/Y k [V]. E.g. the coordinate operand of `texture2D(tex, varUV)` is X2,
  `= varColor` is X1. Input 0 (position) is not put in X0, because the 2D FS reads `u_color` from X0 [V].
- Outputs start as a copy of inputs. `rgb = varColor.rgb; a = …` writes only o1.w [V].
- Output writes within an FS are also visible in the same-numbered X [?]. Because the GLES1 texture environment program does
  `X0 = X4`, `o1 = X0` after `o4 = tex`.
- Color is **o1**. o0 is written by the depth prologue (`0x0C8` dst 0).

### 5.3 X/Y copy rule (fitted result) [?]
- X is always written.
- The Y copy is not written when: `w1[7]` is set, or `w1[28]` is set and `w1[5]` is not.
- How the rule was found: brute-forced combinations of the three files X, Y, U and write/read codes.
  - Target: 19 libObj programs (25 test items). Picked the only family satisfying all of them (`solve*.py` in `scratchpad`).
  - Decisive case 1: 2D vertex `bcd6a4` 59–62. `Y1 = tm1`, `X1 = tc*tm0`, `o1 = Y1 + X1` gives texture matrix scale + offset.
  - Decisive case 2: 2D `bcda74` 56–58.
- The hardware reality is probably pipeline forwarding / a 2-port file. The true meaning of these bits is still unknown.

## 6. Confirmed data flow example (gemini VS, libObj 0xe7ed54, 34 instructions)
GLSL (libObj 0xbcf090): `gl_Position.xy = (dot(inPosition, mtxL2W_k) * invScreenSize) * 2 − 1; y *= −1; w = 1`.
- 57–59: copy inputs → X3, X1, X0.
- 61–62: `MUL X4 = V3*X3`, `X3 = V4*X3`.
- 63–64: `op02` horizontal sum (X=2 → xyz) → X3.x, X3.y.
- 74: `MUL X2 = X4 * U7(invScreenSize)`.
- 75–76: `MUL ×ROM.w(2)`, `ADD −ROM.z(1)`.
- 79: `MUL ×U6.x(−1)`. `c6 = (−1,0,0,0)` from the constant section.
- 78/80: `.zw = ROM.x/ROM.z`.
- 82: `o0 = Y2`.
Running with test values gives `(−0.625, 0.75, 0, 1)`. Matches hand calculation [V].

## 7. Opcodes (w1[27:20])

| op | Name | Meaning | Status / evidence |
|---|---|---|---|
| 0x0C | MOV | dst = src1 | [V] |
| 0x01 | ADD | src1 + src2 | [V] `tex+varColor` |
| 0x03 | MUL | src1 × src2 | [V] `tex*varColor` |
| 0x02 | DP (horizontal sum) | x+y+z(+w, w excluded if modifier 2), broadcast. Matrix multiply is `MUL` + `0x02` | [V] gemini dot3, GLES1 MVP |
| 0x1C | MAX | | [V] pattern: lighting `max(N·L, 0)`, 2D `max(Y, minY)` |
| 0x1D | MIN | | [V] pattern: depth prologue clamp, lighting `min(x, 1)` |
| 0x05 | RSQ | 1/√src1.x | [?] strong: normalize pattern `MUL`, `DP`, `0x05`, NOP×3, `MUL` |
| 0x04 | RCP | 1/src1.x | [?] |
| 0x07 / 0x08 | EX2 / LG2 | | [?] presumed from pow pattern (LG2 → MUL → EX2) |
| 0x11 | MOVA | a0 = (int) src1 | [?] pairs with the following `?9x` relative-address reads |
| 0x1E | CMP | cond[c] = src1[c] OP src2[c]. OP = w1[18:16] | [?] 3 = GT, 4 = EQ fitted from GLES1 flag checks. 0 NE, 1 LT, 2 LE, 5 GE presumed |
| 0x20 | TEX | dst = sample(unit = w1[16:14], coord = src1) | [V] pattern. unit field is [?] |
| 0x2B | TEX→color + END | o1 = src1 (mask), end | [?] last instruction of the 2D texture FS. No END after it |
| 0x24, 0x3D | conditional branch | §8 | [?] |
| 0x0B 0x0D 0x0E 0x0F 0x12 0x13 0x18 0x19 0x1A 0x1F 0x26 0x27 0x29 0x2A | not implemented | used only in GLES1 lighting/texgen/GS paths | `LOG_UNIMP` then false |

## 8. Control flow (opcode 0, w1[15:12])

| w1[15:12] | Behavior | Evidence |
|---|---|---|
| 0 (w0=w1=0) | NOP | [V] |
| 1 | CALL w0 | repeated GLES1 0x1B0 subroutine calls [?] |
| 2 | RET | [?] |
| 3 | JMP w0 | loop back-edge (154 → 0x3A) [?] |
| 4 | END | [V] |
| 0xD | block marker (end of conditional block?) → NOP | [?] |
| 5, 6, 0x11 | GS (EMIT family?) | not implemented |

Conditional branch `op 0x24 / 0x3D`, target = w0 [?]:
- w1[19:12] is 2 bits per component, x is the top pair. In each pair bit1 = ignore, bit0 = required value, combined with AND.
  - 0x7F = x only, 0x55 = all, 0x6A/0x9A/0xA6/0xA9 = one of x/y/z/w.
  - Evidence: the GLES1 light type switch (291–299) uses 0x6A, 0x9A, 0xA6, 0xA9 in sequence.
- w1[10] assumed to be invert. `6a4` of the `0x3D` family behaves that way.
- With this assumption the GLES1 package compositing path comes out correct (§9). Other paths (lighting, fog, palette) are not verified.

## 9. Verification (`tools/test_dmp_shader.c`)
Build/run commands are at the top of the file. Firmware files are read as data only. Result: **25/25 PASS**.
- Simple GLES2 (0xe813c4, 0xe814c4)
  - VS pass-through: `gl_Position = a_position`, `v_texCoord`
  - FS: `= filterColor`, `texture2D(s_texture)*filterColor`
- gemini (0xe7ed54 …)
  - VS position/color/UV
  - 5 FS: `varColor`, `tex`, `tex*col`, `tex+col`, `rgb=col, a=tex.a*col.a`
  - YUV→RGB FS. Matrix columns multiply texel (t.y, t.z, t.x). With BT.601 coefficients, this means texel order is (Cr, Y, Cb).
- 2D renderer (0xbcda74, 0xbcd560, 0xbcd6a4)
  - position = `ep0*x + ep1*y + ep2`, w = 1
  - texcoord = `tc*u_texMatrix[0] + u_texMatrix[1]`
  - FS `u_color`, texture
- GLES1 package (compositing path, measured uniforms)
  - position = MVP·pos, color = V1·c25.w, tex0 pass-through
  - driver-generated 0x1C8 FS (REPLACE) = texel
- Items whose expected value is "presumed" rather than "source meaning": meaning of the 2D texture matrix, GLES1 attribute layout, YUV texel component order.

## 10. Remaining work / unconfirmed
- Actual meaning of the X/Y copy rule (§5.3), w1[18], w1[28], w1[29].
- CMP modes 0/1/2/5, low nibble of branch condition, CALL/RET/block marker.
- Unimplemented opcodes (§7) — GLES1 lighting (`0x0F`, `0x0E`, `0x1A` …), texgen, GS (point/line program `dmp_point_shader_prog`).
  - When these paths execute, `LOG_UNIMP` (`-d unimp`) prints `dmp-shader: opcode at N`.
- ROM.y value, TEX unit field, exact meaning of 0x2B.
- Depth prologue (uses op 0x11.8) is skipped in the default state, so it is not implemented.

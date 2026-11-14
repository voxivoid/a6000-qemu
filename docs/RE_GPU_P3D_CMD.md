# RE: DMP SMAPH-S30 P3D command lists and fixed-function registers

Evidence document for the P3D engine in `hw/display/dmp_smaph.c` (command list interpretation + software rasterizer).
The shader ISA is covered separately in `docs/RE_GPU_P3D_SHADER.md` (interpreter `hw/display/dmp_shader.c`).
For overall GPU structure, PSC/PPF and the IRQ contract, see `docs/RE_DISPLAY_GPU.md` §2.

Sources:
- The GLES2 driver is statically linked into `libObj.so`. Hex-Rays output is `re/ida/libObj.so.c`.
  - Command writers: `sub_5FC750` (single write), `sub_5FC790` (burst), `sub_5FC720` (raw 8-byte entry).
  - Draw state builders: `sub_600EB0` (raster/fragment/framebuffer/texture unit mapping), `sub_5FD990` (vertex fetch, primitive, draw kick).
  - Textures: `sub_5FCED0` (sampler), `sub_5FD100` (texture unit).
  - Other: `sub_611080` (glClear), texture format table around `sub_637740` (L1401420~).
  - `L<n>` below are line numbers in `re/ida/libObj.so.c`.
- Measured: booted stock firmware in the emulator and dumped the list on every `+0x1800` kick (first-boot "Area/Date/Time setting" screen, 45 lists).
- Notation: **[?]** = unconfirmed/hypothesis. **V** = confirmed working with unmodified firmware + emulator.

---------------------------------------------------------------------------------------------------
## 1. Submission path (summary)
- `rm_reg_*` (libObj) writes `+0x1804 = list PA`, `+0x1800 = entry count` (in 8-byte units).
  - The list lives in the `gl_common` heap (PA 0x8D7A0000~).
- Completion is signaled by status bit31, IRQ 76. The ISR acks with `+0x1000 = 0` (RE_DISPLAY_GPU §2.2).
- The model executes the whole list **synchronously** at the `+0x1800` write, then sets the completion bit.

## 2. Command list format
An entry is 8 bytes `[header][value]` (two little-endian 32-bit words). Order is reversed vs PICA200 (`[value][header]`).

| header bits | Meaning | Evidence |
|---|---|---|
| 0 | **call**: `value` = sub-list PA, bits 3-11 = sub-list entry count. Returns to the next entry after execution | glClear tail `v47[0] = (8 * count) \| 1; v47[1] = PA` (around L1372950, `sub_5FC720`). Measured `0x229 → 0x45` entries = matches shader code block size |
| 1 | register auto-increment on burst | `sub_5FC920` (header `0x0450F026`) writes consecutively to 0x450..0x454. Same result as the path sending the same content as single writes |
| 2 | **burst**: bits 3-11 = extra word count (= total words − 1). Extra words fill from the next entry and are zero-padded to an 8-byte boundary | `sub_5FC790`: `(count-1)<<3 \| 4`, pads a trailing 0 if odd |
| 3-11 | count field for call/burst. 0 for single writes | |
| 12-15 | byte enable (0xF = full 32 bits). 0 = no write. Padding entries `00000000 xxxxxxxx` are this case | `sub_5FC750`: `(reg<<16) \| 0xF000` |
| 16-27 | register number (max observed 0x5E4) [? upper bit width] | |

- Sub-lists are reused blocks: shader code (0x458/0x459), operand descriptors (0x460/0x461), constant uniforms, prebuilt lists for glClear, etc.
- Measured nesting depth is 1. The model allows up to 4.
- A list always ends with `0x116 = sync PA>>4`, `0x117 = 1`, `0x44 = 0`, `0x010 = 0x12345678`.
  - 0x010 = 0x12345678 is the same value as PICA's FINALIZE.
  - In the model these four writes do nothing (end of list = completion). [? what the hardware writes to the dummy buffer (`dword_EF0188`) pointed to by 0x116/0x117 is unconfirmed]

## 3. Register map (P3D register numbers; not a direct match to MMIO offsets)

### 3.1 Rasterizer / viewport (`sub_600EB0` L1358757~, glClear L1372780~)
| reg | bits | Meaning | Evidence |
|---|---|---|---|
| 0x41 | 0-1, 4 | Culling. 1 = cull counter-clockwise (CCW, window coords y-up) triangles, 2 = cull clockwise (CW), bit4 = front face is CW. 0 for points/lines | 600eb0 L416-445 |
| 0x42 / 0x43 | f32 | viewport width/2, height/2 | `v386 = w * 0.5` |
| 0x47 | s16 x, s16 y | scissor min. Units are half pixels, origin is viewport center: `2·x0 − 0x49.x` | |
| 0x48 | s16 x, s16 y | scissor max (exclusive): `2·(x0+w) − 0x49.x` | |
| 0x49 | u16 x, u16 y | viewport center ×2 = `2·vpX + W`, `2·vpY + H` | |
| 0x4A | 24 bits | depth offset (units/65535 × 16777000) | unused in model |
| 0x44 | | written 0 at list end [?] | |

Window coords are `x = 0x49.x/2 + ndc.x·0x42`, `y = 0x49.y/2 + ndc.y·0x43` (GL convention, y-up).
Row 0 of the color buffer is the bottom of the screen. The following PPF transfer sets flag bit0 (vertical flip) and moves it to the panel buffer.
Measured PPF: `0xC10 = 0x4401` = flip, input/output format 4.

### 3.2 Fragment ops / framebuffer (`sub_600EB0` lines 1370~2260)
0x100-0x107 match PICA200 (only the 0x105 mask position differs). From 0x10A the layout differs.

| reg | bits | Meaning |
|---|---|---|
| 0x100 | bit8 | 1 = blend, 0 = logic op (driver never enables it) |
| 0x101 | 0-2 eqRGB, 8-10 eqA, 16-19 srcRGB, 20-23 dstRGB, 24-27 srcA, 28-31 dstA | Blend. Factors: 0 ZERO, 1 ONE, 2/3 (1−)SRC_COLOR, 4/5 (1−)DST_COLOR, 6/7 (1−)SRC_ALPHA, 8/9 (1−)DST_ALPHA, 10-13 CONST, 14 SATURATE. Equations: 0 ADD, 1 SUB, 2 REV_SUB |
| 0x102 | 0-3 | logic op (ignored in blend mode) |
| 0x103 | RGBA8 | blend constant color |
| 0x104 | 0 en, 4-6 func, 8-15 ref | Alpha test. Compare funcs: 0 NEVER, 1 ALWAYS, 2 EQ, 3 NE, 4 LT, 5 LE, 6 GT, 7 GE |
| 0x105 | 0 en, 4-6 func, 8-15 compare mask, 16-23 ref, 24-31 write mask | Stencil (mask positions reversed vs ctrulib) [? unverified on real hardware] |
| 0x106 | 0-2 sfail, 4-6 zfail, 8-10 zpass | stencil ops |
| 0x107 | 0 depth test, 4-6 func, 8-11 RGBA write, 12 depth write | |
| 0x10A | 0xB / 0xC | framebuffer mode [?] (0xC if the FBO is an untiled texture) |
| 0x10B / 0x10C | 0-3 | color read / write enable |
| 0x10D / 0x10E | bit0 stencil, bit1 depth | depth/stencil read / write |
| 0x10F | 0-1 | depth format: 0 D16, 2 D24 [?], 3 D24S8 |
| 0x110 | 16-18 | **color format**: 0 RGBA8 (PICA word order), 2 RGB5A1, 3 RGB565, 4 RGBA4, 5 RGBA8 (GL byte order R,G,B,A) |
| 0x111 | PA>>4 | depth buffer |
| 0x112 | PA>>4 | color buffer |
| 0x113 | 0-12 w, 16-28 h | buffer size |
| 0x115 | bit0 | dither |
| 0x116 / 0x117 | PA>>4 / 1 | list-end sync/flush (§2) |

- The color buffer is **8×8 Morton tiled**. Matches PPF reading the panel OSD target (`0x8D7B0500`, 640×480 RGBA4) tiled→linear.
- Confirmed rendering comes out correct on screen (§6).

### 3.3 Vertex fetch / draw (`sub_5FD990`, fd990 = libObj L1355621~)
| reg | bits | Meaning |
|---|---|---|
| **0x202** | 0-27 count, 28 u16, 29 u32 (u8 if both 0), 31 indexed | **Draw kick**. Written last, after all state lists are emitted (L1361641) |
| 0x203 | 32 | index data PA if indexed, else first vertex number |
| 0x204 / 0x205 | 4 bits per slot (0-7 / 8-15) | slot → VS input register |
| 0x206 / 0x207 | 4 bits per slot | type: 0 BYTE, 1 UBYTE, 2 SHORT, 7 USHORT, 3 FLOAT. bit3 = normalized |
| 0x208 | 2 bits per slot | component count − 1 |
| 0x209-0x20C | 8 bits per slot | byte offset within buffer |
| 0x20D | 0-3, 16+k | buffer count − 1. bit16+k = buffer k is a constant (generic attribute) buffer |
| 0x20E / 0x20F | 4 bits per buffer | buffer attribute count − 1 |
| 0x210 / 0x211 | 4 bits per buffer | buffer's first slot |
| 0x212-0x215 | 8 bits per buffer | stride − 1 |
| 0x216+i | 32 | PA of buffer i (i < 16) |
| 0x400 | | 0x1000300 (points/lines), 0x1000200 (triangles) [?] |
| 0x401 | | primitive: 0 points, 0x10 lines, 0x802 line strip, 0x3020 triangles, 0x20 strip/fan |
| 0x402 / 0x403 | | line strip control / 0xA0304 (triangles) [?] |
| 0x404 | 0-1 | topology: 0 list, 1 strip, 2 fan (bit5 always 1) |
| 0x405 / 0x406 | | related to shader output count [?] (`v473[13..15]−1`) |
| 0x408-0x410 | (size<<16 \| offset) | internal vertex/varying memory layout [?]. Not needed by the model |

Measured UI example (`0x202 = 0x90000004`):
- 4 u16 indices (0,1,2,3), triangle strip.
- 1 buffer, stride 64, 5 float attributes: vec3@0, vec4@12, vec2@28, vec3@36, vec3@48.
- Input mapping `0x204 = 0x43201`.

GL_LINE_LOOP is converted by the driver to a strip + a 2-vertex LINES.

### 3.4 Programmable unit (code/constant upload; semantics in the SHADER doc)
| reg | Meaning | Evidence |
|---|---|---|
| 0x440 | 0xF0004 / 0xF0F04 [?] | L1361100 |
| 0x441 | 0 / 0xFFFFFF00 [?] | |
| **0x442** | vertex program entry point (instruction index) | `program+1244` (L1361133) |
| **0x443** | fragment program entry point. +9 may be added to `program+1275`: with default depth range and no polygon offset, the 9-instruction prologue is skipped | L1361141-1361172. Measured load at 0x5A, entry 0x63 |
| 0x445, 0x470-0x477 | 0 [? int/bool uniform candidates] | |
| 0x450 | float uniform index (observed 0..0xC9) | |
| 0x451-0x454 | x, y, z, w (f32). Write to 0x454 commits and increments index by 1 [? auto-increment] | glClear: c3 = clear color, c4 = depth·2−1 |
| 0x458 | code load address (in instructions, 1 instruction = 2 32-bit words) | after loading 68 words at 0x38, the next program loads at 0x5A (= 0x38+34) |
| 0x459 | code word (address +1 word per write) | |
| 0x460 / 0x461 | operand descriptor index / data (auto-increment) | |

- Vertex and fragment programs use **the same code memory and the same uniform space** (unified shader).
- The model gathers everything into one `DmpShaderProg`.
  - Fills `code[2·addr + k]`, `opdesc[]`, `funif[]`.
  - At draw time sets `entry` = 0x442 or 0x443, `extra[i]` = P3D reg `0x440+i` (i < 64).
  - Also fills the `tex_sample` callback (§3.5).

### 3.5 Textures (`sub_600EB0` L630~, `sub_5FCED0`, `sub_5FD100`)
| reg | Meaning |
|---|---|
| 0x580 | texture use flag (9 = used, 0 = none, 1 = other path) [?] |
| 0x581-0x583 | sampler → unit mapping (one nibble per slot, `65537<<n`) [?]. The model uses sampler number = unit number |
| 0x584 | 0x11 [?] |
| 0x585+11u | unit u: border color (r<<24 \| g<<16 \| b<<8 \| a) |
| 0x586+11u | allocated size `(w8<<16) \| h8` (aligned to multiples of 8, memory layout) |
| 0x587+11u | original size `(w<<16) \| h` (coordinate scale) |
| 0x588+11u | LOD bias (bits 0-12), min/max level (16-19, 24-27) |
| 0x589+11u | texel PA>>3 (+X face for cubemaps) |
| 0x58A-0x58E+11u | remaining cubemap face PA>>3 |
| 0x58F+11u | format: bits 0-4 code, bit9 ETC1, bit12 = texture object +48 (presumed untiled layout [?]) |
| 0x5DD+u | sampler: bit0 1, bit1 mag LINEAR, bit2 min LINEAR, bit24 mipmap, bits 8-9 wrap T, bits 12-13 wrap S (0 edge, 1 border, 2 repeat, 3 mirror) |

Format codes (L1401420~): 0 RGBA8 (PICA), 25 (0x19) GL_RGBA/UNSIGNED_BYTE (bytes R,G,B,A), 2 RGB5A1, 3 RGB565, 4 RGBA4,
5 LA8, 7 L8, 8 A8, 12 ETC1, 24 YUV422 family [?].

- Texels are **8×8 Morton tiled**, row 0 is the top of the image (first uploaded row).
  - Detiling the measured A8 texture `0x8E738000` (408×40) shows the (Korean-locale) dialog text "Set area/date/time." upright.
  - The RGBA4 dialog frame `0x8DFB0200` (600×416) was confirmed the same way.
- 0x1019 (= 25 | bit12) is used when compositing the Rigel window buffer (`0x8E9B7B80`, 640×480). The buffer content is a solid color, so byte order and layout are not yet confirmed [?].

## 4. Model pipeline (dmp_smaph.c)
1. List interpretation (§2) → P3D register file (4096 entries). Side effects only on 0x202, 0x450-0x461.
2. `0x202` kick:
   - read indices → vertex fetch (§3.3) → `dmp_shader_run(VERTEX)`.
   - VS results are reused within the last 8 index range.
3. Output register 0 is taken as clip coordinates [?]. Triangles with any vertex w ≤ 0 are dropped. **Near-plane clipping not implemented**.
4. Viewport transform (§3.1) → culling (0x41) → scissor + buffer bounds → edge-function rasterization sampled at pixel centers.
   - All varyings are perspective-correct interpolated.
   - FS input k = VS output k [?]. Input 0 is replaced with (x+0.5, y+0.5, z, 1/w).
5. `dmp_shader_run(FRAGMENT)` → output V1 = RGBA (V0 = depth), following the hypothesis in `dmp_shader.c` [?].
   - alpha test → color write enable (0x10C) → blend (0x100/0x101/0x103) → channel mask (0x107) → write to tiled buffer.
6. Not implemented: depth/stencil test (UI draws without a depth buffer: 0x10D/0x10E = 0, 0x111 not written), point/line primitives, mipmaps, ETC1, dither, logic op.

## 5. Remaining work / verification plan
- [?] The value 0x116/0x117 sync actually writes to memory. Harmless for now since the driver does not poll it.
- [?] 0x450 auto-increment, 0x581-0x584 sampler mapping, 0x10A, 0x400/0x402/0x403/0x405/0x406.
- [?] Shader output/input register semantics (position, color, depth, discard). Must be reconciled with `RE_GPU_P3D_SHADER.md`.
- Depth/stencil implementation (likely used by Android GL apps, 3D effects such as `SequenceApp3DZoomCmd`, etc.).

## 6. Measured results (2026-11-14, unmodified firmware, first-boot "Area/Date/Time" screen)
- ~60 s after boot, 44 P3D lists and 32 draws execute (`p3d-draw` in `-trace dmp_smaph_job`).
  - 27 are 4-index quads drawn to the 640×480 OSD target `0x8D7B0500`.
  - 3 are 88-index 9-patch buttons.
  - 2 draw to the RGBA8 target `0x8D858080` (Rigel window composite).
  - No unhandled header bits (LOG_UNIMP).
- PPF moves the color buffer to panel buffers `0x8F758000` / `0x8F7EE000`. Panel buffers are no longer zero (307200/307200 pixels).
  - **Decoded as RGBA4 it is exactly the stock UI**: black background, thin-bordered dialog box, white text "Set area/date/time.",
    selected orange "OK" button, gray "Cancel" button, bottom guide "◆Select ●OK" (on-screen text is Korean; English glosses here).
- **V** (confirmed by this measurement):
  - list format (call/burst/byte enable)
  - vertex fetch (stride 64, 5 float attributes)
  - viewport / scissor / culling (0x41 = 2)
  - tiled color buffer (RGBA4, row 0 = bottom, PPF flip)
  - A8 and RGBA4 tiled textures with bilinear sampling
  - SRC_ALPHA / ONE_MINUS_SRC_ALPHA blend
- Panel format:
  - This buffer is **RGBA4444**. Evidence: P3D 0x110 = 4, PPF `0xC10 = 0x4401` (input/output format 4), background pixel `0x000F` = opaque black.
  - However, the current VPH scanout model (console 0 / `screendump`) reads this buffer as RGB565, so the same screen appears with a navy background and color-shifted buttons.
  - VPH channel format interpretation must be fixed to RGBA4 (cxd90014_vph.c's responsibility).
- Unresolved: the Rigel window composite program (target `0x8D858080`) stops the interpreter at shader op 0x1E (opcode not implemented in `dmp_shader.c`), so those 2 draws write no pixels.

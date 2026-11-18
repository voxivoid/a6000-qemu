# Plan: Sony Picture Profiles (a6300) and other cameras' looks as direct A6000 ISP tables

Goal (owner): implement every Sony Picture Profile that exists on the a6300 (PP1..PP10 presets and their parameters) and
then emulate looks of other cameras, by writing the A6000 ISP directly (RAM-only, reversible). Resources and registers:
`A6000_ISP_REGISTER_MAP.md`. Method/history: `A6000_LOOK_MOD_ISP.md`. Marks: **[V]** verified on camera, **[S]** static,
**[?]** hypothesis / not done.

## 1. What a "look" is on this ISP (capability budget)
| ISP resource | Offset | Verified | Can express |
|---|---|---|---|
| Gamma, 96 knots, global | `0x15300` | **[V]** | any monotone-ish tone curve: S-Log2/3, LogC, Cine1-4, ITU709, Movie/Still gamma, black gamma, knee (all are just curve shapes) |
| 3x3 matrix Q10 (linear, pre-gamma [?]) | `0x1550c` | **[V]** | colour mode matrices, saturation, S-Gamut/AWG primaries, white-point-neutral channel mixing |
| 3D LUT banks (5x9x9 YCbCr deltas) | `0x17000..` | [S] writer known, never written | hue/sat/luma-dependent colour (Colour Depth RGBCMY, Colour Phase, skin tone shaping) |
| 2D/hue-sat LUT candidate | `0x16200..0x16dff` | [?] | per-hue saturation/hue shift |
| Black level / WB gains | `0x3400..` | [?] | black level (PP "Black Level"), WB shift |
| Detail / sharpening, NR | `0x15700..0x15cxx` etc. | [?] | PP Detail group |
So: the **tone side** of all PP gamma/knee/black-gamma/black-level is fully reachable today; the **colour side** is
reachable with a 3x3 now, per-hue control needs the LUT stage (next RE target).

## 2. Sony Picture Profile parameters (a6300 / libObj `PARAMID_PP_*`) -> ISP mapping
| PP parameter | Values (a6300) | Maps to A6000 ISP | Status |
|---|---|---|---|
| Black Level | -15..+15 | gamma low end offset (or `0x3xxx` black regs) | tone side done by curve offset [?] |
| Gamma (type) | Movie, Still, S-Cinetone, Cine1, Cine2, Cine3, Cine4, ITU709, ITU709(800%), S-Log2, S-Log3, HLG1-3 | gamma table | S-Log2 **[V]**, S-Log3 formula ready, ITU709 ready; Movie/Still/Cine*/S-Cinetone/HLG need the real curves (section 4) |
| Black Gamma Range / Level | Wide/Mid/Narrow, -7..+7 | low-end shaping of gamma table | curve edit [?] |
| Knee Mode / Auto Set / Point / Slope | Auto/Manual, 75..105%, -5..+5 | highlight roll-off of gamma table (+ maybe `0x15b08` curve) | curve edit [?] |
| Color Mode | Movie, Still, S-Cinetone, Cine, Pro, ITU709 Matrix, B&W, S-Gamut, S-Gamut3.Cine, S-Gamut3 | 3x3 matrix (+ 3D LUT for S-Cinetone/Cine) | S-Gamut-like and ITU709 matrices: matrix **[V]** mechanism, coefficients from published primaries; look-specific ones need captures |
| Saturation | -32..+32 | matrix scale around luma (`make_hwmatrix.py sat S`) | **[V]** mechanism |
| Color Phase | -7..+7 | matrix hue rotation / LUT | rotation in matrix, [?] calibrate |
| Color Depth R/G/B/C/M/Y | -7..+7 | 3D LUT / 2D hue LUT | needs LUT RE [?] |
| Detail Level/Adjust (V/H, B/W balance, Limit, Crispening, Hi-light detail) | | sharpening block | needs register ID [?] |
| Copy / Reset | | n/a (host-side presets) | host tool: store presets as files |
| WB Shift (WB_SHIFT_*) | | WB gains / matrix | [?] |
PP1..PP10 factory presets: exact per-preset parameter values are **not yet extracted**; they must be read from the a6300
firmware/settings defaults (section 4), not assumed.

## 3. Implementation architecture
1. **Preset = a small file** (`mods/look/presets/*.json`): `{gamma: table or generator+params, matrix: 3x3 relative to
   stock, lut: optional, flags}`. Generators live in `mods/look/make_hw*.py`.
2. **Applier** (`look.sh apply`): reads the live stock matrix (it depends on WB/Style), composes target matrix, writes
   gamma + matrix, patches both writers. `revert` restores writers. (look.sh currently supports gamma only; matrix
   support = TODO, procedure documented, binaries tested manually.)
3. **Parameters** (Saturation, Black Level, Knee, ...) are applied by regenerating the tables on the host and pushing
   (no camera UI; on-camera UI would need the PP menu hook in `tools/pphook`, separate task).
4. **Stills vs video**: stills pick up the same SRAM; movie uses its own camera table (saved `movie_tbl.bin`), the writer patch
   freezes whichever we wrote. A per-mode preset would need a mode-watcher.

## 4. What is needed from the a6300 to get exact curves  (next big work item)
The a6300 av-cam (unpacked in `firmware_a6300/unpacked`) holds the real PP gamma engine: `EC_Gamma.cpp`,
`job_EC_CalcCaptureGamma`, `CheckSetExGammaTableControl`, S-Log2/3 + 6 knee points, and a "stock EC record 31 (S-Log3)".
Plan: (a) extract the 96-word EC gamma records (stock table set in the image, a6400 had `0x03e5363c` = record 31) - each is
exactly the format our SRAM expects (96 x u32); (b) find the PP-type -> record index map; (c) knee/black-gamma
parameter formulas from `job_EC_CalcCaptureGamma`. If records are pre-baked in the image, we get every Sony gamma
bit-exactly with no modelling: just write them to `0xF3215300` (the a6300 SRAM format is identical: same writer, 96 x u32
at CC +0x15300, a6300 writer `0x43c2b91c`). The input knot axis is the same for the same ISP generation [?: confirm
by diffing knots in the a6300 image]. Matrices: from the a6300 colour tables (Color Mode -> matrix, saturation,
phase), same Q10 3x3 format as `0x1550c` (a6400 "RGBMatrixSW" is s16 Q10).
Source-calibrate: the A6000 sensor differs from a6300; gamma is sensor-independent in principle (log of scene
linear) but the linear scale (what knot = 18% grey) depends on exposure and ISO; keep `--grey-x` as calibration.

## 5. Emulating other cameras' looks
Recipe per look (do in this order, measure each step):
1. **Tone**: capture a reference chart/scene on the A6000 with identity gamma (or stock) and the target camera's JPEG
   or LUT. Fit 96 knots: `y_target = f(linear)`. Published transfer functions (S-Log2/3, LogC3/4, C-Log2/3, V-Log, F-Log,
   Rec.709, HLG, PQ) can be generated directly (`make_hwgamma.py`: slog2/slog3/logc3 done; add others as small functions).
2. **Gamut**: published primaries -> 3x3 (`make_hwmatrix.py awg3` pattern; add `slg3`, `rec2020`, `vgamut`, ...).
3. **Hue/sat curve** of the camera (the "colour science" part): needs the LUT stage. Fit a 3D LUT from paired patch
   measurements (ColorChecker shot with both cameras, or target LUT cube -> 5x9x9 YCbCr deltas), as the a6400
   `lut-converter` does (`ivica00/bionz-x-research/a6400-lut-converter`, reusable fitter: same 5x9x9 7-bit delta
   structure per its docs). 
4. Validate by shooting a chart and comparing patches (Delta E), JPEG pulled from SD with `adb`.
Limits: 8-bit output, global gamma only (no local tone mapping, DRO off), sensor dynamic range fixed, no sensor-specific
noise/colour response transfer, and each camera's perceived look includes its sensor's spectral response (not
reproducible by a 3x3 beyond approximation).

## 6. Work queue (ordered)
1. Add matrix to `look.sh` (apply/revert, auto stock capture, cached backups). Verify the matrix survives video start.
2. Determine ISP order (matrix vs gamma) with an experiment: big-gain matrix + known gamma, compare chart.
3. Identify the 3D-LUT write path (`43b67a68`, bank writer `43b67918`, enable `0x15d40`): write an obvious pattern into
   one bank, view; document grid/format. Check `0x16200..0x16dff` as 2D LUT.
4. Extract a6300 gamma records + PP->record map + colour mode matrices from `firmware_a6300` (section 4); generate all PP
   presets; compare S-Log2 against our formula version.
5. Identify black level / WB gain (`0x3400`..), detail (`0x15700` block) registers by write tests.
6. Measure everything from real JPEG/MOV (Delta E tools), tune `--grey-x` per ISO/exposure.
7. Optional: on-camera selection (PP menu via `tools/pphook`) and mode-dependent presets.

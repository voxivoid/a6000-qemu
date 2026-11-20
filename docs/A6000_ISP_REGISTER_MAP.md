# A6000 ISP (CamCore "CC" block) - what we can change, and how

Status of every claim is marked: **[V]** verified on the real camera, **[S]** static RE of av-cam 3.21 only,
**[?]** hypothesis. Companion docs: `A6000_LOOK_MOD_ISP.md` (how the gamma was found, flicker/patch story),
`A6000_PICTURE_PROFILE_ISP_PLAN.md` (the goal: a6300 Picture Profiles + other cameras' looks as ISP tables).
Raw data: `docs/data/isp_regs_static_live.tsv` (every accessor-visible register: offset, live value, R/W, width,
accessor addresses) and `docs/data/isp_live_dump.txt` (non-zero words of the live window).
Tools: `mods/look/gammamod` (`-F hwrd|hwdump|hwwr`), `tools/isp_regscan.py`, `tools/isp_stages.py`, `mods/look/look.sh`.

Credit: the CamCore (CC) ISP register layout this map is built on (the 96-word gamma at `+0x15300`, the Q10 colour matrix at
`+0x1550c`, `+0x15480..+0x154b8`, the 3D-LUT bracket at `+0x15d40`, the 5x9x9 YCbCr delta LUT, rows summing to 1024) was first
documented for the A6400 by [ivica00/bionz-x-research](https://github.com/ivica00/bionz-x-research) (MIT, (c) 2026 ivica00):
[`docs/evidence/0505-av-cam-cc-3dlut-and-ec-gamma-pipeline.md`](https://github.com/ivica00/bionz-x-research/blob/main/docs/evidence/0505-av-cam-cc-3dlut-and-ec-gamma-pipeline.md),
[`docs/ISP_TRANSFORM_CATALOG.md`](https://github.com/ivica00/bionz-x-research/blob/main/docs/ISP_TRANSFORM_CATALOG.md), the `arri_isp`
mod and `a6400-lut-converter`. Everything marked **[V]** here was then checked on an A6000 (FW 3.21).

## 1. Access
* CamCore CC register block = physical **`0xF3200000`** (BIONZ X, CXD90014). av-cam (LIRO uITRON, VA = PA - 0x40000000,
  loaded at PA `0x83541000`) reaches it through accessor leaf functions `add.w r0,r0,#0x15xxx; ldr/str` on the handle
  (`handle->base`). **[V]**
* From Linux: `/dev/mem` + `mmap` (read() on MMIO fails). `gammamod -F hwrd <pa> <nwords>` reads, `-F hwwr <pa> <file>` writes
  (32-bit accesses, one 4 KiB page per call, <=256 words for hwdump/hwwr, any for hwrd). Without `-F` only the gamma table
  is reachable. Reads of every listed page were safe on the camera (11264 words, no hang). **[V]**
* Writes take effect immediately on the live pipeline. All changes are RAM/register only: battery pull = stock.
  Nothing is written to flash.
* The camera re-programs registers from its own software state at **events** (video start, mode change, shutter, style
  change). To make a write stick, patch the camera's own writer function to `bx lr` (section 4).

## 2. Block map (offsets from `0xF3200000`)
Generated from live reads + accessor scan of av-cam. "fn" = av-cam VA of the multi-field setter (W) / getter (R) that
programs the group; those are the functions to patch/hook. Meaning column is **[V]** only where stated.

| Offset | Size | fn (W / R) | Contents / meaning |
|---|---|---|---|
| `0x15300` | 0x180 | writer `43b67414`, wrapper `43b6747c`, 7 callers | **[V] Gamma**: 96 x u32 output (0..1023) at 96 fixed linear 11-bit knots (see look-mod doc) |
| `0x15480..0x154b8` | | | gamma control words + 9 extra words (`0x15494..0x154b4`: live `2,2,4,3,4,4,3,4,3`) [?] (a6400: gamma metadata) |
| `0x15500..0x1552c` | 12 words | W `43b675e0` / R `43b6768a` | **[V] Colour matrix**: `+0x15500/04/08` = three 1-bit flags (live 0), `+0x1550c..+0x1552c` = 3x3 **signed Q10**, one s16 per u32, row-major, written as the low 16 bits; rows sum to 1024 on stock. Verified: R<->B swap matrix visibly swaps colours live |
| `0x15600..0x156cc` | | W `43b67358`,`43b673ae`,`43b69264`,`43b692bc`,`43b69312`,`43b693bc` | 6 setters, ~30 regs. **[V] `0x156ac/b0/bc/c0` = saturation gains** (setter `43b69312`, fields `0x156a4..0x156c0`); rest unidentified |
| `0x15700` | 0xa0 | | table, live: `42998, 8053, 1911, 1245, 1150, ... 14, 28` decaying curve [?] (noise-reduction / shading-like) |
| `0x15800..0x15854` | | W `43b69b34`,`43b69bd8` | 12 + 4 fields [?] |
| `0x15900..0x15954` | 22 regs | W `43b66f84` / R `43b670ba` | [?] |
| `0x15a00..0x15aec` | ~56 regs | W `43b6862e..43b689be` (6 setters) | [?] large multi-parameter stage (live nonzero in 46 words) |
| `0x15b08` | 0x70 | | monotone rising curve `1,0,0,0,0,1,342,683,1053,2579,4133,...` [?] (a second tone/knee-like curve) |
| `0x15c10..0x15c18` | 3 x u16 | W `43b68c88` | **[V] sharpness (edge enhancement) gain**: `0x15c14`/`0x15c18` are 4-bit (value & 0xF), Standard 0 = 12 (`0xC`), sharpness +3 = 14; `0x15c10` drifts 118..122 (scene dependent). Writing 0 = visibly softer (verified, setter frozen). `0x15c30..0x15c7c`: further detail-block setters `43b68d18/86/f6` [?] |
| `0x15d00..0x15d44` | 18 regs | W `43b67988` / R `43b67b1a` | **3D-LUT stage control**: `+0x15d40` = write-enable bracket (1 during bank write, 0 after), `+0x15d44` bit0 status. [S] |
| `0x15e00..0x15eac` | ~28 regs | W `43b69884`,`43b6991e` ... | [?] |
| `0x16000..0x161ff` | 2 x 0x100 | | all words `6911` (0x1aff) live [?] (flat map/gain table; neutral) |
| `0x16200..0x16dff` | 12 x 0x100 | (bank writer `43b67918`, selector `43b67a68`) | packed 27-bit triplet words (e.g. `3027001`, `7319931`, `50280443`) [?] smooth bell-shaped profiles = **2D/hue LUT** candidate (strings `Copy2DLUT`) |
| `0x16d00` | | | words `134133949`/`67025085`/`58636349` [?] |
| `0x17000..0x17800` | 8 banks (sizes 0x12c,0xf0,0xf0,0xc0,0xc8,0xa0,0xa0,0x80) | `43b67a68` (-> `43b67918`) | **3D LUT banks**, a 1620-byte table split into 8 windows at `0x17000,0x17200,0x17300,0x17400,0x17500,0x17600,0x17700,0x17800`; identity is `256` per entry (live: 256/257). Grid per a6400 notes: 5x9x9 YCbCr deltas [S] |
| `0x14100..0x14a94` | | W `43b6bed2`,`43b6c3ca`,`43b6c480`,`43b6c82e`,`43b6cf56` | [?] earlier pipeline stages (`0x14300` +0xc8: 14 regs; `0x14700` live nonzero 30 words) |
| `0x1118c..0x111a8`, `0x11400..0x1190c` | | W `43b663dc`,`43b665b6` | [?] |
| `0x3100..0x3e68` | | W/R `43b69ff0`,`43b6a0ac`,`43b6a636`,`43b6aea0`... | separate sub-block (RAW-side?) : `0x3400,0x340c,0x3410,0x3414` read u16 live **256 each** [?] = four channel gains (Q8 = 1.0, WB-like); `0x3d00..0x3d40` 33 regs; `0x3e00..0x3e68` 21 regs |
| `0x99a00`, `0xa1e00..0xa5e60` | | `43b799a2`, `43b79d5c`, `43b793b0`... | far window (statistics/AE-AWB readback?) [?] |

Complete register-by-register list with live values: `docs/data/isp_regs_static_live.tsv` (415 accessors).
Regenerate: `tools/isp_regscan.py 43b60000 43b80000 regs.json` then `tools/isp_stages.py av-cam.bin regs.json`.

## 3. Resources that are confirmed usable for looks
### 3.1 Gamma (tone curve) **[V]**
`0xF3215300`, 96 x u32, see `A6000_LOOK_MOD_ISP.md` 7c. Generator: `mods/look/make_hwgamma.py` (`slog2`, `slog3`, `logc3`,
`arri709`~, `rec709`, `identity`). Applies to live view, stills and movie. One table for the whole frame (global, not per
colour channel).

### 3.2 Colour matrix **[V]**
`0xF321550C..0xF321552C`, 3x3 s16 Q10. Row sums 1024. Stock (Standard style, this unit, daylight WB) was
`[[1292,-180,-88],[-44,1340,-272],[-24,-108,1156]]`; it changes with Creative Style / colour mode / WB (it is the
sensor-to-display matrix, so it already contains WB-dependent camera calibration; always start from the live stock).
`mods/look/make_hwmatrix.py OUT STOCK awg3|sat S|stock` builds new ones from the live stock (left-multiplies a target
primaries transform; rows renormalised to 1024). Applied AWG3 (`[[801,239,-16],[9,1037,-22],[-4,106,922]]`) = mild
desaturation, matches a mathematically wide-gamut mapping; the R<->B swap proves the registers are live.
Use it for: saturation, hue rotation, channel mixing, primaries conversion (S-Gamut-like, AWG, Rec.2020 -> 709 views),
film-stock-like cross-talk. Limit: linear 3x3 only; no per-hue control (that needs the LUT stage).
Not yet measured: whether the matrix sits before or after the gamma (it is a large-gain matrix [stock off-diagonals
-180..-272] so it is almost certainly in the **linear** domain, before the gamma [?]); whether the three flag
registers (`+0x15500/04/08`) switch an offset/clip/bypass.

### 3.3 Writers to freeze (RAM patch, reversible) **[V for gamma, matrix patched and effective]**
| What | PA (VA-0x40000000) | original bytes | patch |
|---|---|---|---|
| gamma writer | `0x83b67414` | `00 f5 a8 30` | `70 47 a8 30` (`bx lr`) |
| matrix setter | `0x83b675e0` | `70 b5 0c 46` | `70 47 0c 46` (`bx lr`) |
Every setter listed in section 2 can be frozen the same way: write `70 47` (bx lr) over its first halfword with a
32-bit aligned store (keep the 2nd halfword). Check original bytes first; patch only on FW 3.21.
Open: whether the camera rewrites the matrix at events (we patched it before the colour test; the gamma case showed the
camera rewrites at video start, so the same is expected).

## 4. How to find what an unknown register does (protocol)
1. `gammamod -F hwdump <pa> N save.bin` (save original) - writes can always be undone by writing it back.
2. Write a value that is easy to see (gamma: invert; matrix: swap; unknown: set to 0 / max / +-50%) on **one** register or
   group, watch live view with a colourful/high-contrast scene. Do not write several unknown groups at once.
3. If the camera rewrites it, patch the group's setter (table in section 2) first or poll.
4. Record the result here with **[V]**.
Dangerous candidates (still safe to try, battery pull recovers): `0x15d40` (3D-LUT write bracket), `0x3xxx` (RAW side),
`0x11xxx`. Don't write while recording video the first time.

## 5. Not available / limits
* Zero Sony PP/S-Log/knee engine in A6000 av-cam (a6300 has it); everything has to be done by writing these tables.
* Gamma is a 96-knot piecewise curve: fine near black (step 2 in 11-bit linear), coarse near clip (128 steps).
  Anything needing true 12/14-bit log precision in highlights is approximated.
* Output is 8-bit JPEG / 8-bit 4:2:0 video: log looks band; ISP bit-depth after the gamma is 10 bit [?].
* Sensor clip is fixed: no extra dynamic range.
* The ISP writes cannot change RAW stills; RAW is saved before the ISP. [?]

## 6. Phase A static mapping results (av-cam 3.21, no camera needed)
Tools: `tools/isp_regscan.py` (leaf register accessors), `tools/isp_stages.py` (multi-field setters/getters),
`tools/isp_windows.py` (table windows copied with the generic memcpy `0x43b7f878`), `tools/isp_callers.py` (callers + nearby
log strings). Data: `docs/data/isp_stages.tsv`, `isp_windows.tsv`, `isp_stage_callers.tsv`.

1. **Real hardware accessors vs. software fields.** A scan of the whole 11 MB image finds 744 "`add.w r0,r0,#X; ldr/str`"
   leaf functions, but only those fed by the handle->base getter (`0x43acee62` = `ldr r0,[r0]`, 157 call sites, and
   the sibling `0x43ad8880`) are MMIO. The clusters at `0x43ab0000/0x43ad0000/0x43b30000/0x43b40000` are C++ object fields
   (offsets like `0xb14`, `0x1c50`), not registers. Real CC registers are in `0x43b60000..0x43b80000` only. So the register
   list in section 2 / the TSV is essentially complete **for accessor-style registers** (CC block `0x15000..0x15eac`,
   `0x14100..0x14a94`, `0x11118c..0x1190c`, `0x3100..0x3e68` and a far window `0x99a00/0xa1e00..0xa5e60` reached through the
   second base getter).
2. **Table windows (bulk copies) found** (memcpy to/from MMIO):

| fn | dir | window (offset from base) | bytes | what |
|---|---|---|---|---|
| `43b67414` | W | `0x15300` | 0x180 | **gamma** [V] |
| `43b685d8` | W | `0x15b08` | 0x70 | table #2 (curve-like, live `1,0,0,0,0,1,342,683,...`) [?] |
| `43b69b22` | R | `0x15700` | 0xa0 | table #3 readback (live decaying curve) [?] |
| `43b68606` | R | `0x16200` | 0xa80 | big table readback (live packed 27-bit triplets) [?] |
| `43b69f92/9c/a6` | W | `0x3600`, `0x3700`, `0x3800` | 0xf4 each (61 words) | three parallel tables in the `0x3xxx` block (live all 0) [?] |
| `43b6b536` | W | `0x3950` | 0x200 | table (live 0) [?] |
| `43b6b544` | W | `0x3b50` | 0x50 | table (live 0) [?] |
| `43b78afe` | W | `0xa3f00` (second base) | 0x100 | table in the far block [?] |
| `43b7a480` / `43b7a492` | W | `0xa5950` / `0xa5b50` (second base) | 0x200 / 0x50 | tables in the far block [?] |
| `43b67a68` -> `43b67918` | W | `0x17000..0x17800` | 0x654 total in 8 banks | **3D LUT**, bracketed by `0x15d40` [S] |
The `0x16200` and `0x15700` windows only show readers; their writers go through a different path (or DMA) - not yet found.
3. **Sub-blocks (class names from RTTI strings).** CamCore is made of: `CamAif, CamArc, CamCas, CamDcv, CamDev (Develop),
   CamHead, CamNr, CamRdc (+RdcW0/RdcW1/RdcR0), CamSig, Iif, Prc, Sgc1, TestPG`, driven by controllers `CapController,
   CdetController, CompDecompController, CoreController, DevelopController, EEController (edge/detail), NRController,
   SplitCapController, SplitEEController`. The boot IO log (`docs/reference/io_after_boot.txt`) shows the camera touching these
   bases early: `0xF3000000`, `0xF3020000`, `0xF30A1000`, `0xF3100000`, `0xF3120000`, `0xF3212000` + `0xF3215000` (our CC
   block), `0xF3400000/F3402000/F3420000/F3423000`, `0xF3C04000..`. So **detail (EE), NR (CamNr), Develop (CamDev), shading
   etc. are separate hardware blocks at other bases, not in the `0xF3200000` window**; they were not enumerated by the
   accessor scan because they use other base getters. Next: find each class's base (constructor/`GetBaseAddress` path
   `[CAMERA_MAIN_CC]: CamCore GetBaseAddress`) and re-run the scans per base.
4. **Stage naming from callers** (`isp_stage_callers.tsv`): the only string-labelled stage is the **3D LUT** (callers
   `0x4381d2c6` "CamCore 3D-Lut table Write Start/End", `0x4381d3f0` "CamCore Change 3D-Lut"): setters `43b67988` (control, `0x15d00..0x15d44`)
   and `43b697bc` (`0x15e00..0x15e48` mode/change). The `0x4381d..` code is the CC wrapper layer (`CAMERA_MAIN_CC`:
   `SetDevelopParam`, `SetMovDevelopParam`, `SetReviewDevelopParam`, `SetPlayRawDevelopParam`, `Create3DLUT`,
   `LutMode Change SA/CAP/MOV-MON`). Callers in `0x4382a...` are the matching getters, `0x43852..` drives the `0x3500`
   group, `0x43861..` the `0x15a00` group, `0x43865..` the `0x15a40..` setters; none of these functions carry log strings,
   so their meaning is to be established by live diff (Phase B) and write tests (Phase C).
5. **Parameter-set variants** exist per mode: `SetDevelopParam` (still), `SetMovDevelopParam` (movie), `SetReviewDevelopParam`,
   `SetPlayRawDevelopParam`, `SetDevDetectDevelopParam`, each with `[CapMode][ExposureID][ContID][BDROSeq][BDRObase][FrameID]`
   - the camera re-programs a different set of CC registers per mode/frame. This is why movie mode showed another gamma.

## 7. Sub-block bases (hypothesis from constructors) and Phase B snapshots
The CamCore sub-block classes initialise a register handle with a **default base constant** (pattern `ldr r1,[pc]=0xF3xxxxxx;
b 0x43ad48dc`). The constants and their init functions (address order follows the alphabetical RTTI order
Aif, Arc, Cas, Dcv, Dev, Head, Nr, Rdc, ... [?], so the class assignment is a guess):
`0xF3120000` (`436c5bb0`), `0xF3100000` (`436c691c`), `0xF3121000` (`436c71fc`), `0xF300C000` (`436c812c`),
**`0xF3200000` (`436c85a0` = CamDev/Develop; this is the CC block we use)**, `0xF3020000` (`436cd38c`), `0xF3030000` (`436ce694`),
`0xF3040000` (`436ced30`); other bases in code: `0xF3002000`, `0xF3041000`, `0xF30A0000/F30A1000`, `0xF30B0000`,
`0xF3102000`, `0xF3131000`, `0xF31A0000..F31D1000`, `0xF3273000`, `0xF340B000`, `0xF3420000/22000`, `0xF3440000..F3449000`,
`0xF3480000`, `0xF34A0000/B0000/C0000`. The literal tables at `0x43bcc104..0x43bcc460` / `0x43bd2ca4..` list per-block
`{base, base+0x200.., id}` entries (interrupt/DMA bookkeeping). To identify a block: read it with `gammamod -F hwrd` (read-only)
before/after changing a setting (below). Sub-block registers outside `0xF3200000` have NOT been read yet.

### Phase B: live snapshots + diff (read-only)
```sh
CAM=<ip> mods/look/isp_snap.sh std_iso100          # snapshot of CC pages 0x0,0x1000,0x3000,0x5000,0x11000..0x17000
# change ONE setting on the camera, wait for live view to settle, then:
CAM=<ip> mods/look/isp_snap.sh vivid_iso100
python3 tools/isp_diff.py docs/data/snaps/std_iso100.txt docs/data/snaps/vivid_iso100.txt
```
Suggested series (one setting at a time, always compare to the baseline): Creative Style (Standard/Vivid/Portrait/Landscape/
B&W/Sepia), Contrast/Saturation/Sharpness (-3/+3), DRO off/Auto/Lv1/Lv5, NR (high ISO NR off/low/high), ISO 100 vs 6400,
WB (daylight/tungsten/custom), exposure comp, movie mode, video recording. Note: the writers patched earlier freeze gamma/matrix,
so run `look.sh revert` + matrix writer revert first to see the camera's own values.

## 8. Phase B results (live diffs; raw diffs in `docs/data/snaps/diff_*.txt`, snapshots `docs/data/snaps/*.txt`)
Camera state note: snapshots `std`/`vivid` were taken with the camera's own gamma/matrix writers **unpatched** (stock), ISO/scene unchanged
between them, so differences come from the Creative Style change only (+ small scene noise: AE/AWB can move a few words).

### 8.1 Creative Style Standard -> Vivid (1063 words changed)
| Region | Change | Reading |
|---|---|---|
| `0x15300..0x1544c` (gamma) | ~80 of 96 knots change (e.g. knot idx 10: 84 -> 126; idx 15: 259 -> 491...) | **[V] the style rewrites the whole gamma curve** (contrast), confirming the gamma table is the style tone curve |
| `0x1551c`, `0x15520` (matrix row 1) | `1340 -> 1400`, `-272 (65264) -> -332 (65204)`; rows 0/2 unchanged; row sum stays 1024 | **[V] Vivid changes only the G row of the colour matrix (more green saturation)**; other style colour changes are in the LUTs below |
| `0x16000..0x161fc` (128 words) + `0x16204..0x16dfc` (all 12 profile blocks) | rewritten | ~~colour-tone tables of the style~~ **CORRECTED in 8.4: these change with no setting changed (state/scene dependent), not style parameters** |
| `0x17300..0x173ec` (60 words), `0x17800..0x1787c` (32 words) | rewritten | **[V] 3D LUT banks #3 and #8 are used by the style** (the others unchanged) |
| `0x15700..0x1577c` (32 words) | rewritten | **CORRECTED 8.4: also changes in the no-change control (state dependent)** |
| `0x15b88..0x15ba4`, `0x15bbc..0x15bc4` | 7 + 3 words, small changes | parameters near table #2 (`0x15b08`) |
| `0x156a4..0x156c0`, `0x15600` | 9 words | stage `43b693bc/43b69312/43b692bc` group; values 79..510 (gain/threshold-like) |
| `0x15814`, `0x1581c` | `1 -> 0` | two enable flags of stage `0x15800` switch off in Vivid |
| `0x15e4c` | `0xefffc5 -> 0xec57ff` | LUT mode/level word in the 3D-LUT change stage (`43b697bc`) |
| `0x3538..0x354c` | `1339->1336, 1883->1884, 1050->1046`, `0x3544: 33488896 -> 0`, `0x3548: 0 -> 511`, `0x354c` | the `0x3500` group (getter `43b6a186`) - looks like per-style gain/limit parameters |
| `0x00000`, `0x00004` | `16320 -> 16370` | block-0 registers (value ~0x3fc0..0x3ff2, 14-bit full-scale-like) |
| `0x00810`, `0x00910` | `1 -> 0`, `0x10000 -> 0` | block-0 enable-like regs |
So a Creative Style = **gamma (96) + colour matrix (3x3) + hue/sat table set (0x16000..0x16dff) + 3D-LUT banks + a few parameter
registers**, all programmed per style. To clone a style or emulate a look in full, write all of these, not only gamma+matrix.
Open: which of `0x16000/0x16200` blocks are hue-dependent saturation vs. hue shift; whether the 3D LUT banks (value 256 = neutral)
or the `0x16xxx` tables dominate colour.

### 8.2 Creative Style Standard -> B&W (918 words changed; `docs/data/snaps/diff_std_vs_bw.txt`)
Colour matrix (Q10 rows) per style: Standard `[1292,-180,-88 | -44,1340,-272 | -24,-108,1156]`, Vivid `[... | -44,1400,-332 | ...]`,
**B&W = identity `[1024,0,0 | 0,1024,0 | 0,0,1024]`**. [V] So B&W switches the camera colour-correction matrix off (pure sensor RGB with
WB gain) and removes colour through other registers (below), not through the matrix.
Other B&W-specific changes (not in Vivid): `0x00d10: 0 -> 1` (**candidate mono / chroma-kill flag**), `0x156d0: 0 -> 1`, `0x15904: 1 -> 0`,
`0x156ac/b0/bc/c0` thresholds `93/99/79/90 -> 64` (all four equal = four per-channel/per-band gains set to a common value, e.g.
colour-noise-reduction or chroma gain), `0x15814/0x1581c: 1 -> 0` (same as Vivid), table `0x1591c..0x15948` rewritten, 3D-LUT banks
almost unchanged (only 3 words), but the hue/sat tables `0x16000..0x16c7c` are rewritten as in Vivid (values `0x6fb0fd7`/`0x2fb0fd7`
alternate in `0x16c94..0x16d24`: a 1-bit pattern difference in the high bits - a bit-field flag toggled across blocks, likely
chroma enable/limit per block). Gamma changes too (B&W has its own tone curve; head `15,31,51,115,124,109,118,235,...` is noisy/unsorted like
the stock table, it is not simply smoother).
Hypothesis to test (Phase C, write): set `0xF3200D10 = 1` on a colour style to see if colour disappears; set the four `0x156ac..0x156c0` words
to 64 on Standard and watch.

### 8.3 Standard, Saturation 0 -> +3 (1038 words changed; `docs/data/snaps/diff_std_vs_sat3.txt`)
* **Gamma (`0x15300..`) unchanged, colour matrix (`0x1550c`) unchanged** [V]: saturation is NOT done with the matrix or the tone curve.
* Changed: 3D-LUT banks **#3 (`0x17300`, 60 words; see correction 8.5) and #8 (`0x17800`, 31 words)** (same banks as Vivid) [V]; hue/sat tables `0x16000..0x16dff`;
  table `0x15700`; parameter block `0x15b20..0x15bc4`; `0x15c10`.
* Four values that scale with saturation [V, strong]: `0x156ac / 0x156b0 / 0x156bc / 0x156c0` = **`93 / 99 / 79 / 90` (Standard sat 0) ->
  `192 / 200 / 109 / 124` (sat +3)**, and `64 / 64 / 64 / 64` in **B&W**; plus `0x15600: 151 -> 655`. Interpretation (hypothesis): four chroma-gain
  bands; B&W = floor. Write test: on Standard write `0x156ac..` to 192/200/109/124 and compare with Saturation +3 on screen.
* `0x3538 / 0x353c / 0x3540` = `1339 / 1883 / 1050 -> 1707 / 2210 / 1381` (+27% / +17% / +32%): per-channel chroma/limit parameters of the `0x3500` group.
* `0x00a10/0x00b10/0x00c10: 0 -> 1`, `0x00410/0x00d10: 0 -> 0x10000`, `0x00038: 0x10101 -> 0x101`, `0x00810/0x00910` `1/0x10000 -> 0`: block-0 per-style
  enable/select flags (they differ between all three styles: block 0 is the **style/mode select block**).
* Contamination warning: `0x11400/0x1140c/0x11900/0x11908` and their mirrors `0x14200/14204/14400/1440c/14900/14908` change between *any* two
  snapshots (value pairs ~`24268/23271/788/865`): those are AE/AWB measurement/gain readbacks that follow the scene, **not** style
  parameters. Ignore them when diffing; `0x1433c/0x14344` also moved slightly.

### 8.4 Control experiment (nothing changed) and corrections - READ THIS before trusting 8.1-8.3
`std2` = snapshot of the same Standard style (contrast 0, sat 0) taken later with no settings changed. Words that differ per region
(`docs/data/snaps/*.txt`, counts from `tools/isp_diff.py`-style comparison):

| pair | gamma | matrix | 0x156xx | 0x15700 | 0x15b | 0x16000-16dff | 3D bank3 `0x17300` | 3D bank8 `0x17800` | 0x3500 | block 0 |
|---|---|---|---|---|---|---|---|---|---|---|
| **std ~ std2 (control)** | **0** | **0** | 7 | 32 | 21 | **870** | **0** | 17 | 6 | 7 |
| std2 ~ contrast+3 | 77 | 0 | 7 | 32 | 21 | 826 | 60 | 15 | 5 | 4 |
| std2 ~ saturation+3 | **0** | 0 | 8 | 31 | 21 | 849 | 60 | 16 | 6 | 7 |
| std2 ~ Vivid | 69 | 2 | 9 | 31 | 21 | 847 | 60 | 15 | 5 | 4 |
| std2 ~ B&W | 74 | 9 | 9 | 31 | 21 | 871 | 3 | 17 | 6 | 5 |

Conclusions [V]:
* **Noise / state-dependent (changes with nothing changed): `0x16000..0x16dff` (870 words), `0x15700` table, `0x15b00` block, 3D bank #8, `0x3500`
  group, block-0 words, `0x156xx` small drift, `0x15814/0x1581c` flags.** These are re-computed from scene/AE/AWB/ISO state (and
  the `0x11400/0x14200/0x14900` readbacks) - NOT style parameters. They cannot be used to identify a setting; to write a look they probably
  must be left to the camera (or frozen as a set) [?].
* **Real style parameters (0 changes in control): the gamma table, the colour matrix, 3D-LUT bank #3 (`0x17300`, 60 words).**
  * Contrast +3 -> gamma (77 knots) + bank #3. Matrix untouched.
  * Saturation +3 -> bank #3 (60 words) + `0x156ac/b0/bc/c0` + `0x15600` (655); gamma and matrix untouched. Saturation uses the **3D-LUT bank #3
    (a chroma/hue-vs-luma table) and the four `0x156xx` gains**, not the matrix.
  * Vivid -> gamma + matrix G row + bank #3 (+ the same four gains region).
  * B&W -> identity matrix (+ gamma changes, bank #3 only 3 words): B&W bypasses the matrix; chroma is killed by the `0x156xx`/`0xd10` flags [?].
* So the **minimum register set for a full style = gamma `0x15300` + matrix `0x1550c` + 3D-LUT bank #3 `0x17300..0x173ec` + the saturation gains
  `0x156ac/b0/bc/c0` (+ `0x15600`) + block-0 select flags `0x00a10/b10/c10/d10/038/410`** [?, flags unverified].
* Correction to 8.1/8.3: the `0x16000..0x16dff` tables and `0x15700` are **not** style-dependent; ignore the earlier "colour tone tables" reading.
* Method lesson: always take a **control snapshot (same state, twice)** and diff against it; compare against the *settled* later baseline (`std2`), not the first one.

### 8.5 Sharpness +3 and corrections (snapshot `std_sh3`, diff vs control `std2`)
* Real changes (not noise): **`0x15c14: 12 -> 14`, `0x15c18: 12 -> 14`, `0x150c0: 0 -> 1`**, plus the same style-flag words. Gamma, matrix, `0x156ac..c0` unchanged.
* **Write test [V]:** with the camera's own setter `43b68c88` frozen (PA `0x83b68c88`, original bytes `70 b5 0d 46`, patch `70 47 0d 46`),
  `gammamod -F hwwr 0xF3215C14 <2 words>` with value 0 made live view **visibly softer**; the registers are **4-bit** (writing 60 read back 12).
  The camera rewrites these continuously when the setter is not frozen (a 20 s polling hold counted ~1000 overwrites); a value of 15 looked
  sharper but was not conclusive until the setter was frozen. Sharpness range appears to be 0..15 (Standard 0 = 12?, i.e. the register holds
  `12 + sharpness` or similar; -3..+3 map to roughly 9..15 [?] - measure by snapshots at -3/+3 if needed).
* **Correction (bank #3):** `0x17300..0x173ec` (60 words) is **one value replicated 60 times**: `0x1e7046` on stock Standard, `0x1e7146` on
  contrast+3 / saturation+3 / Vivid / sharpness+3, `0x1e7046` + `0x7f7946` on B&W. A single bit (`0x100`) flips when any style parameter is non-default.
  It is a flag/identity-LUT word, not tone data; the 3D LUT is therefore **neutral (identity) in these styles** - style colour comes from
  matrix + `0x156xx` gains, not from the 3D LUT. Bank #8 (`0x17800`) varies without setting changes (noise).
* Block-0 words (`0x00a10/b10/c10/d10/0910/0038/0410/0810`) flip between snapshots even in the control, so they are state flags, not style selectors.
* Parameter map so far (verified): sharpness = `0x15c14/0x15c18`; saturation = `0x156ac/b0/bc/c0` (+`0x15600`) [write test pending];
  contrast = gamma table; colour mode = matrix; Vivid = gamma + G-row of matrix + gains; B&W = identity matrix + `0x156xx = 64`.

### 8.6 Saturation write test [V]
Standard style, saturation 0. Setter `43b69312` frozen (PA `0x83b69312`, original `70 b5 0d 46`, patch `70 47 0d 46`), then
`gammamod -F hwwr 0xF32156AC` with six words `[g0, g1, 478, 470, g2, g3]` (`0x156b4/b8` left at stock):
* `192, 200, 109, 124` (the camera's own values for Saturation +3) -> live view **very saturated** (confirmed by owner).
* `64, 64, 64, 64` (the camera's B&W values) -> live view **very desaturated** (confirmed by owner).
* Stock Standard (sat 0) = `93, 99, 79, 90` (they drift by +-1 with the scene). So **`0x156ac/b0/bc/c0` are four chroma gain bands**; 64 = near-grey end, ~95 = neutral,
  ~200 = strong. Unknown: exact band meaning (likely Cb/Cr gain pairs, or per-luma/per-chroma bands), whether 0 gives full mono, and whether the
  scale is linear (probably gain/64 [?]). `0x15600` (151..655) co-varies with saturation (candidate: global chroma limit/clip) [?].
  Hence a **direct saturation control without touching the matrix or gamma** exists: independent of Creative Style and look.
* Hold method: freeze the setter and write once; no polling needed.
Verified independent controls so far (all RAM-only, camera-own setters frozen to keep them):

| Control | Registers | Setter to freeze (PA = VA - 0x40000000) | Stock Standard | Range seen |
|---|---|---|---|---|
| Tone curve / contrast | `0x15300` (96 words) | `43b67414` | style table | any curve |
| Colour matrix | `0x1550c` (9 words, s16 Q10) | `43b675e0` | `1292,-180,-88,-44,1340,-272,-24,-108,1156` | any, rows sum 1024 |
| Saturation (chroma gain) | `0x156ac,b0,bc,c0` | `43b69312` | `93,99,79,90` | 64 (grey-ish) .. 200 (very saturated) |
| Sharpness | `0x15c14,0x15c18` (4-bit) | `43b68c88` | 12 | 0 (soft) .. 15 |

* **`0x156ac/b0/bc/c0` = 0 -> fully black-and-white [V]** (owner: "now is b&w"). `64` left visible colour (desaturated), so the scale is **0 = mono, ~64 = weak colour,
  ~95 = neutral (Standard), ~200 = very strong**: roughly linear chroma gain (gain ~ value/95) [?]. B&W style itself uses 64 for these four plus the identity matrix
  and `0xd10`, so its mono comes from a further stage (e.g. chroma clamp), irrelevant for custom looks: **set the four gains to 0 for mono**.

### 8.7 DRO (snapshots `dro_off`, `dro_lv5`, controls `std`/`std2`; Standard style, sat/contrast/sharpness 0)
Region counts (words changed): `dro_off ~ dro_lv5`: gamma **0**, matrix **0**, `0x15c00` 0, bank #3 8, bank #8 32, `0x16xxx` 793 (noise), `0x15700` 32 (noise).
* **DRO is not applied through the gamma table or the colour matrix** at this level [V]. (The Bionz DRO/BDRO stage is a separate local-tone-mapping
  block; it uses the `[BDROSeq][BDRObase]` frame parameters seen in the `SetDevelopParam` log strings.)
* Changed non-noise registers: **3D-LUT bank #4 (`0x17400..0x174bc`, 48 words) takes three different values in three DRO states** [V]: `0x7f7fff` (`8355711`)
  in the earlier Standard snapshots (`std2`: the camera default DRO state, probably Auto/off-equivalent), `0x273 69` (`2570601`) with DRO off,
  and `0x273 69 - 0x100` (`2570345`, with a few `8339067`) with DRO Lv5. These are *flag-like replicated words* (like bank #3), **not** a table, so they
  mark a mode, they do not shape the image [?]. Also `0x15600: 852 -> 0`, `0x15604: 68 -> 0`, flags `0x15814/0x1581c: 0 -> 1`, `0x15a00: 0 -> 3`, `0x15e4c`
  (LUT-mode word) changes. Block-0 words `0xb10/0xc10/0xd10` flip as in the control (state flags).
* Conclusion: the registers that carry the DRO *effect* are **not** in the `0xF3200000` pages 0x0/0x1000/0x3000/0x5000/0x11000-0x17000. The BDRO block lives at another
  base (see 7) and would need its own snapshot. The CC block here only shows the mode flags.
* Because DRO and the global gamma are independent, **a custom gamma look is not disturbed by DRO**; keep DRO Off while using a custom look so the local tone map doesn't alter it.

## 9. Other blocks: all readable, DRO found (snapshots `*.blocks.txt` via `mods/look/isp_snap2.sh`)
**Safety result [V]:** every candidate base from section 7 can be read with `gammamod -F hwrd` without hanging the bus: `0xF3000000`,
`F3002000`, `F300C000`, `F3020000`, `F3030000`, `F3040000`, `F3100000`, `F3102000`, `F3120000`, `F3121000`, `F30A0000/1000`, `F3440000/1000`,
`F3445000`, `F3446000`, `F34A0000` (first page of each, 4 pages for the first six). `isp_snap2.sh NAME` snapshots all 35 pages (35,840 words, ~1 min).
Method: `dro_off` vs `dro_lv5` (262 words differ) minus a control `dro_off2` (same state, 233 words differ = noise). Noise concentrates in `0xF3021000`
(193 words: AE/AWB/histogram statistics, follows the scene) and `0xF30A0000`, `0xF3031000`, `0xF3441000`, `0xF3003000`.

### 9.1 DRO (BDRO) [V, DDR-buffer-based]
45 words change with DRO and are not noise:
| Address | off -> Lv5 | Reading |
|---|---|---|
| `0xF3040100`, `0xF3040108` | `0 -> 1`, `0 -> 1` | **enable flags of block `0xF3040000`** (DRO / BDRO hardware path) |
| `0xF3040110` | `0 -> 0x10000` | enable/mode |
| `0xF3040120` | `0 -> 10486186` (`0xa0012a`) | size/config word |
| `0xF3040128 .. 0xF3040138` | `0 -> 0x93xxxxxx` ascending: `2466390016, 2467004416, 2467158016, 2467204096, 2467219456` | **DMA buffer base addresses in DDR (`0x9xxx_xxxx`)**: histogram/gain-map buffers for the DRO local tone map |
| `0xF304013c`, `0xF3041004` | `0 -> 31`, `0 -> 48` | counts/size |
| `0xF3002284..0xF30022a0`, `0xF3002404..0xF3002410`, `0xF3002480` | `0 -> 0x80000000 / 0x40002 / 0xc01d0e00 / 0x10000 / 0x60000000` | clock/DMA-path enables in block `0xF3002000` |
| `0xF3000038`, `0xF3000410`, `0xF3000c10`, `0xF3000d10` | flags | block-0 state flags (also flip in the control for some) |
| `0xF3020280/0290/0c00`, `0xF3030124/164/280`, `0xF3003540` | small config words | related stages |
So **DRO is a separate hardware stage at `0xF3040000` fed by DDR buffers**: the camera computes the DRO curve/gain map in software and the hardware applies it. The
effect is therefore not a register table in the CC block; to remove or replace DRO, keep the enable flags `0xF3040100/108/110` at 0 (patch their setter) or edit the
DDR gain buffers (addresses in `0xF3040128..138`) [?]. For custom looks: **DRO Off** (flags 0) is the clean state.
Data: `docs/data/snaps/diff_blocks_dro_off_vs_lv5.txt` (raw, includes noise), `*.blocks.txt`.

### 9.2 Noise reduction, first attempt (high-ISO NR Off -> Normal at the scene's ISO; long-exposure NR Off; snapshots `nr_off`, `nr_normal`)
Total 821 words differ; after removing words that also changed in the no-change controls (`std`/`std2`, `dro_off`/`dro_off2`) **76 remain**. Mostly flag
words that flip with any state change (`0xF3000810/910/a10/b10/c10/d10`, bank words `0xF32172xx..0xF32174xx`, block-0 style flags), so the NR signal is weak here.
Candidates that are not obviously state flags: **block `0xF3030000/0xF3031000`**: `0xF3030080: 15 -> 13`, `0xF3030164: 0 -> 0x2450000`, `0xF3030280: 8288 -> 8544`,
`0xF3031180..90` (five DDR-looking addresses `0x93xxxxxx`, changing buffer assignments), `0xF30311a0..0xF30311e0` (three parameter sets of 8 words each, e.g. `273944718 -> 384177965`,
`16842801 -> 16777274`) and `0xF3003540/48`, `0xF3020280/2a0`, `0xF3100038: 256 -> 65792`. Interpretation [?]: `0xF3030000` is the **NR / frame-buffer (CamNr?) block** (DMA address
tables and per-frame NR parameter sets), but at low ISO the camera barely changes its parameters, so NR *strength* registers are not visible yet.
Inconclusive; to see NR strength registers repeat at **ISO 6400**: (a) ISO 6400, high-ISO NR Off, (b) ISO 6400, high-ISO NR High, diff each against a no-change control.

### 9.3 Noise reduction at ISO 6400 (snapshots `iso6400_nroff`, `iso6400_nrnormal`; A6000 offers only Low/Normal high-ISO NR) - negative result
NR Off -> Normal at ISO 6400: 811 words differ, **52** after removing control noise, and they are the same class as in 9.2: flag words (`0xF3000c10`, `0xF3200410/d10`),
the ping-pong buffer-address words (`0xF3031180..90`, `0xF3441320/3a0`, `0xF30213xx`) and a few small values (`0xF3030080 13<->15`, `0xF3030280`, `0xF3003540`). No register shows a
strength-like change in any scanned page. Comparing ISO (nr_off, scene ISO) with ISO 6400 shows the **`0xF30311a0..e0` parameter sets and `0xF3030080/124/280`** moving (so the
`0xF3030000/0xF3031000` block does track ISO), but those same words also move between "NR off" and "NR normal" at fixed ISO only as buffer/frame state.
Conclusion [?]: the A6000's high-ISO NR is **not controlled through a visible register in the scanned pages**; it is probably a software/CPU stage (`KKNR`/`CCNR`/`LFNR` code in av-cam) or uses
tables in DDR. Next step for NR would be static RE of the NR controller (`Camera::CamCore::NRController`, `CamNr` class base) rather than more snapshots.
Control-based scan summary: **found and verified**: gamma, matrix, saturation gains, sharpness; **found (not writable-tested)**: DRO enable `0xF3040100/108/110` + DDR buffers `0xF3040128..138`;
**not found**: NR strength, black level, WB gains (white-balance snapshots not taken), detail beyond `0x15c14/18`.

## 10. White balance gains [V] (snapshots `wb_day`, `wb_inc`; write test)
**Correction:** the words `0xF3211400`, `0xF321140C`, `0xF3211900`, `0xF3211908` (and their mirror at `0xF3214400/0C/900/908`) that sections 8.3-8.4 listed as "AE/AWB readbacks"
are the **white-balance channel gains** (Auto WB makes them follow the scene, which is why they differed between any two auto snapshots).
Fixed presets (same scene): **Daylight: `0x11400=22507, 0x1140c=25894, 0x11900=941, 0x11908=441`; Incandescent: `26003, 21594, 436, 805`**.
Write test (stock matrix untouched, WB setter `43b665b6` frozen: PA `0x83b665b6`, original `70 b5 0c 46`, patch `70 47 0c 46`):
* `0xF3211900 = 1900` (from 436) -> **image turns red** => `0x11900` = **R gain**. (Incandescent lowers it, Daylight 941, because tungsten light is reddish.)
* `0xF3211908 = 1900` (R reset to 941) -> **purple** => `0x11908` = **B gain** (R + B up with G unchanged = magenta/purple).
So `0x11900` = R gain, `0x11908` = B gain (scale unknown, unity probably near `~512..1024` for G-equal [?]); `0x11400/0x1140c` (22507/25894 day, 26003/21594 inc) move opposite to each other
and probably are a second representation of the same gains (R/B in another scale) or the G-normalised pair [?]. The setter `43b665b6` writes 6 u16 fields at `0x11400..0x1190c` (reader `43b6661e`).
WB is applied **before the colour matrix**: the matrix `0xF321550C` is identical for Daylight and Incandescent (checked), i.e. matrix = fixed sensor-to-working-space, WB = separate per-channel gain stage.
Use for looks: **per-channel RGB gain (tint / split colour casts / cross-process base) independent of matrix and saturation**; for a neutral look keep the camera's own AWB/preset values and only
add a tint by scaling `0x11900/0x11908` relative to the current pair.

# RE: display scan-out (VPH display controller, AIF DMA, vsync IRQs, Rigel → VDF path)

Static RE of firmware 1.10 for the `sony-a5000` QEMU machine. This doc follows `RE_DISPLAY_GPU.md`, whose §4.3 left the
scan-out block unidentified. That gap is now closed. Sources:
`av-cam.bin` (VA 0x43541000; IDA db `C:\a5000\emu\vdf_avcam.i64`, a copy of `gfx_avcam.bin.i64`), `re/ida/libObj.so.c`,
`re/ida/gfx/android_gralloc.diadem.so.c`, and `emulator/docs/runtime/io_after_boot.txt`.
Claims marked **[?]** are inferred and not yet proven.

New artifacts (all in `re/ida/gfx/`):
* `vdf_vph_driver.c`: Hex-Rays output for the whole VPH driver, 0x438E0000–0x43910000 and 0x43ABE000–0x43AC4000
  (2653 funcs). Single-field register accessors are renamed `W_<off>_<mask>[s<shift>]` / `R_<off>`, where off is relative to 0xF3000000.
* `vdf_vph_regs.txt`: every VPH register and bit field found through the 449 accessor functions, as `reg  W<func>[mask>>shift]`.
* `vdf_aif.c`: the AIF DMA driver (0x43895000–0x43896800).
* `vdf_q.py` / `vdf_ctx.py` / `vdf_parse_acc.py` / `vdf_rename*.py`: IDA query runner and the post-processing scripts.

---------------------------------------------------------------------------------------------------
## 0. Summary (read this first)

```
 im.elf / libObj (Linux)                       av-cam.bin (µITRON, VA 0x4354_1000)                 hardware
 ────────────────────────────                  ─────────────────────────────────────                ─────────────────────────────
 Rigel WS composes windows (GPU)               VDF  (VdfInputCmd*, VdfDisplayCmd*, OSD/YUV pins)
 dmpNative::FbContainer::gen_struct_for_update   │   builds VPH_CMD_UPDATE_OSD / _UPDATE_YUV / _SET_MODE
 vdf_if_port_send_update(rec[≤2])  ── osal msg ─▶│   (VdfInputVphDrvCall.cpp, VdfDisplayVphDrvCall.cpp)
   (OSDPin, osal id 0x80003B, type 5)            ▼
                                               VPH driver (0x438E..0x4390, 0x43AB..0x43AC)          VPH  = display controller
 camera pipeline (YC, live view) ─ YUV pin ─▶    shadow config, applied every vsync by VPH task     regs 0xF3440000–0xF3446FFF
                                                 sub_43904274 (waits flag 10)  ─────────────────▶   AIF DMA ch 51–58 @0xF3441200+
                                               ISR GIC 101 / 102 (vsync), GIC 79 (VPH misc)  ◀────  panel out  (0xF3445xxx) → LCD/EVF
                                                                                                    line  out  (0xF3446xxx) → HDMI/TV
```

* **The display controller is "VPH"** at **PA 0xF3440000** (16 KiB+, sub-blocks up to 0xF3446FFF). It has two outputs:
  **panel** (LCD/EVF, internally "sel 1") and **line** (HDMI / analog TV, "sel 0"). Each output has a **YUV video layer**
  and an **OSD layer**. The bus-QoS debug dump `sub_43769D84` lists `[VPH] r0..r6 w0`, that is **7 read DMA ports + 1 write port**.
  These are exactly AIF channels 51–57 (read) and 58 (write) (§2).
* **Frame-buffer addresses are not VPH registers.** They are programmed into the **AIF DMA channel** registers at
  `0xF3441200 + 0x80·k` (k = 0..7, AIF ch 51..58): start address +0x20, line bytes +0x24, line count +0x2C, pitch +0x48.
  The layer/format/position live in VPH proper (0xF3440100–0xF34404FF) and in the per-output blocks 0xF3445000/0xF3446000.
* **Layer → DMA channel:** panel YUV = **ch51**, line YUV = **ch52**, panel OSD = **ch53**, line OSD = **ch54**,
  ch55 (read) + ch58 (write) = memory-to-memory pass [?], ch56/57 unknown [?].
* **OSD pixel size** is set by **F3440404 bit0** (panel) / **F3440414 bit0** (line): 1 = 4 B/px (32-bit RGBA from the Rigel
  window server), 0 = 2 B/px. Live view is YUV through ch51. Y/C addresses come from the VDF YUV pin (the camera pipeline), not from Linux.
* **Vsync IRQs:** **GIC ID 101 → `sub_43AC0F4E`** and **GIC ID 102 → `sub_43AC0FFC`**, one per output. **79 → `sub_43904228`** (VPH status at
  0xF34400A0). Both 101 and 102 must be raised periodically: the VPH task blocks on them right after init, which is exactly why
  only `0xF3440028 = 0x111` appears in `io_after_boot.txt` (§4).
* **Commit:** software shadowing. The VPH task keeps a 3088-byte pending config per output (`dword_43E07390` → `byte_43E08BB0`)
  and re-applies it **after every vsync**. It writes the VPH registers, sets the layer-enable bit (F3440144 / F3440184) and kicks the AIF channel
  (ctrl bit0). No "shadow-update" hardware bit was found. The registers are probably double-buffered at vsync in hardware [?], so the emulator should **sample
  all state at its vsync tick**.
* **Linux never touches VPH.** im.elf / libObj / gralloc reference no 0xF344xxxx address. Rigel buffers reach the display
  only as **OSD image records** sent to VDF by osal message (§5).

---------------------------------------------------------------------------------------------------
## 1. VPH register block 0xF3440000 (map)

All accesses are 32-bit read-modify-write of bit fields, done by one generated accessor per field at 0x43904000–0x4390C000 and
0x43AC0xxx–0x43AC3xxx. The full field list is in `vdf_vph_regs.txt`. Roles were inferred from the callers. Offsets are from 0xF3440000.

| range | role | evidence |
|---|---|---|
| 0x000–0x00C | 4 regs, bit0 + bit8 each (start/run per output) [?] | `sub_438E5EF0`/`sub_438E5AD4` (output start/stop): set 0x000/0x008 bit0, clear 0x004/0x00C bit0 → **set/clear pairs** [?] |
| 0x010 | bit0, bit8: output-0 / output-1 sync mode [?] | `sub_438E5920`, `sub_438E5EF0` |
| 0x028 | **clock/reset enable**, written once in init with the value from `ddl_clkctrl(10, {0xC0004,0x300010})` = **0x111** | `sub_438E8990`. Also the only VPH write in `io_after_boot.txt` |
| 0x038 | 16-bit **debug monitor pins**: bit9 = in ISR101, bit10 = in ISR102, bit11 = in VPH task | `sub_438E4940` |
| 0x040–0x05C | panel-side interrupt / sync-source configuration (values such as 0x30033, 0x70077, 0x10000) | `sub_438E98F0`, `sub_438E9ADC`, `sub_438E9B34(1,..)` |
| 0x060–0x07C | same for the line side | `sub_438E96F4`, `sub_438E9B34(0,..)` |
| 0x0A0 (R) / 0x0A4 / 0x0A8 | VPH misc IRQ status / ack-or-mask / enable (init 0x30000). ISR79 tests bits 16, 17 | `sub_43904228`, `sub_438E8854` |
| 0x0C0–0x0CC | set/clear pairs (0x10001 / 0x10000) [?] | `sub_43ABFA24` |
| 0x100 | global control (written at first vsync) | `sub_438E8854` |
| 0x108 R bit4, 0x114/0x118/0x11C/0x120 R | **status**: per-output field / active flags read inside the vsync ISRs (`sub_43ABF87E(n)`: 0→0x118, 1→0x114, 2/4→0x120) | |
| **0x140–0x16C** | **panel input stage** (11-bit fields) | |
| 0x140 | bit20, bits24+ (mode) | `sub_43AC2DB2` |
| **0x144** | **panel layer control**: **bit0 = YUV layer on**, **bit1 = OSD layer on** [?], bits4-5, 8-9 (`sub_43AC2CC8`), bits24-25 | `sub_43AC0B94(1,on)`, `sub_438F93F0` sets bit0 and then starts ch51. The OSD update sets bit1 and then starts ch53 (`sub_438F4F9C`) |
| 0x148 | 2×11 bit (YUV source size) [?] | `sub_43AC2CE2` |
| **0x14C / 0x150** | **OSD active window** (x0,y0) / (x1,y1) on the panel raster, hi16/lo16 [? which half is H] | `sub_43AC2D0E` from `sub_438F4838` |
| 0x154, 0x158, 0x15C | YUV input format flags (12 single-bit flags in 0x15C) | `sub_43AC2D32` |
| 0x160–0x16C | second (chroma) plane geometry [?] | `sub_43AC2D86`, `sub_43AC2DB2` |
| **0x180–0x1AC** | **line input stage**, same layout as 0x140 with 12-bit fields. **0x184 bit0 = line YUV on** | `sub_43AC0B94(0,on)`, `sub_438F90CC` (then starts ch52); `sub_43AC2E48` = 0x18C/0x190 OSD window |
| 0x1C0–0x1EC | third layer (panel only), 0x1C4 bit0 on/off [?] | `sub_43AC0BAC` |
| 0x200–0x2D8 | 4 units of byte fields: background / border colours [?] | |
| 0x300–0x3D0 | **4 colour-space-conversion units**: 3 regs of 2×16-bit coefficients, 3×7-bit offsets, enable at +0x10 | accessors at `0x439080C8…`. Init sets all four enables (`sub_438E8854`) |
| **0x400–0x43C** | **panel OSD**: **0x404 bit0 = 32 bpp (1) / 16 bpp (0)**, 0x408 bit0 = field/interlace flag (desc+28), 0x428 (2×10/11 bit), 0x42C = OSD lines (desc+14) | `sub_438F4838` (a1=1) |
| **0x410–0x45C** | **line OSD**: **0x414 bit0 = 32/16 bpp**, 0x410[18:16] = line video mode index 0–5 and 0x410[2:0] = NTSC/PAL [?], 0x448, 0x44C = lines | `sub_438F4838` (a1=0), `sub_438F9B20` (720×240 / 858×525 = 480i), `sub_438F9F04` (720×288) |
| 0x460–0x4C8, 0x500–0x508, 0xA00–0xA08 | more layer geometry; 0x500/0xA00 bit24 = output run [?] | `sub_438E5AD4` |
| 0x700–0x77C | 20 × (x,y) 11-bit pairs + colours: hardware frame boxes (AF / face frames) [?] | `sub_43AC1650` |
| 0x1000–0x15FF | **AIF DMA for VPH** (§2) | |
| **0x5000–0x5FFF** | **panel output** | |
| 0x5000–0x5060 | panel timing generator (10/11/12-bit H/V totals, sync, active) [?] | `vdf_vph_driver.c` @43081.. |
| 0x5100–0x51E8 | panel picture processing: 0x51AC–0x51E4 = 16-step tone LUT [?], peaking, zebra, colour temp. (VDF `VphSetZebraCall/VphSetPeakingCall`) | |
| **0x5500–0x553C** | **panel OSD resizer**: 0x5504 (2×13-bit size), 0x5510 (2×15-bit ratio), 0x5514, 0x551C (phase), 0x5538/0x553C (mode / blend bits from desc+24) | `sub_43AC19D8`, `sub_43AC1A78` |
| 0x5600–0x5684 | 12 boxes × (3-bit colour, x, y) + 8 × 24-bit colours (OSD frame boxes) [?] | |
| **0x6000–0x6FFF** | **line output** (HDMI/TV), same structure: 0x6000 timing (12-bit), 0x6100 encoder, 0x6228/0x622C encoder control, 0x6500 OSD resizer (`sub_43AC16B6`), 0x6600 boxes, 0x6900 DAC/levels [?] | |

The panel/line split is proven by the OSD-update code (`sub_438F4838`): output a1=1 writes 0x404/0x42C/0x14C/0x5504 and DMA
ch53, while a1=0 writes 0x414/0x44C/0x18C/0x6504 and ch54. It is confirmed by the line-only analog modes
(720×240 @ 858 total and 720×288) written to 0x410/0x6100. Line fields are 12 bits wide (HDMI 1080 needs H-total 2200), panel fields 11 bits.

---------------------------------------------------------------------------------------------------
## 2. AIF DMA channels (the frame-buffer addresses)

The generic "AIF" DMA driver is at 0x43895C64… (`[AIF%d]BAD startadrs`, `ofs0..3`, `rptadrs1..3`, `stop TMOUT`). It is shared by every
image block. The channel table at **0x43B278D4** holds 12 bytes per channel: `{group_reg, ch_reg, u8 dir(0=read,1=write), u8 nibble_idx}`.
VPH channels:

| AIF ch | ch regs | dir | use |
|---|---|---|---|
| 51 | 0xF3441200 | R | **panel YUV** (live view / playback image) — `sub_438F6CF4(a1=1)` |
| 52 | 0xF3441280 | R | **line YUV** — `sub_438F6CF4(a1=0)`, started in `sub_438F90CC` |
| 53 | 0xF3441300 | R | **panel OSD** — `sub_438F4400(a1=1)`, started in `sub_438F4F9C` |
| 54 | 0xF3441380 | R | **line OSD** — `sub_438F4400(a1=0)` |
| 55 | 0xF3441400 | R | YUV mode a2=1 source, paired with ch58 [?] |
| 56 | 0xF3441500 | R | ? |
| 57 | 0xF3441580 | R | ? |
| 58 | 0xF3441480 | W | VPH write-back (w0) [?] |
Group registers: 0xF3441000 (one nibble per channel, written by `sub_43896442`), **0xF3441004 = error status** (ISR `sub_4390B2A8`
tests `& 0xEEEEEEEE`; also 0xF3442004/0xF3443004 `& 0xE`). Keep them 0.

Per-channel registers (offsets from the channel base, taken from `sub_43896374/438964EE/4389653A/43896588…/43895E14`):

| off | meaning |
|---|---|
| +0x00 | **ctrl**: **bit0 = start/busy** (start = OR 1; the start is refused while bit0 is still 1, so hardware must self-clear it at end of frame), bits1-2 = mode (OSD uses 1), bit3, bit4, bit5 (write ch), bit11 + bits16-19 = ring/IIB mode, bit13 cleared |
| +0x04 | **bit31 = running** (stop polls it until 0, ≤21 sleeps), bit30 = stop request, bit0 |
| +0x08 | bit31 set after stop |
| +0x0C | write channels only |
| +0x1C | bit14 → alignment (set: 2-byte, clear: 8-byte, for start and size) |
| **+0x20** | **start address** (guest-physical; OSD = buffer + bpp·(y·stride + x)) |
| **+0x24** | **bytes per line** (OSD: (width rounded up to 4 px + pad) · bpp, at least 0x380) |
| +0x28/+0x2C | ofs0 (u32 offset) / **count0 (u16) = lines − 1** |
| +0x30/34, +0x38/3C, +0x40/44 | ofs1..3 / count1..3 (4-D address generator; the YUV path uses them to interleave the Y and C planes) |
| **+0x48** | VPH channels: **[15:0] = line pitch in bytes (signed; negative = vertical flip)**, [31:16] = field offset — `sub_43AC11B8` |
| +0x4C | [30:0] extra skip offset (interlace) — `sub_43AC1174` |
| +0x50 | repeat mode, +0x54..0x5C rptadrs1..3 (alternate buffers) |
| +0x70 | bit0 — `sub_43AC11DE` |

OSD geometry (`sub_438F4400`, disassembly @0x438F444C): bpp = 4 if desc+16 else 2; vdec = 1, or 2 for some panel types
(the 640×480 OSD is shown on a 240-line panel by line skipping [?]); pitch = vdec·bpp·stride_px; count0 = lines/vdec − 1.

---------------------------------------------------------------------------------------------------
## 3. Layer descriptors (what VDF hands the VPH driver)

**VPH_CMD_UPDATE_OSD** (built by VDF `UpdateOsdPrmGen` `sub_43660D44`, consumed by `sub_438F4F9C` (panel) / `sub_438F503C` (line)):
`+0 u32 addr, +4 u16 stride_px, +6 u16 h, +8 u16 x, +10 u16 y, +12 u16 width_px, +14 u16 lines, +16 u32 is32bpp,
+20 u32 mode (1 normal, 2/3 = 3-D / half modes, 4 = shrink ×7/8) [?], +24 u32 blend (1|2 → 0x553C), +28 field, +32 vflip, +40 panel class,
+44 id, +48 done-callback, +52 → 0x5500/0x6500 bit8`.
Default when there is no OSD: `sub_438F4F48`, 640×480 at address 0.

**YUV input** (`sub_438F6CF4`): `+0 Y addr, +4 Y stride_px, +8 x, +10 y, +12 w, +14 h, +16 C addr, +20 C stride, +24/+26 C x/y,
+32 fmt (0 → 2 B/px packed 4:2:2; ≠0 → 1 B/px Y plus separate C plane, 128-byte C bursts interleaved through ofs1..3), +60 vflip, +64 flags`.
In live view the addresses come from the camera YC pipeline through VDF `VdfInputCmdUpdateYuv(Pin)` / `VphUpdateYuvCall` (`sub_43661BC8`).
Linux is not involved.

---------------------------------------------------------------------------------------------------
## 4. Interrupts, VPH task, commit

ISR table at **0x43E94D04** (entries `{attr, intno, 0, handler}` with stride 16; `sub_4355D8FC` = ena_int). VPH entries:

| GIC ID | handler | role |
|---|---|---|
| **101** | `sub_43AC0F4E` | vsync of output index 0 (`R 0x118`, monitor bit9, sets flag 10 bit0) — line [?] |
| **102** | `sub_43AC0FFC` | vsync of output index 1 (`R 0x114`, monitor bit10, sets flag 10 bit1) — panel [?] |
| 79 | `sub_43904228` | VPH misc: reads 0xF34400A0, writes 0xF34400A4 |
| 98, 99 | `sub_43655A90`, `sub_43654D9C` | SYSV "INT SYS HS" (system / sensor sync), not VPH |
The ISRs do not write any VPH ack register (only the 0x038 monitor bits). Model 101/102 as **pulses or edge interrupts** [?].
They match the emulator's "GIC enabled after boot" list (79, 98, 99, 101, 102).

VPH task `sub_43904274`: `sub_438E8990` (init: 0x028 = 0x111, ena_int 79/101/102, osal ports) → `sub_438E8908` →
**wait flag 10 (bits 1|2)** → `sub_438E8854` (first real register setup: 0x100, 0x144 bits, 0x1C4, 4×CSC enables, 0x0A8, …) →
loop `{wait flag 10; sub_43AC2FD0()}`. `sub_43AC2FD0` re-applies the pending YUV/OSD config of both outputs
(`sub_4390A05C` → `sub_439098B8` / `sub_43909C68` copy 3088-byte shadow → `sub_438F4F9C` / `sub_438F503C`), logs AIF errors
(`sub_4390B2A8`) and finally clears the monitor bit. **So without IRQ 101/102 VPH is never programmed.** This is the state seen in
`io_after_boot.txt`, where 0xF3440028 is the only VPH write.

Commit rule for the emulator: treat everything as latched at vsync. Per-layer validity = layer-enable bit (0x144 / 0x184)
AND the AIF channel having been started (ctrl bit0 was set since the last vsync, or a sticky "configured" flag [?]).

---------------------------------------------------------------------------------------------------
## 5. How Rigel buffers reach VPH

1. The im.elf Rigel window server composes with the GPU (RE_DISPLAY_GPU §2). Android gralloc calls `rglDI_flushVDF`.
2. libObj `dmpNative::FbContainer::gen_struct_for_update` → `dmpNative::InternalCmdUpdate::set_data(update_osd_record*,int)`
   (`sub_5D7BD4`, int_cmd.cpp) → `vdf_if_port_send_update(rec, n≤2)` (`sub_5DC3D4`: osal msg 0x4C bytes, type 5, osal id 0x80003B).
   `update_osd_record` is 32 bytes: `+0 tgtOffH, +2 tgtOffV, +4 tgtSizeV, +6 tgtSizeH, +8 sizeV, +10 sizeH, +12 bpp (2|4),
   +16 flags (bit0 panel, bit1 line, bit4 "yuv trans" (32 bpp only), bit10 channel), +20 phys address, +24, +28 bmp handle`.
   This passes through `C_DataflowInfra_Entry_OsdImageSize::SetData_Image_{Address,SizeH/V,Target_*,Sel,Out_Target,Trans_Type}`.
3. In av-cam, VDF (`VdfInputCmdSetOsdArea`, OSD pin) → `UpdateOsdPrmGen` → VPH OSD layer (ch53 panel / ch54 line). When
   "yuv trans" is set, VDF first converts RGBA8888 → YUV 4:2:2 with the VFX engine (`VDF::VfxRgba8888ToYuv422Processor`,
   0xF3800000/0xF3900000), which yields a 16-bpp OSD [?].
4. Nothing in Linux maps 0xF344xxxx (no hits in im.elf / libObj / gralloc / libdihal decompiles). `/dev/mem` is used only for the GPU.

Expected values on a real camera: panel OSD = the 640×480 Rigel main window (32-bpp double buffer 0x8E9B2B00 / 0x8EC14E00,
stride 0xA00), or the 16-bpp buffers 0x8F758000 / 0x8F7EE000 (stride 0x500) after YUV conversion [?].

---------------------------------------------------------------------------------------------------
## 6. Emulator model (minimum to get a picture)

1. **RAM register file** for 0xF3440000–0xF3446FFF (reads return the last write). 0xF3441004 / 0xF3442004 / 0xF3443004 read 0.
   AIF ch +0x04 bit31 reads 0 (so stop never times out).
2. **Vsync timer** at 60 Hz [? panel rate; sftest reports 30 fps for Rigel]: once 0xF3440028 ≠ 0, pulse **GIC 102** and **GIC 101**.
   On each tick, latch the state below, then clear ctrl bit0 of channels 51–58.
3. **Compose the panel** (and optionally the line output into a second console):
   * YUV layer (0x144 bit0): ch51 start / line bytes / lines / pitch. Packed 4:2:2 if the line bytes ≈ 2·w, otherwise Y plus C [?]. Convert with BT.601
     (or decode the CSC unit at 0x300). If disabled, use black.
   * OSD layer (0x144 bit1 [?] or ch53 configured): base = ch53+0x20, line bytes = +0x24, lines = +0x2C + 1, pitch = (s16)+0x48,
     bpp from 0x404 bit0. 32 bpp = R,G,B,A bytes (A in byte 3; the window buffer reads 0xFF000000 when opaque black) [? R/B order].
     16 bpp: try YUYV first, then RGB565 [?]. Alpha-blend over YUV. Position the layer with the 0x14C/0x150 window.
   * Output size = 0x14C/0x150 window or the 0x5504 resizer output. Otherwise default to 640×480.
4. Fallback while the OSD path is not configured: the HLE monitor in RE_DISPLAY_GPU §5.

Open items [?]: exact meaning of the 0x144 bit1 and 0x1C4 layers; the 16-bpp OSD format; alpha/blend registers (0x5538/0x553C,
`VdfDisplayCmdSetOsdAlpha`); panel timing values at 0x5000; which of IRQ 101/102 belongs to the panel; ch55–58 roles; LCD-vs-EVF
selection (a5000 has an LCD only, panel IC over SIO/GPIO as in RE_DISPLAY_GPU §4.2; HDMI = line output + hdmi.ko TX).

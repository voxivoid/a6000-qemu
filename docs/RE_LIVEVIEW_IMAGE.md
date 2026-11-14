# RE: live-view image content — ch51 pixel format, the resize writer, and an injection spec (fw 1.10)

Follows `RE_LIVEVIEW_PIPELINE.md` (the control path now reaches VPH ch51) and `RE_SENSOR_ISP.md` §4.b / §7.
Method: Hex-Rays on a private copy of `C:\a5000\emu\lv2\pristine.i64` (`C:\a5000\emu\lvimg\`, restored from the
pristine copy before every query). Dynamic data comes from one run of the unmodified firmware (current
`qemu-system-arm` copy, pristine NAND clone, `-trace 'cxd_aif_*'`). It uses HMP `xp` of guest RAM and the VPH/AIF register
files in steady live view, plus one gdb breakpoint at `438F6CF4`. Addresses are av-cam VAs (PA = VA + 0x40000000)
unless MMIO. `[?]` = not proven.

---

## 0. TL;DR

* **ch51 carries one 8-bit plane, not packed YUYV.** In live view the VPH YUV descriptor says: planar 1 B/sample
  (`+32 = 1`), **1024 × 516 samples, line pitch 1152 B**, and **no C plane** (C address `+16 = 0`, interleave flag
  `+64 = 0`). The AIF channel is programmed as a 1-D line walker: `+0x24 = 0x400` bytes per line, `+0x28 = 0x80` =
  skip added **after** each line (pitch = 0x400 + 0x80 = 0x480), `+0x2C = 0x203` = lines − 1. `+0x48` (pitch) is 0
  and is not used on this path.
* **VPH scales the plane to the panel.** Unit 0 at `0xF3440300` is the YUV-layer scaler. The old doc called it a
  CSC. It holds `0x308 = 0x1133199D`: H ratio `0x199D = ((1024−1)<<12)/(640−1)`, V ratio
  `0x1133 = ((516−1)<<12)/(480−1)`. The low-pass bytes are `0x30C = 0x003B2727`, i.e. out/in × 64: 639·64/1023 = 39,
  479·64/515 = 59. So the panel shows **1024 × 516 → 640 × 480**.
* **The writer is the resize engine "RC" at 0xF3880000**, job `job_RCHV_dataset_main 437712E4`, kicked once per frame
  by `Sync_Kick_RCH` (`437DFDB0`). It reads the front-end stream from the IIB through **AIF ch23** (`0xF3881200`,
  `0x60000000`, 0xD80 B × 516 lines). It writes the plane through **AIF ch26** (`0xF3881380`, nibble 3, IRQ 41).
  The destination alternates **0x0CE00000 / 0x0CF34000**, which the CPU sees as 0x8CE00000 / 0x8CF34000. The geometry is
  the same as ch51. The RC's own memory check (`437714C0..437714D4`) needs `pitch × h × 2`, **halved when
  param[24] = 1** (set here). That is a single 1-B/px plane, so the firmware asks for luma only in this mode.
* **Injection spec (§4):** when AIF ch26 completes, write a host frame (8-bit luma, 1024 × 516, pitch 1152) to the
  programmed destination, then raise the completion as now. With no source configured, write nothing. The decoder
  must treat a C-less ch51 as Y8 with neutral chroma. Zeroed memory then shows **black**, which is what a real camera shows
  with the lens cap on. Writing mid-grey would show a lit grey scene. The flat green seen today is a decoder artefact
  (zero bytes read as U = V = 0).
* **Other readers:** none of the per-frame DMA readers touches the live-view buffer. Only VPH ch51 reads it. AE/AWB
  statistics are taken before the resize (front end, develop block), so they would not see an injected frame.

---

## 1. The ch51 format

### 1.1 Run-time state (steady live view, two buffers sampled)

| item | value | source |
|---|---|---|
| VPH `+0x144` | `0x01000233` (bit0 YUV layer on, bit1 OSD on) | HMP `xp 0xF3440144` |
| ch51 `+0x00 / +0x20 / +0x24 / +0x28 / +0x2C` | `2 / 0x8CE00000 or 0x8CF34000 / 0x400 / 0x80 / 0x203` | `xp 0xF3441200` |
| ch51 `+0x30..+0x4C` (ofs1..3, count1..3, pitch, skip) | all 0 | same |
| ch52 (line YUV) | all 0 | `xp 0xF3441280` |
| ch51 shadow descriptor (`byte_43E06B78`: `+8` addr, `+12` size) | `0x8CF34000`, `0x400` | `xp 0x83E06B78` |
| ch51 shadow ofs/count (`word_43E057B0`: `+0` count0, `+8` ofs0) | `0x203`, `0x80` | `xp 0x83E057B0` |
| buffer content | all zero (nothing writes it) | `xp 0x8CE00000`, `0x8CF34000` |

VPH YUV descriptor, captured with gdb at `438F6CF4` entry (`r0 = 1` panel, `r1 = 0` mode, `r2 = 0x43E058E0`, lr
`438F9877` in `438F969C`). The VDF copies at `0x43D60DC4` / `0x43D60BE8` (136 B each, `436607E0`) hold the same
values:

| off | field (`RE_DISPLAY_SCANOUT.md` §3) | value |
|---|---|---|
| +0 | Y address | `0x8CE00000` (alternating) |
| +4 / +6 | Y stride (px) / buffer lines | `0x480` (1152) / `0x204` (516) |
| +8 / +10 | x / y | 0 / 0 |
| +12 / +14 | width / height | `0x400` (1024) / `0x204` (516) |
| +16 / +20 / +24 | C address / C stride / C x,y | **0 / 0 / 0** |
| +32 | format: 0 = packed 2 B/px, ≠0 = planar 1 B/sample | **1** |
| +36 | progressive (`438E7600`: lines ÷ 1, else ÷ 2) [?] | 1 |
| +52 | 1 | |
| +60 | vflip | 0 |
| +64 | Y/C interleave mode (`== 1` → ch51 reads Y and C lines alternately) | **0** |
| +132 | done callback | `0x43A28367` |

### 1.2 How the firmware maps the descriptor to AIF registers

* `pbl_aif_set_func 43895C64` → `43896374`: ctrl `+0x00` from desc bytes 0–5 (mode bits, bit11 + nibble = IIB mode),
  `+0x0C` = desc+16 (write channels only), **`+0x20` = desc+8 (start address), `+0x24` = desc+12 (bytes per
  transfer)**.
* `43895D14` → `438964EE`: **`+0x28` ofs0, `+0x2C` count0, `+0x30` ofs1, `+0x34` count1, `+0x38` ofs2, `+0x3C`
  count2, `+0x40` ofs3, `+0x44` count3** (count = repetitions − 1).
* `43895DA0` → `4389653A`: `+0x50` repeat mode, `+0x54..+0x5C` rptadrs1..3.
* Address generator semantics: **after each transfer of `+0x24` bytes, the address advances by `+0x24 + ofs0`**,
  `count0 + 1` times; the outer loops add ofs1..3 the same way. Evidence: here `0x400 + 0x80 = 0x480` = the
  descriptor's Y stride 1152 (1 B/px). The same convention gives a consistent Y→C→Y walk in the `+64 == 1`
  interleave path of `438F6CF4` (count0 = 1, ofs0 = C − Y, ofs1 = Y − C) [? the exact size term of that path is
  lost in Hex-Rays].
* Bytes per sample: `438F6CF4` / `438F585C` use 1 when desc+32 ≠ 0 (alignment 64), else 2 (alignment 4). Width 1024
  × 1 = the 0x400 transfer.

### 1.3 VPH scaler — unit 0 at 0xF3440300 (not a colour converter)

`438F7FAC` (panel YUV layer) → `43AC4186` / `43AC3E4A` (ratio/phase calculation) → `43AC2A68` / `43AC2AB8`:

| reg | fields | value | meaning |
|---|---|---|---|
| 0x300 | [31:16] / [15:0] | `0xF88E / 0x008E` | V phase pair (s16, 4.12) [? field 0/1] |
| 0x304 | [31:16] / [15:0] | `0x00C9 / 0x00C9` | H phase pair [?] |
| 0x308 | [31:16] V ratio / [15:0] H ratio | `0x1133 / 0x199D` | in/out in 4.12: `((in−1)<<12)/(out−1)` |
| 0x30C | [22:16] / [14:8] / [6:0] (7 bit) | `0x3B / 0x27 / 0x27` | low-pass cut-offs = out/in × 64 (64 = none), scaled by the picture adjust values desc+68..72 |
| 0x310 | bit0 | 1 | enable |

Only `(in, out) = (1024, 640)` gives 0x199D and 0x27, and only `(516, 480)` gives 0x1133 and 0x3B. Units at
0x340 / 0x380 / 0x3C0 have the same layout and are probably the other layers' scalers [?].

Panel raster (for placement): `0xF3445028 = 0x359`, `+0x2C = 0x20D` → 858 × 525 total (480p-style timing). Active
lines 0x26..0x206 (`0x5014/0x5018`), and the YUV layer uses the same window (`0x14C = 0x00260026`,
`0x150 = 0x02060206`). `0x5048 = 0x02800025` → 640 px from H 0x25. So the scaled plane fills the 640 × 480 panel.
`0x148 = 0x00250025` and `0x154 = 0x1AD` are layer timing fields whose meaning is not decoded [?].

### 1.4 Answer to "what is ch51"

| property | value |
|---|---|
| layout | one plane, 8 bit per sample, line-linear |
| width × height | 1024 × 516 samples (`+0x24` × (`+0x2C` + 1)) |
| line pitch | 1152 B (`+0x24` + `+0x28`) |
| buffers | two, 0x134000 apart (0x8CE00000, 0x8CF34000), alternating per frame |
| chroma | **none is programmed on this path** (C address 0, `+64 = 0`, only ch51 started). The sample is luma by the RC's own accounting (§2.2) |
| subsampling | n/a for the programmed data. If a C plane is ever present, `+64 = 1` and desc +16/+20/+24 describe it. The RC's ×2 memory rule implies a 4:2:2 C plane of the same size as Y [?] |
| range | **not encoded in any register found** [?]. Assume BT.601 8-bit. Decoding Y as full range (grey = Y) is the neutral choice until the panel tone LUT (`0x51AC..0x51E4`) is decoded |
| display | VPH scaler 1024 × 516 → 640 × 480 over the panel's active area |

So the current decoder (packed YUYV, pitch from `+0x48` or `+0x24`, width 640) is wrong on three counts: format, pitch and
width. The 512-px area seen today is 1024 B read as 2 B/px.

---

## 2. Who writes the buffer

### 2.1 Per-frame DMA activity (AIF trace, ~640 frames, unmodified firmware)

| AIF ch (table `0x43B278D4`) | regs | dir | per-frame programming | role |
|---|---|---|---|---|
| 4 | 0xF3002400 (FE unit 0, nibble 4) | W | ctrl `0x803` (bit11 IIB), `+0x20 = 0x60000000`, size 0xD80, count 0x203 | front end → IIB stream, 3456 B × 516 lines |
| 5 / 1 | 0xF3002480 / 0xF3002280 | W / R | `0xC0184200` ↔ `0xC01ADE80` (iram), 0x1B0 B, ping-pong | front-end per-frame table (statistics or correction) [?] |
| 23 | 0xF3881200 (RC, nibble 0) | R | ctrl `0x2803`, `0x60000000`, 0xD80 B, count 0x203 | RC input from the IIB |
| **26** | **0xF3881380 (RC, nibble 3)** | **W** | ctrl `0x3`, **`0x0CE00000` / `0x0CF34000`**, size 0x400, ofs0 0x80, count 0x203 | **RC output = the ch51 buffer** |
| 18 | 0xF3020280 (develop group, nibble 1) | W | `0x40100000` / `0x4012F000`, 0x26000 B, count 0 | develop-block statistics output [?]; 0x40xxxxxx is not mapped in the QEMU machine |
| 25 | 0xF3881280 (RC, nibble 1) | W | **never started** | the RC's second output (C plane?) [?] |

Group `0xF3881000`: `+0x008 = 0xF000` (nibble 3 enabled), status nibble 3 = done → IRQ 41 (`438CB858`), as in
`RE_SENSOR_ISP.md` §7.1. Start order within a frame: FE ch4/ch5/ch1, then RC ch26 (output) **before** ch23 (input),
then develop ch18.

### 2.2 The resize job and its parameters

`437DFDB0` ("Sync_Kick_RCH") → without distortion correction (`+3113 == 0`) → `437712E4(job_RCHV_dataset_main)`.
It fills the static block `dword_43D89778` and calls `rcct_Resize 438CA92C` → `ResizeSeqCt 438CE5F8`. Values read
with HMP from `0x83D89778`:

| block off | source | value | meaning |
|---|---|---|---|
| +0x00 | param[0] | `0xA` | input format code (IIB stream) [? enum] |
| +0x04..+0x24 | param[3..11] | 0 | input addresses (none: IIB) |
| +0x28 / +0x2A | input width / lines | `0x6C0` (1728) / `0x204` (516) | 1728 px × 2 B = the 0xD80 IIB line |
| +0x34..+0x42 | crop/window from `a3` | `0x0000 0x0027 0x06BF 0x01B5 0x0000 0x01B3 0x0FFF 0x0C99` | [? meaning] |
| +0x5C | param[15] | 1 | output memory-list count |
| +0x60 / +0x64 | list[0] {addr, size} | `0x8CF34000` / `0x00116700` | output buffer, 1 140 480 B available |
| +0x80 | output pitch | `0x480` (1152) | |
| +0x82 / +0x88 / +0x8A | output lines / width / lines | 516 / `0x400` (1024) / 516 | |
| +0x9C (`dword_43D89814`) | param[24] | **1** | halves the required size (below), and selects pitch rounding `43A5DFD8` instead of `43A5DFCA` |
| +0xA0 (`dword_43D89818`) | param[13] | 0 | (2 = skip the size check) |

Memory check `437714A6..437714D4`: required = `pitch × lines × 2`, `>> 1` if param[24] = 1. It is compared with the
sum of the memory-list sizes. Here 1152 × 516 = 594 432 ≤ 1 140 480. Two planes (1 188 864 B) would not fit the list. So the
firmware itself configures **one 8-bit output plane**, and 1728 → 1024 is a horizontal-only resize (516 → 516 lines).

Address alias: the job gets `0x8CF34000`, and AIF ch26 is programmed with `0x0CF34000`. The RC/AIF bus addresses DRAM
from 0 (bus 0x0xxxxxxx = PA 0x8xxxxxxx) [? the conversion function was not located]. VPH ch51 takes the
CPU PA `0x8CE00000` directly. Other AIF address spaces seen: `0x6xxxxxxx` = IIB (internal, not DRAM), `0xC01xxxxx` =
iram, `0x40xxxxxx` = unknown (not DRAM: it would be the kernel image).

### 2.3 Stage order (one frame)

```
sensor ──SubLVDS/SIF──▶ CamCore FE unit 0 (0xF3000000, Pig IRQ 36)
   FE output AIF ch4 ──IIB 0x60000000 (line stream, 1728 px, 0xD80 B/line, 516 lines)──▶
   RC 0xF3880000: AIF ch23 (R, IIB) → H resize 1728 → 1024 → AIF ch26 (W) → DRAM 0x8CE00000 | 0x8CF34000
                  completion: group 0xF3881000 nibble 3 → IRQ 41 (438CB858)
   EE sequence stage 1 → YC state 2 (437737D4) → SendYCPin (VD_Post) → VDF UpdateYuvThrough (4367EC50)
   → VphUpdateYuvCall 43661BC8 → 438F969C → 438F6CF4: ch51 = same buffer, Y8, 1024 × 516, pitch 1152
   → VPH scaler 0x300 (1024×516 → 640×480) → panel
side outputs: FE ch5/ch1 (iram tables), develop ch18 (0x26000 B statistics) [?]
```
The "develop" (RAW → YC) step happens before the IIB, inside the CamCore units [?]. No separate develop DMA carries
image data in this mode: ch17 is never started, and the RC reads straight from the FE's IIB stream.

---

## 3. Open question that matters for colour

On a real a5000 the live view is in colour. In the emulator the firmware asks the RC for one 1 B/px plane
(param[24] = 1) and the pin reaches VDF without C information. In `436607E0` the pin's C sub-info is absent, so
`desc+16` is left at 0 and `desc+64` is never set to 1/128. The possibilities:
* **[?] A** — this mode is luma-only by design, and VPH synthesises neutral chroma for a C-less planar layer. A
  register or a 0x15C flag that would say so was not found (`0x158 = 0x15C = 0`).
* **[?] B** — param[24] (and the missing C sub-info) depends on state that the emulator does not reproduce yet (camera
  mode block `+1424+0x60` in `437DFDB0`'s object). On real hardware the RC would then write a C plane (ch25, or a
  second memory-list entry), and VPH would run ch51 in the `+64 == 1` Y/C interleave mode.

Either way the injection below writes what the firmware programs. If B turns out true, the same rule extends to the
C plane (§4.2 step 3).

---

## 4. Injection spec

### 4.1 Ranking

| rank | point | fidelity | needs |
|---|---|---|---|
| **(a)** | **RC output AIF ch26 completion** (`cxd90014-aif` instance `0xF3881000`, channel 3) | best: the frame lands in guest DRAM where the firmware expects it, at the time the hardware would finish writing it. Everything downstream (EE/VDF/VPH, double buffering, any future reader of the buffer) works unchanged | the channel registers only |
| (b1) | VPH compose (`cxd90014_vph.c`, today's `pattern=bars`) | display-only; guest memory stays empty | nothing; keep as a debug view |
| (b2) | IIB stream (FE ch4 → RC ch23) | would need a model of the RC (1728 → 1024 polyphase H resize, YC → Y selection, crop at +0x34..), and the IIB format code 0xA is undecoded [?] | RC engine model |
| (b3) | sensor / RAW (FE input) | would need the whole CamCore front end: black level, demosaic, WB, gamma, YC conversion, plus the RC | full ISP model |

(b2) and (b3) need ISP processing because the data they carry is not yet in the output format. The RC changes width
(1728 → 1024) and format (2 B/px stream → 1 B/px plane), and the FE turns Bayer RAW into the IIB stream. Only at the
RC output does the data match the buffer that VPH reads.

### 4.2 Behaviour of (a)

Machine option, debug only: e.g. `-global cxd90014-aif.image=<file|pattern>` on the `0xF3881000` instance, or a
separate `a5k-imgsrc` object referenced by that instance. With the option absent nothing changes.

At the moment `aif_complete()` marks channel 3 of group `0xF3881000` done, and **before** setting its status nibble
(so the frame is in memory before IRQ 41):
1. Read the channel registers: `A = +0x20`, `S = +0x24`, `O0 = (int32)+0x28`, `N0 = +0x2C + 1`, and ofs1..3 /
   count1..3 (`+0x30..+0x44`). Skip the write if `+0x0C`/ctrl say IIB mode (`ctrl bit11`) or if `A >= 0x20000000`.
   Only DRAM destinations are written.
2. Destination PA = `0x80000000 + A` (bus → CPU alias, §2.2) [?]. Walk the 4-D generator: for each of the N0 lines,
   write S bytes at the current address, then advance by `S + O0`. Apply the outer loops if their counts are non-zero.
   Each line gets one source row: scale the host image to `S` × `N0` samples (nearest or bilinear) and convert to 8-bit
   luma, BT.601 `Y = 0.299R + 0.587G + 0.114B` [? range, §1.4].
3. If a C destination ever appears (a second write channel of the group started in the same job, e.g. ch25 /
   nibble 1), fill it with 4:2:2 CbCr of the same frame using its own registers [? layout: interleaved Cb,Cr bytes
   per line].
4. Then set the status nibble and IRQ as today. Do not change the completion delay (2 ms after start) or the order
   of the other channels.

Sources: `pattern=bars` (8 luma steps), `pattern=ramp`, or a file. The file must already be `S × N0` raw Y8, or an image
the host converts. A static source is fine; a sequence can advance one frame per completion.

### 4.3 Default (no source configured)

* **Leave guest memory untouched.** This keeps the machine identical to the real one as far as the firmware can see:
  no extra memory traffic, same timing.
* Fix the VPH decoder separately (that fix is display only). For ch51 with `desc` C-less planar (all that is visible
  in registers: ch51 only, no C channel), read `+0x24` bytes per line, pitch `+0x24 + +0x28`, `+0x2C + 1` lines.
  Scale to the panel window (1024 × 516 → 640 × 480 from the 0x308 ratios), and show luma with neutral chroma
  (R = G = B = Y).
* **Lens-cap comparison:** a real camera with the lens cap on shows an almost-black live view: Y near the black level,
  chroma neutral, gain noise. Untouched (zero) memory decoded as Y8 gives black, so it is the closer default.
  Mid-grey would represent a uniformly lit grey subject, which the camera never shows when no light comes in. The
  green seen now comes from decoding zero bytes as U = V = 0 and is not a property of the hardware.

---

## 5. Other readers of image data (not to be modelled now)

| consumer | input | would it see an injected ch26 frame? | evidence |
|---|---|---|---|
| VPH panel YUV layer | ch51 = the ch26 buffer | **yes** | §1, §2 |
| VPH line (HDMI) YUV layer | ch52 | yes, if HDMI is connected (VDF's second descriptor `0x43D60BE8` is built from the same pin) [?] | ch52 regs 0 in this run |
| AE / AWB / AF statistics | front end (FE ch5/ch1 iram tables, OPD blocks 0xF3030000/0xF3031000, IRQ 67–72) and develop ch18 (0x26000 B to `0x40100000`) | **no**: all are taken before the RC | §2.1; IRQ 67–72 never fire (`RE_SENSOR_ISP.md` §2.5) |
| face detection | probably a second, smaller RC/VFX job over the YC buffer (`ZIT Det` / `ZitDdcDriver` strings) [?] | would, if it reads the ch26 buffer [?] | no AIF channel other than VPH ch51 reads 0x0CE00000/0x8CE00000 in this run, so it is not active |
| live histogram | OPD statistics [?] | no [?] | none observed |
| still / movie capture | separate RC jobs (`Capture::rcct_Resize_dataset_main 43757B24`, `Sequence_Movie` …) and the JPEG/AVC encoders | no: the injection only covers live view | |

---

## 6. Open questions

1. Colour (§3): is the luma-only RC output (param[24] = 1, no C sub-info in the YC pin) the real live-view
   configuration, or a consequence of emulator state? What does VPH show for a C-less planar layer: neutral chroma,
   or a fixed colour register?
2. Value range (video 16–235 or full 0–255) of the RC output and of the VPH input. Decode the panel tone LUT
   `0x51AC..0x51E4` and the remaining "CSC" units 0x340/0x380/0x3C0 (unit 0 is the scaler).
3. The RC bus-address conversion (`0x8CF34000` → `0x0CF34000`) and the meaning of the `0x40xxxxxx` space used by
   develop ch18.
4. RC parameter meanings: input format `0xA`, crop words `+0x34..+0x42`, param[24] / param[13] names. Which state
   selects the distortion-correction path (`+3113`, two RCH jobs with `43771F14`)? That path would change the job
   count per frame.
5. VPH layer fields `0x148 = 0x00250025`, `0x154 = 0x1AD`, `0x144` bits 4–5 / 8–9 / 24–25, and the exact size term
   of the `+64 == 1` Y/C interleave path in `438F6CF4`.

## 7. Reproduce

* IDA (copy pristine → `lv.i64` before each run): `dec 438F6CF4 43896374 438964EE 4389653A 438F585C 438F7FAC
  43AC4186 43AC2A68 437712E4 437DFDB0 436607E0 43660138 43A28772`, `dis 437714A0 40`, `dwords 43B278D4 216`.
* Run: `-trace 'cxd_aif_*'`, wait about 40 s for live view, then HMP `xp /32wx 0xF3441200` (ch51), `xp /16wx 0xF3440140`
  (layer), `xp /16wx 0xF3440300` (scaler), `xp /32wx 0xF3881380` (RC ch26), `xp /56wx 0x83D89778` (RC job
  block), `xp /34wx 0x83D60DC4` (VDF YUV descriptor).
* gdb breakpoint `*0x438F6CF4` → `x/40wx $r2` gives the VPH descriptor. **Detaching gdb stopped the engine for good:**
  no AIF starts after it, so the run had to be killed (as `RE_LIVEVIEW_PIPELINE.md` §7 warns). Do all gdb reads in
  one session and then kill QEMU.

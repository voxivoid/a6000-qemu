# RE: image sensor → capture front end → ISP → live view (fw 1.10)

Static RE of `av-cam.bin` (VA = PA − 0x40000000, Thumb-2) with Hex-Rays on windev (`C:\a5000\emu\sensor\sens2.i64`, a copy
of `ui_avcam.i64`), plus dynamic runs of the unmodified firmware with the `a5ktrace` plugin (`fn=` probes) and
`-d unimp` (the catch-all `cxd90014-regfile` logs every access to unmodelled blocks). Addresses are av-cam VAs unless
they are MMIO (0xF…). `[?]` = not proven.

Related docs: `RE_AVCAM_IRQ_BOOT.md` (IRQ table, task graph), `RE_ENGINE_TICK.md` (engine frame tick, 0xF3C30000 event
block), `RE_DISPLAY_SCANOUT.md` (VPH, AIF DMA, ch51 panel YUV).

---

## 0. TL;DR

* The sensor is driven by class `Camera::Imager::IMX156_ImagerCtrl` (RTTI string `N6Camera6Imager6IMX15612C_ImagerCtrlE`,
  vtable 0x43E8B868). **Control is write-only**: register writes go through the **P2S** ("parallel-to-serial")
  serialiser at **0xF3C00300 / 0xF3C00800 / 0xF3C00900, IRQ 38**, not through SIO/SPI/I2C. There is **no sensor ID
  read-back** anywhere on this path.
* Image data enters via **SubLVDS PHY (0xF292B000)** → **SIF** sync-code receivers (0xF3122000/0xF3123000) → two
  identical **CamCore front-end units** at **0xF3000000** and **0xF3100000** (+ 0xF3010000/0xF3110000 sub-blocks) whose
  RAW/statistics outputs are written to DRAM / on-chip SRAM by **AIF DMA channels 0–11**. Frame completion of each unit
  is reported by its "Pig" interrupt block: **IRQ 36 (0xF3000000), IRQ 37 (0xF3100000), IRQ 35 (0xF3120000)**.
* Dynamic result in shooting mode (stock firmware, current emulator): **the engine does start live view** — every
  frame it runs `Imager::Execute`, `SetMode`, `SetExposure`, the P2S send, the CamCore param-set/start and the SIF
  reads — **but none of the front-end completions ever happens**:
  1. P2S busy bit `0xF3C00908 bit0` is never cleared (regfile keeps the 1 written at start) → every later P2S send is
     refused (`sub_43A49854` returns 0), IRQ 38 never fires.
  2. AIF channels **1, 4, 5** (0xF3002280/0xF3002400/0xF3002480) keep ctrl bit0 = 1 forever → `pbl_aif_set_func`
     returns −3 (busy) → logged `AIF%d pbl_aif_set_func error[-3]` and `CamCore ParamSetError 0x2206`.
  3. IRQ 35/36/37 (Pig frame done) never fire.
  4. Consequently **VDF never receives a YC pin**: `VphUpdateYuvCall` (`43661BC8`) and the VPH YUV setter
     (`438F6CF4`) are **never executed**, so AIF ch51 (panel YUV) is never programmed. That is why live view is black,
     and is the most likely reason why other camera functions (MENU in shooting mode) wait [? — app side is another
     agent's topic].
* Minimal plan (§4): model **P2S completion**, **AIF channel completion** (self-clearing start bit) for the
  front-end/ISP channel groups, and the **Pig frame-done IRQs 35–37** clocked from the engine frame. Then inject an
  image either at the VPH ch51 read (cheap, post-ISP) or at the ISP output AIF write channels (more faithful).

---

## 1. Sensor control interface

### 1.1 Imager object
| item | value | evidence |
|---|---|---|
| class | `Camera::Imager::IMX156_ImagerCtrl` (base `C_Imager`, vtable 0x43E8B7A8) | RTTI 0x43EA80B8, vtable 0x43E8B868 |
| instance | `&dword_43D6FE10[323]` = **0x43D7031C** (single channel; `Imager Select: ch%d does not exist` for ch≠0) | `sub_436AE9D8`; runtime r0 of every probe |
| vtable slots | +8 GetInfo, +16 **PowerOn** `436AF630`, +20 **PowerOff** `436AF414`, +24 **SetMode** `436AF334`, +28 **GetParam** `436AF1D8`, +32 **SetExposure** `436AEF00`, +40 **SetRead** `436AFC48`, +48 **Execute** `436B0658`, +52 exec-select `436AEEB0`, +84 `AsyncInitial` `436AFE18` (via `4398DB8E`) | vtable dump |
| drive modes | table `dword_43EA82B8` = {0x0B,5,0x10,7,0x18,0x1A,0x19,3,1,0x0A,0x0F,0x17,0x1E}; **17 = POWERSAVE** (all setters return 2) | `sub_43A49FFC`, `436AF334` |

Boot sequence (dynamic, one each): `4398DB8E` (camera power command, case 1) → `PowerOn` → `AsyncInitial` → first P2S
send → `SubLVDS PowerOn`. In live view, per frame: `SetMode`, `GetParam`, `SetExposure`, `SetRead`(occasional),
`Execute`, P2S send.

### 1.2 Power / reset lines
`PowerOn` toggles GPIOs whose port/pin come from Backup (`sub_4355DCD8` = `Backup_read_4Byte`) pairs
0xF90008/09, 0A/0B, 0C/0D, 0E/0F, 0x26D/26E, 0x143/144, 0x145/146 (+ 0x147/148 in AsyncInitial), with fixed delays
(utimer, "PowerOn Wait No.00..03"). In the user's Backup.bin all of them are **0xFFFFFFFF (not fitted)** except
**0xF9000E/0F = port 0 / pin 1** [? which role]. The GPIO helpers are `43AC7740` (low), `43AC7734` (high), `43AC76BE`
(direction/config), `43AC772A(port,pin,level)`. Nothing here is read back → no model needed beyond the existing GPIO.
Sensor supply rails are presumably switched by the sub-micro/PMIC [?].

### 1.3 P2S serialiser (sensor register writes) — **0xF3C00300, IRQ 38**
Unit table `dword_43B1DC5C` (3 pointers per unit): unit 0 = {**0xF3C00300**, **0xF3C00800**, **0xF3C00900**}; units 1/2 =
{0xFC088300, 0xFC088800, 0xFC088900} / {0xFC089300, …} (unused on a5000 [?]).

| reg | access | meaning | evidence |
|---|---|---|---|
| 0xF3C00300 | RMW | bit0 set at config; +0x0C low byte = byte count/flags (`436AE684`), +0x04 bit0, +0x08 bits0-2 from the 5-byte sync config | `436AE62C` |
| 0xF3C00340 + 4·i | W | packed payload, one byte per word (i < 32) | `436AE6A4` (index 16+i) |
| 0xF3C00800/+8 | W | `+0` = mode bits0-1, `+8` = kick toggle **1,0,1** | `436AE62C`, `436AE734` |
| 0xF3C00904 | W `\|=1` | IRQ ack (ISR) / enable | `436AE758`, ISR `436AE784` |
| **0xF3C00908** | R/W | **bit0 = start / busy**. Driver writes `\|=1` to start and **refuses a new transfer while bit0 is 1** (`436AE7A8`) | `43A49854` |

Packet build: `436AE414` (begin, SyncMode must be 2, clears flag 24 bit), `436AE5EC(addr,data)` per register
(formatter `43A49B36`: sequential addresses are packed as runs), `43A49986` → `436AE4EC` (program + start) →
`43A49948`. Completion: **IRQ 38** handler `436AE3EC` acks `0xF3C00904 |= 1` and calls `43A49938` (sets event flag 24
bit of the object) — waited by `436AE458` "P2SWaitTransComp" with a **2 ms** timeout and by `436AE4EC` ("P2S Error:
Last trans is not end") with 1 ms. `ena_int(38)` is issued by the first start only (`43A49854`, unit 0).

Initial register list (AsyncInitial, `dword_43B36D78`, 19 × {u16 addr, u8 data}):
`00:34 0E:04 10:08 16:00 27:80 2B:03 4F:80 51:38 52:0A 55:60 62:99 69:12 76:18 7A:18 84:00 AF:00 B0:02 B1:00 B2:02`.
Per-frame writes (exposure/gain/readout) come from the tables at `dword_43BDD108`/`43B1EF5C` etc. [not decoded].

**Sensor ID read-back: none.** The P2S is write-only and no other path reads the sensor; the sensor type is fixed at
build time. The only sensor "reads" are the SIF sync-code counters (§2.2) and the imager temperature via ADC
("ImagerTempAD", `GetImagerTemperature`) [? ADC channel].

---

## 2. Image data path

```
IMX156 ──SubLVDS──▶ SubLVDS PHY 0xF292B000 ──▶ SIF (sync-code rx) 0xF3122000/0xF3123000
        ──▶ IIF (input select, IIFSENS0/1)  ──▶ CamCore front-end unit 0 0xF3000000 (+0xF3010000)
                                                  unit 1 0xF3100000 (+0xF3110000), "Pig" frame ctl, IRQ 36 / 37
             AIF write ch 3–5 / 9–11 ──▶ DRAM (RAW, stats) / on-chip SRAM 0xC01xxxxx
        ──▶ develop/YC (0xF3020000/0xF3021000, AIF ch17–20), stats/OPD (0xF3030000/0xF3031000, IRQ 67–72),
            0xF3040000 (IRQ 73), resize/filter engines 0xF3800000/0xF3880000/0xF3890000/0xF3900000/0xF3980000 (AIF 23–34, 59–68) [?]
        ──▶ YC buffer ──YC pin (dataflow)──▶ VDF `SystemCmdPinSend::YcPinExec` 43672FC4
        ──▶ VdfInputCmdUpdateYuvPin 4367F700 → VphUpdateYuvCall 43661BC8 → VPH AIF ch51 0xF3441200 → panel
```
Block names come from RTTI (`CamCore::{CamAif,CamArc,CamCas,CamDcv,CamDev,CamHead,CamNr,CamRdc,CamSig,Iif,Sgc1,TestPG,Prc}`,
`IifPigInterruption`, `CamPigInterruption01/23`); the mapping of each name to a base is only partly proven [?].

### 2.1 SubLVDS PHY — 0xF292B000
`SubLVDS PowerOn` (`436B3126`, block sel 0): `0xF292B008 = 0x1000` (`436B3218`), then `0xF292B008 = param & 0xFFF`
(`436B31E8`), then `0xF292B004` or `0xF292B008 = 0x10000` (`436B3228`). Write-only. Regfile is enough.

### 2.2 SIF — sync-code receivers, 0xF3122000 / 0xF3123000
Six "sif_block_sel" values (0–5). Sync-code patterns are written by `SIF SetSyncCode` `43543F00` (e.g. block 0:
0xF3122310…). **Sync-code counters** (`ReadSIFScodeCnt` `436B2474`): block 0 `0xF3122330`, block 1 `0xF3122224`,
block 2 `0xF3123330`, … (one per block). They are summed per frame (`436B0E12`) and printed as `SIFcount` in the
`Imager::Execute` trace. No consumer that fails on 0 was found [? — keep a per-frame incrementing counter to be safe].

### 2.3 CamCore front-end units and their "Pig" frame/IRQ block
Same register layout at **0xF3000000 (unit 0, IRQ 36)**, **0xF3100000 (unit 1, IRQ 37)** and **0xF3120000 (IRQ 35)**
(objects `436A682A/6866/68B6`; IRQ masks in each object: 0x20000000 / 0x40000000 / 0x10000000):

| off | meaning | evidence |
|---|---|---|
| +0x00 / +0x08 | per-sub-block reset/enable bits (writes 0x40…0x2000 at init) | unimp log |
| +0x10 | command: `2·x` = start/arm path A (bit1), `8·x` = path B (bit3); observed 0x8, 0x4 | `43A45FD0/43A45FDC/43A45FE2` |
| +0x14 | `8·x` / `x` = matching enable [?] | `43A45FF6/43A45FFC` |
| +0x1C / +0x24 | **AIF channel mask** for path A / B (bits 0x1000<<k = AIF ch k of the unit's group, table `dword_43B1B6A0`) [?] | `43A46004`, `43A4600C`, `436A6640` |
| +0x20 | observed 0x161 / 0x981 [?] | unimp log |
| **+0x48** | **status A**, write-back = W1C | ISR `436A6904`/`43A465F2`, `436A62A4` |
| **+0x4C** | **status B**, W1C | `436A62B0` |
| +0x50 | bit27 / bit25 = IRQ enable for the two paths (0x8000000, then 0xC000000) | `43A4603E`, unimp log |

ISR (e.g. IRQ 36 `43A465F2`): read +0x48 / +0x4C; for B: ack and run the waiter list (`436A65A8`); for A: ack,
`436A6794` → for every AIF bit in the saved mask call the AIF "done" hook `43A970AE` → `438964B0`, then run the
waiter list (`436A6414`). Waiters are registered with tags `obj+0x14 | 0x2000000` (A) / `| 0x8000000` (B)
(`436A6700` / `436A666C`). The three IRQs are enabled together by `436A62BC` (enabled after boot).

The 0xF3120000 unit additionally has the input/clamp registers +0x28 (`436A5D3C`), +0x38/+0x3C (`bit0`, `&3` status
polled), and its frame routine `436A5E7C` runs every frame (dynamic) computing crop/offsets from `+0x…` readbacks.

### 2.4 AIF DMA channels used by the camera pipeline
Generic driver at `43895C64` (`pbl_aif_set_func`), table **0x43B278D4** (`{group, ch, dir|nibble<<8}`), same register
layout as RE_DISPLAY_SCANOUT §2 (ctrl +0x00 **bit0 = start/busy**, +0x04 bit31 running, +0x20 address, +0x24 size/line,
+0x28.. ofs/count, +0x44, +0x5C). `pbl_aif_set_func` returns **−3 if ctrl bit0 is still 1** (`438965E8`), −8 for bad
alignment.

| AIF ch | regs | dir | block |
|---|---|---|---|
| 0–5 | 0xF3002200 + 0x80·k (group 0xF3002000) | R,R,R,W,W,W | CamCore unit 0 |
| 6–11 | 0xF3102200 + 0x80·k (group 0xF3102000) | R,R,R,W,W,W | CamCore unit 1 |
| 12 | 0xF3131200 | R | ? |
| 17–20 | 0xF3020200/280/300/380 | R,W,W,W | develop/YC [?] |
| 21–22 | 0xF3041200/280 | R,W | 0xF3040000 block |
| 23–34 | 0xF3881xxx, 0xF3891xxx, 0xF3981xxx | 2R+2W each | resize/filter engines [?] |
| 35–44 | 0xF30A1xxx, 0xF30B1xxx, 0xF31A1xxx, 0xF31B1xxx, 0xF31C1xxx | R,W pairs | ? |
| 45–50 | 0xF3811xxx, 0xF3911xxx | 2R+1W each | ? |
| 51–58 | 0xF3441200… | | **VPH** (see RE_DISPLAY_SCANOUT) |
| 59–68 | 0xF3800200…, 0xF3900200… | 3R+2W each | VFX (IRQ 77/78) |
| 69–71 | 0xF31D0200/280/300 | R,R,W | ? |

Observed in live view (unimp log): ch4 `+0x20 = 0x60000000` (IIB/internal buffer mode, `+5 == 3`), size 0xD80, ctrl
0x802→0x803; ch5 `+0x20 = 0xC0184200` (on-chip SRAM), size 0x1B0, ctrl 0x2→0x3; ch1 ctrl 0x2→0x3. After that the
driver only polls `+0x00`, `+0x1C`, `+0x44` once per frame and every restart fails with −3.

### 2.5 Other front-end IRQs (from RE_AVCAM_IRQ_BOOT §1.1)
67–70 (0xF3030104… stat/DMA engines), 72 (0xF302042C), 73 (0xF3040004), 74/75 (0xF3850000/0xF3950000 +0x10),
83/84 (0xF3C30000, modelled). None of 67–75 fired in the runs below; whether the live-view chain needs them is open.

### 2.6 VDF side
`SystemCmdPinSend::YcPinExec` `43672FC4` receives YC pins; `VdfInputCmdUpdateYuvPin::Execute` `4367F700` →
`UpdateYuvThrough` `4367EC50` / `UpdateYuvResize` `4367E818` → `VphUpdateYuvCall` `43661BC8` → VPH YUV input
`438F6CF4` (descriptor in RE_DISPLAY_SCANOUT §3) → ch51.

---

## 3. Engine readiness gates — what happens now

Run: pristine NAND, frozen QEMU binary, `a5ktrace fn=` probes (fncap 30, reloaded several times), first-boot dialog →
MENU → PlayMemories → Enter → shooting screen (NO CARD).

| probe | hits per window | meaning |
|---|---|---|
| `4376B594` HS_Sequence, `4376A2C4` VD_Drive | every frame | engine frame tick runs (RE_ENGINE_TICK) |
| `436B0658` Imager::Execute, `436AF334` SetMode, `436AEF00` SetExposure, `436AF1D8` GetParam | every frame | **live view (EE) is running on the engine side** — already before shooting mode |
| `436AE4EC` P2S send | every frame | but `43A49854` refuses (busy), only one `ena_int(38)` at boot |
| `437DF190` CamCore param-set, `437DFBF0` CamCore start, `436A5E7C` 0xF3120000 frame | every frame | front end is (re)armed each frame |
| `436B2474` SIF sync count | every frame | reads 0 |
| IRQ 35/36/37/38/67/72/73/74/75 handlers | **0** | no front-end completion ever |
| `43661BC8` VphUpdateYuvCall, `438F6CF4` VPH YUV set | **0** | **no YC frame ever reaches VDF/VPH** |
| `ena_int` during live view | only 202/204 (SIO) | |

Engine error log (`43894848`, format string = r0) during shooting mode: `AIF%d pbl_aif_set_func error[-3]` for ch 4, 5, 1
(`436A5228`), `CamCore ParamSetError 0x2206` (`437DF6BC`, return of `436A3FAC`), plus timing warnings
`HS_Main/HS_Sequence/VD_Sequence/VD_Post: start delayed / overflow`, `int_LM_Send1st_Handler: EC_VD_Post had been
still running` — the latter appear because the slowed (plugin) emulation and the evsel model fire all event sources
of a frame at the same instant (RE_ENGINE_TICK §1) [?]. They are warnings, not hangs.

Gates, in pipeline order (all missing in the emulator today):

| # | gate | needed hardware behaviour | today |
|---|---|---|---|
| G1 | P2S transfer complete | `0xF3C00908` bit0 self-clears after the serial transfer; `0xF3C00904` status; **IRQ 38** pulse | bit0 stays 1, no IRQ |
| G2 | AIF channel complete (front-end ch0–11, ISP ch17–50, VFX) | ctrl `+0x00` bit0 and `+0x04` bit31 self-clear at end of frame / transfer | latched in regfile → −3 |
| G3 | front-end frame done | Pig `+0x48`/`+0x4C` status bits (bit25/27 class, matching the armed path) + **IRQ 36/37/35** per frame while armed (`+0x50` enables) | never |
| G4 | ISP/develop completion IRQs | 67–70/72/73/74/75 when their engines are kicked [?] | never (not yet seen kicked) |
| G5 | YC pin to VDF | follows from G1–G4 (pure software) | never |
| — | sensor ID / handshake | **none exists** | n/a |

---

## 4. Minimal emulation plan

### 4.a Reach "live view running" (black/grey frames)
Ordered; each step is checkable with the probes above (look for IRQ handler hits, then `43661BC8`).

1. **P2S** — new `hw/misc/cxd90014_p2s.c`, MMIO 0xF3C00300 (0x100), 0xF3C00800 (0x100), 0xF3C00900 (0x100), IRQ 38.
   Latch payload/config writes. On `+0x908 |= 1`: schedule completion (a few µs; bytes×bit-time is fine), then clear
   bit0, set `+0x904` status bit0 and pulse/raise IRQ 38; `+0x904` write-1 clears. Optionally log the decoded
   {addr,data} stream (trace event) — that is the sensor register log for free.
2. **AIF completion for camera channel groups** — generalise the VPH AIF logic into `hw/dma/cxd90014_aif.c`
   (one instance per group: 0xF3002000, 0xF3102000, 0xF3020000, 0xF3041000, 0xF3811000, 0xF3881000, 0xF3891000,
   0xF3911000, 0xF3981000, 0xF30A1000, 0xF30B1000, 0xF31A1000, 0xF31B1000, 0xF31C1000, 0xF31D0000, 0xF3131000,
   0xF3800000, 0xF3900000). Behaviour: `+0x00 |= 1` (start) → `+0x04` bit31 set → completion → clear `+0x00` bit0 and
   `+0x04` bit31; group `+0x004` error status reads 0. For channels owned by a Pig unit (ch0–11) complete them at the
   unit's frame-done (step 3); for the others complete after a short delay [? per-block trigger not decoded].
   Stand-alone channels with `+0x20 = 0x6xxxxxxx` (IIB mode) must not be treated as DRAM addresses.
3. **Pig frame-done** — `hw/misc/cxd90014_campig.c`, three instances at 0xF3000000 (IRQ 36), 0xF3100000 (IRQ 37),
   0xF3120000 (IRQ 35), each 0x100 of registers on top of the regfile (the rest of each 64 KiB block stays regfile).
   When `+0x10` arms a path (bit1 or bit3) and `+0x50` enables its IRQ (bit25/bit27 [?]), then at the next engine
   frame (hook the existing 29.97 Hz frame clock in `cxd90014_evsel.c`, source 29, or a VD timer at the same period)
   set the corresponding status bit in `+0x48` (A) / `+0x4C` (B), complete the AIF channels in `+0x1C`/`+0x24`, and
   raise the IRQ; W1C on write. Which exact status bit the driver expects is the main open item — the ISR acks
   whatever it reads and only uses the value as the tag for `436A6794`, so mirroring the armed bits (0x2000000 /
   0x8000000) is the starting point [?].
4. **SIF counters** — in the same campig/sif file or the regfile: make `0xF3122330`, `0xF3122224`, `0xF3123330`
   (+ the other block counters) increase once per frame (hypothesis; zero did not crash).
5. Re-run the probe set. Expected next symptoms: IRQ 35–38 hits, the AIF −3 errors gone, then either `43661BC8`
   hits (done) or a new stall in the develop/resize engines (IRQ 67–75, VFX 77/78) → model those as
   "kick → complete → IRQ" in the same style.

Everything else (exposure, gain, stats at 0xF3021000/0xF3031000/0xF3C04100 that are read every frame) can stay
read-as-written/zero: AE will see a black scene and saturate gain, which is harmless.

### 4.b Showing a test pattern / host image

| option | where | how | pros | cons |
|---|---|---|---|---|
| **B1 post-ISP (recommended first)** | VPH ch51 scan-out (`cxd90014_vph.c`, already composes YUV under the OSD when 0x144 bit0 is set) | when ch51 is programmed (+0x20 Y addr, +0x24 bytes/line, count, C plane via ofs1..3 when fmt≠0), write the host image (converted to the programmed YUV 4:2:2 layout) into guest RAM at those addresses just before compose, or compose the host image directly in place of the guest buffer | tiny; geometry/format come from the firmware; no ISP math | needs §4.a done first (VDF must program ch51); AE/AF/face/histogram see nothing; recorded stills/movies are not the injected image |
| **B2 ISP output** | write-direction AIF channels of the develop/resize engines (ch18–20, 25/26, 29/30, 33/34 …) | on completion, fill the destination described by the channel regs (+0x20/+0x24/+0x28.. counts) with a scaled host image in the expected YC layout | stills/JPEG and live view both get the image; still no ISP math | must learn which channel carries which plane/size per mode [?]; more surface |
| **B3 sensor RAW** | front-end write channels ch3–5 / 9–11 | write a Bayer RAW frame (from a host image, mosaicked) | fully faithful upstream of the ISP; stats engines could be fed | the whole develop/YC pipeline (0xF3020000+, resize, VFX) would then need real image processing — very large |

Suggested order: §4.a 1–3 → confirm `43661BC8` → B1 with a fixed colour bar → B2 later if captures should contain the
image.

New files (in `qemu-overlay/`, registered in Kconfig/meson like the others):
`hw/misc/cxd90014_p2s.c`, `hw/misc/cxd90014_campig.c` (incl. SIF counters), `hw/dma/cxd90014_aif.c` (shared with VPH
later), and an optional `hw/misc/a5k_imgsrc.c` (host image/test-pattern source used by VPH/AIF, debug option only).

---

## 5. Tools used / reproduce
* IDA query runner: `C:\a5000\emu\sensor\sens_q.py` on `sens2.i64` (commands `dec`, `xstr`, `callers`, `dwords`, `dis`).
* Probe list for `a5ktrace fn=` (one `addr label` per line): `436AE4EC p2s_send`, `436AF630 img_poweron`,
  `436AFE18 img_asyncinit`, `436B0658 img_execute`, `436AF334 img_setmode`, `436A6904 irq35`, `43A465F2 irq36`,
  `436A6940 irq37`, `436AE3EC irq38`, `43661BC8 vph_updyuv`, `438F6CF4 vph_yuvset`, `43894848 errlog`,
  `4355D8FC ena_int`. The `errlog` r0 is a format-string VA → read it from av-cam.bin at `r0 − 0x43541000`.
* Do not `xp` GIC registers from the HMP monitor: it crashed the frozen QEMU binary (SIGSEGV, exit 139).
* With the plugin loaded, key presses need a longer hold (`sendkey ret 600`).

## 6. Open questions
1. Exact status bits the Pig ISR expects in `+0x48`/`+0x4C`, and the meaning of `+0x10/+0x14/+0x20/+0x24` values
   (0x8/0x4, 0x161/0x981).
2. Which CamCore RTTI block lives at which base (0xF3010000, 0xF3020000/0xF3021000, 0xF3030000/0xF3031000,
   0xF3040000, 0xF3850000/0xF3950000/0xF3A50000).
3. Whether the develop/resize stages (IRQ 67–75, VFX 77/78) gate the YC pin once G1–G3 are modelled.
4. Why `CamCore ParamSetError 0x2206` (`436A3FAC`) — expected to be the AIF busy error, to be confirmed after G2.
5. Per-frame P2S register content (exposure/gain tables at `dword_43BDD108`/`43B1EF5C`) — only needed for logging.
6. Whether the shooting-mode MENU stall is caused by this (no YC → camera "monitoring" never reported ready) — to be
   checked against the app-side analysis.
7. Imager power: the Backup GPIO pairs are absent on a5000; which of p0.1 / the sub-micro switches sensor power [?].

---

## 7. Implementation status (camera-fe branch, plan §4.a)

All new models are hardware-level (no firmware patches); every value below comes from the av-cam code cited, the rest
is marked `[?]`. QEMU files: `hw/misc/cxd90014_p2s.c`, `hw/misc/cxd90014_campig.c` (Pig blocks + SIF),
`hw/dma/cxd90014_aif.c`, wiring in one block "camera front end" of `hw/arm/sony_a5000.c`, debug live-view source in
`hw/dma/cxd90014_aif.c` (`lv-source`). Trace events: `cxd_p2s_*`, `cxd_campig_frame`, `cxd_aif_start/done/status`.

### 7.1 What is modelled

| block | model | evidence |
|---|---|---|
| P2S 0xF3C00300 (device covers the page 0xF3C00000–0xFFF), IRQ 38 | write `0x908` bit0 = 1 starts a transfer; 50 µs later [? time] bit0 clears, `0x904` bit0 sets, IRQ 38 is held until `0x904` bit0 is written 1 (W1C). Other registers read back as written; the first 16 payload bytes are traced | `43A49854`, `436AE758` (`+4 \|= 1` before start), ISR `436AE3EC`→`436AE784` |
| CamCore "Pig" 0xF3000000 / 0xF3100000 / 0xF3120000 (first 0x100 bytes), IRQ 36 / 37 / 35 | a write to `+0x10` arms path k for every set bit k (0..3); at each engine frame (33367 µs, own timer, phase arbitrary [?]) armed paths set `+0x48` bit (24+k) and disarm; IRQ = `(+0x48 \| +0x4C) & +0x50 & 0x0F000000`; `+0x48`/`+0x4C` W1C; `+0x4C` is never set [?]. GPIO out 0 pulses every frame | accessors `43A45FA4..43A46054`; arm `436A6700` (`+0x10 = 2`, `+0x50` bit25) / `436A666C` (`+0x10 = 8`, bit27); ISR `43A465F2` → `436A6794` writes the status high byte back to `+0x10`/`+0x14` (re-arm) |
| SIF 0xF3122000–0xF3123FFF | sync-code counters `+0x330`, `+0x224`, `+0x1330`, `+0x1224` return the number of frames of the 0xF3120000 unit [? semantics]; rest read back as written | `ReadSIFScodeCnt 436B2474` → `436B26E8/289C/2AD4/2C88` |
| AIF channel groups (table `0x43B278D4`, all except VPH 0xF3441000) | channel `+0x00` bit0 = start/busy, `+0x04` bit31 = running. On completion both clear and status nibble bit0 sets in group `+0x000` (W1C). Channel k's sysbus IRQ = `(+0x000 & +0x008) nibble k != 0`. Front-end groups 0xF3002000 / 0xF3102000 complete at their Pig unit's frame end; all others 2 ms after start [? trigger not decoded]. `+0x004` reads 0. No data is moved | `438965E8`, `43896588/43896614`, `43896650`; status read `43896484`/`43AB98B6`, ack `0xF << 4k` `43AB98C6`, Pig done hook `438964B6`; `+0x008` nibble RMW `43896442` (0xF3881008 = 0xF000 while ch3 is waited for) |
| AIF completion IRQs 40–50 | ch1 / ch3 of the resize ("RC") groups: 0xF3881000 → 40 / 41, 0xF3891000 → 42 / 43, 0xF3981000 → 44 / 45, ch1 of 0xF30A1000 / 0xF30B1000 / 0xF31A1000 / 0xF31B1000 / 0xF31C1000 → 46 / 47 / 48 / 49 / 50 | ISR table `0x43E94D04` (`{irq, 0, handler, attr}`): 40..50 → `438CB804, 858, 73C, 790, 8CC, 920, 598, 5EC, 640, 694, 6E8`; each handler checks one or two RC objects (e.g. IRQ 41 → objects `0x43DFFDF4`/`0x43E002CC`); at run time those objects' status descriptor (`obj+252` → `+20` register, `+64` nibble) is `{0xF3881000, 3}` (read with HMP `xp` from guest RAM). Objects of IRQ 40/42/44/46..50 point at ch1 of the groups listed |
| live-view test frame (debug only) | the former VPH `pattern=bars` option is gone; `-global cxd90014-aif.lv-source=bars\|ramp\|file:PATH` writes a luma frame into guest memory when RC ch26 completes (`RE_LIVEVIEW_IMAGE.md`, Implementation status) | — |

Notes:
* The RC IRQ lines are level-sensitive in the guest's GIC configuration (a pure `qemu_irq_pulse` is lost; a 10 µs level
  re-enters the ISR several times). With the status-nibble model the line drops when the ISR acks the nibble, as the
  code expects.
* QEMU cannot map an MMIO region whose start is not page aligned across a page boundary (the in-page offset collides
  with the section index in the TLB, assertion in `iotlb_to_section`); the guest page size here is 1 KiB. That is why
  the P2S device covers the whole 0xF3C00000 page.
* The previous §2.3 statement "objects `436A682A/6866/68B6`, masks 0x20000000/…" is unchanged; the RC IRQ row of
  `RE_AVCAM_IRQ_BOOT.md` §1.1 ("object +0x14 → status reg") is now resolved: the status register is the AIF group
  header `+0x000` of the watched channel.

### 7.2 Result (unmodified firmware 1.10, first boot → MENU → Enter → shooting screen, NO CARD)

Probe windows of ~30 frames in shooting mode (`a5ktrace fn=`), before → after:

| probe / log | before (§3) | after |
|---|---|---|
| IRQ 38 handler `436AE3EC` | 0 | every P2S send (2 per frame) |
| IRQ 36 handler `43A465F2` | 0 | every frame (path B/C, status `0x0C000000`) |
| IRQ 35 / 37 | 0 | 0 — the 0xF3120000 unit arms path A without IRQ enable, unit 1 is not armed in this mode |
| IRQ 41 (`438CB858`, resize 0xF3881000 ch3) | n/a | every frame (one resize job per frame: ch0 reads the IIB 0x60000000, ch3 writes 1024 B lines at 0x0CE00000/0x0CF34000 [? address space]) |
| `AIF%d pbl_aif_set_func error[-3]`, `CamCore ParamSetError 0x2206` | every frame | gone |
| `CamCore Start Error 0x208`, `job_RCHV_dataset_main: RC_ERR:0x07 rcct_Resize error!` (appeared once the front end ran, before IRQs 40–50 existed: RC request pool exhausted, `ResizeSeqCt` "request = NULL") | — | gone |
| remaining engine log | — | timing warnings only (`HS_Main/HS_Sequence/VD_Sequence ... start delayed / overflow`) |
| `VphUpdateYuvCall 43661BC8`, `438F6CF4`, VPH `0x144` bit0, ch51 | never | **still never** (`0xF3440144 = 0x03000232`, ch51 registers 0) |
| MENU (`sendkey m`) in shooting mode | delivered but ignored | **opens the main menu** (Camera Settings / Custom / Wireless / Application / Playback / Setup); Enter on the shooting screen opens the shooting-mode guide |

So G1–G3 of §3 are passed and the front end runs every frame, but live view is still black because no YC frame is
offered to VDF. The MENU result is **not** caused by the front-end models: it also opens in a run where the imager was
never powered (P2S/AIF registers untouched); the same holds for Enter opening the mode guide. It coincides with the board change that releases the shutter lines at
reset (main tree `357e485`; before it the camera booted with S1/S2 held, which plausibly blocked the menu) [?].

### 7.3 Next gate: the YC output is never requested

* `SendYCPin` `43773EA8` (file `SyncYCOut.cpp`) runs every frame from VD_Post `4376FC48` (and from HS_Sequence
  `4376B594` when the mode state `*(*(0x43D84A40)+0x10)` is 4 or 5; it is 0 here). It returns at once unless one of the
  four YC outputs `dword_43D8AF48[0..3]` is in state 2. All four are 0.
* State 1 ("requested") is set by `43773E80(k, on)`, called from `4376FC48` with `on` = bit1 (output 0) / bit2
  (outputs 1, 2) of byte `0x43D874F4` (= camera parameter block `0x43D87298` + 604, `a3[2]+604`), gated by bit7 and
  the mode state. State 2 is then set by `437737D4` from the EE sequences (`4354B530` …) when a YC frame is produced.
* At run time byte `0x43D874F4` has bits 1/2 clear (0x00 in one run, 0x40 = bit6 only in another), so no output is
  ever requested. No direct writer of that byte was found in
  av-cam (the block is filled by the camera mode/parameter commands, `4376CFDC`, `43763528`, `436F9A70`) — the request
  is expected to come from the application/VDF side (YC pin connect for the panel) [?]. A gdb watchpoint on the byte
  slows the boot too much to be usable; an `a5ktrace` probe on the camera command dispatcher is the next step.
* So the next work item is on the request side (which command sets the YC-out bits and why the shooting app does not
  send it — e.g. a missing lens / monitoring-ready condition), not another hardware completion. The develop/resize/VFX
  IRQs 67–75/77/78 were not needed so far (their engines are not kicked, or are polled).

### 7.4 Observations / caveats

* Imager power-on (`4398DB8E` case 1 → `PowerOn 436AF630`) does not happen on every boot: on the final board 3 of 9
  runs reached the shooting screen without it (then no P2S/front-end traffic at all; earlier runs without the CEC W1C
  fix of `357e485` missed it as well). It also logs `PowerOn Wait No.01 TimerError!!`
  and `AsyncInitial: WaitTimer OpenError/StartError` (utimer 11) in some runs, yet the imager continues. Both are
  independent of the new models (seen before any P2S access) [? race in the boot sequence].
* An av-cam NULL call `PC=0, LR=4376FBD7` (list walk in `4376FBA4`, `obj->vtbl[3]`, task `liro-kliro_100`) oopses the
  camera engine in some long runs with the trace plugin loaded (seen in 2 runs; not seen in the runs without the
  plugin). Same code
  area as the oops in `RE_ENGINE_TICK.md` §11.4 [? timing race, aggravated by slow emulation].
* 4.b step 1 (test pattern): the VPH-side version was replaced by `cxd90014-aif.lv-source`, which writes guest
  memory at the RC output once VDF programs ch51 (`RE_LIVEVIEW_IMAGE.md`, Implementation status).

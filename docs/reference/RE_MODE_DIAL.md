# RE: shooting-mode "dial" and how to change the mode in the emulator (fw 1.10)

Scope: the two prior still-capture agents found the emulator stuck in **SCN** with no way to leave it
(`RE_CAPTURE_COMPLETION.md` §6.1: "The emulator has no mode-dial key ... I cannot leave SCN for P/A/M").
This document establishes **how the a5000 reads/changes the shooting mode**, why there is **no mode-dial
hardware input to model**, and gives a verified, faithful way to boot the emulator in P/A/S/M so Gate-3 can be
exercised outside SCN.

Method: Hex-Rays/objdump of the stock view libraries (`re/ida/viewUnified2/6/7/8.so.c`, decompiler output kept
in the parent `re/`, never committed), the user's `re/backup/whitelist/backup_props_all.tsv`, and live QEMU
runs (pristine NAND COW clones, HMP `screendump`). `[?]` = not proven.

---------------------------------------------------------------------------------------------------------------------

## 0. Summary

| Question | Answer |
|---|---|
| Does the a5000 have a physical mode dial? | **No.** `input.ko`'s key table (Backup 0x1120000, firmware-verified against the WBI snapshot, 18 entries — `RE_INPUT_I2C_POWER.md` §7) has **no** mode-dial source: the ADC ladders are ch 0x0C (DELETE), 0x0D (4-way + centre), 0x0F (zoom), GPIO is MOVIE / panel-rotate / shutter, the sub-micro slot-A keys are POWER/PLAY/MENU/connector/flash. There is no ADC channel, GPIO ladder or sub-micro slot for a mode dial. The a5000 (like the NEX-3N / a5100) has no mode-dial wheel on the body. |
| Then what is the "mode dial"? | A **software (on-screen) dial**: the widget `PAS_ModeDial` in the view `ViewModeDialGuide` (`viewUnified8.so`), rotated by the control wheel / arrows and confirmed with the centre button. Its position is **persisted in Backup**, not read from hardware. |
| Where is the dial position stored? | **Backup property `0x10708BF`** (1 byte).  Read live by `CmnViewBigModelUtil::getRecViewFromDial` (`viewUnified2.so` 0x1715e0): `sub_1865F0` = `Bkup_Read(0x10708BF)` → `sub_2242D0` maps it through a 19-entry table to the "RecView" (which shooting view / OSD to show). |
| Where is the actual exposure mode stored? | **Backup property `0x1070009`** (1 byte) = the exposure-mode enum (= `model/CAMERA` param **0x12A**). The selector writes both 0x1070009 and 0x10708BF on confirm. |
| How does the engine change mode (the real apply)? | On confirm the selector runs **`requestModelExecute("model/CAMERA", cmd 0x10, ParamList{ param 0x12A = <enum> })`** (plus the two Backup writes). This is the correct engine mode-change path — **not** a direct write of 0x12C and **not** `model/STILL_REC`. (This corrects `app/docs/TRACKB_INPROCESS.md` §17, whose `model/STILL_REC` / 0x12C route was ignored by the engine: the engine derives 0x12C from this `model/CAMERA` 0x10 apply.) |
| So is there a hardware input to add to `sony_a5000.c`? | **No.** The dial is driven by inputs the board already models (control-wheel rotation `cxd90014-jog`, the 4-way ADC, the centre button) through the on-screen selector. Modelling a fake hardware dial would violate fidelity (`AI_WORKFLOW` §1.3). |
| How to change the mode faithfully in the emulator, then? | Pre-set the **persisted** soft dial in the RAM Backup copy at boot (exactly what the selector persists), via the stage-1 loader (`a5kcopy -bkset`), so the camera powers up in the chosen mode. Exposed as the machine option **`-M sony-a5000,shoot-mode=p\|a\|s\|m\|scn`**. **Verified:** SCN → **P** and SCN → **M** on the OSD (§4). |

---------------------------------------------------------------------------------------------------------------------

## 1. The dial read path (firmware)

`CmnViewBigModelUtil::getRecViewFromDial` (`viewUnified2.so` **0x1715e0**):

```c
int getRecViewFromDial(this) {
    this[435] = sub_1865F0(this);                 // raw dial byte
    WrapperSettingUtil ws;
    this[435] = sub_2242D0(&ws, this[435]);       // map -> RecView
    return this[435];
}
```

* `sub_1865F0` (0x1865F0): `Bkup_Read((BackupManager*)0x10708BF, &v4); return v4;` — **the raw dial position is Backup
  property 0x10708BF**, a single byte. Nothing hardware is read.
* `sub_2242D0` (0x2242D0): `v2 = raw - 1; return (v2 > 0x12) ? 3 : dword_38ED58[v2];` — maps the raw byte (1..19)
  through a 19-entry `.rodata` table (`dword_38ED58` @ file 0x38ED58, extracted from `viewUnified2.so`):

  | raw 0x10708BF | 1 | 2 | 3 | 4 | 5 | 6..14 | 15 | 16 | 17 | 18 | 19 | other |
  |---|---|---|---|---|---|---|---|---|---|---|---|---|
  | RecView | 1 | 3 | 5 | 4 | 6 | **11** | 8 | 3 | 1 | 2 | 10 | 3 |

  RecView 11 = the SCN shooting view (consistent with `model/CAMERA` 0x12C = 11 = SCN, OSD-correlated in
  `TRACKB_INPROCESS.md` §16).

The OSD mode letter ("P"/"A"/"S"/"M") is drawn from the engine exposure mode
(`CmnViewModelWrpCameraExposureMode::getExposureModeActVal` → `model/CAMERA` 0x12A), while `getRecViewFromDial`
(0x10708BF) selects the shooting **view/layout**. A consistent mode needs both set together, which is what the
selector does.

---------------------------------------------------------------------------------------------------------------------

## 2. The on-screen mode-dial selector (firmware)

The selector is the view **`ViewModeDialGuide`** (`viewUnified8.so`, `ViewModeDialGuideToInstance` @0x2a54a;
pushed as view id `@V083`). Its dial is the widget class **`PAS_ModeDial`** with `rotateModeDial(ROTATION)`,
`setModeDialLevel(LEVEL,...)`, `setModeIcons(int)`, `getSelectItemNum()` — i.e. an on-screen rotating dial driven
by the **control wheel** (`cxd90014-jog`, host `]`/`[`) and the 4-way, confirmed with the **centre** button.

On confirm, `sub_2B8C8` (`viewUnified8.so`) dispatches on the selected position `*(view+484)` and, per case,
**writes the two Backup bytes and runs the engine apply**:

| case (`*+484`) | Backup 0x1070009 (`exp_enum` = 0x12A) | Backup 0x10708BF (dial pos) | engine apply | next view |
|---|---|---|---|---|
| 0x1C | 1 | 2 | `requestModelExecute("model/CAMERA", 0x10, {0x12A=1})` | @V083 |
| 0x1D | 2 | 3 | …`{0x12A=2}` | @V083 |
| 0x1E | 3 | 4 | …`{0x12A=3}` | @V083 |
| 0x1F | 4 | 5 | …`{0x12A=4}` | @V083 |
| 0x20 | 6 | 1 | …`{0x12A=6}` (also 0x1070A5E=6) | @V083 |
| 0x21 | 8 | word_12 | …`{0x12A=8}` (also 0x1070A5E=8) | @V083 |
| 0x22 | — | from 0x1070AAF | `model/STILL_REC_PANORAMA` cmd 0x2E | @V057 |
| 0x23 | 9 | from 0x1070AB0 | …`{0x12A=9}` | @V083 |

So the **exposure-mode enum** (`0x1070009` / `0x12A`) is the authoritative mode value; `0x10708BF` is the dial
position that feeds `getRecViewFromDial`. The panorama position uses a different model (`STILL_REC_PANORAMA`).

### Mode name ↔ enum

Verified on the OSD (§4) and from the P-A-S-M case order 0x1C→0x1F:

| enum (0x1070009 / 0x12A) | dial pos (0x10708BF) | mode | evidence |
|---|---|---|---|
| 1 | 2 | **P** Program Auto | **V** (OSD "P") |
| 2 | 3 | **A** Aperture Priority | H (P-A-S-M case order) |
| 3 | 4 | **S** Shutter Priority | H (P-A-S-M case order) |
| 4 | 5 | **M** Manual Exposure | **V** (OSD "M") |
| 6 | 1 | **SCN** Scene Selection | **V** (default boot) |
| 8 | word_12 | iAuto / Superior Auto [?] | `[?]` (also writes 0x1070A5E) |
| 9 | 0x1070AB0 [?] | iAuto / Superior Auto [?] | `[?]` |

The two Auto positions (enum 8/9) and panorama also write `0x1070A5E` / use `model/STILL_REC_PANORAMA`, so they
are left `[?]` here (not needed to leave SCN for Gate-3).

### Why Enter does not open the selector from the SCN shooting screen

On the a5000 there is no mode button; the selector is reached through **MENU → Camera Settings → "Shoot Mode"
(촬영 모드)** (settings node id 193). From the SCN shooting screen the centre button opens the **scene** sub-selector,
not the exposure-mode dial, and in the emulator that sub-view renders black (`RE_CAPTURE_COMPLETION.md` §6.1),
which is why driving the selector by keys is unreliable. The boot-time Backup route (§3) avoids the UI entirely.

---------------------------------------------------------------------------------------------------------------------

## 3. Changing the mode in the emulator — `-M sony-a5000,shoot-mode=`

Because the dial is persisted state (not hardware), the faithful board-level lever is to pre-set the **RAM Backup
copy** before the firmware reads it — exactly the two bytes the selector would have written — using the existing
`a5kcopy -bkset` path in the stage-1 loader (the same mechanism as the standard `a5kcopy -bkset 0x0051000f 01`
boot-mode line). The board cannot write Backup itself, so it passes the values to the loader on the kernel command
line.

* **`hw/arm/sony_a5000.c`**: new machine option `shoot-mode=p|a|s|m|scn`. Each name maps to `(exp_enum, dial_pos)`
  from the table above; when set, `a5k_init` appends **`a5kmodeset=<EE><DD>`** (hex) to the kernel cmdline. Default
  unset = stock (token absent, cmdline byte-identical).
* **`provision/loader.sh`**: after the boot-mode `bkset`, parse `a5kmodeset=<EE><DD>` from `/proc/cmdline` and
  `a5kcopy -bkset 0x01070009 <EE>` + `a5kcopy -bkset 0x010708bf <DD>` into the RAM Backup copy.

This touches only the RAM Backup copy of a COW NAND clone; nothing is written to flash, and a stock boot (no
option) is unchanged. `0x1070009` and `0x10708BF` are both subsystem-0x107 menu-written properties (sizes 1,
ranges 1..0x1f and 1..9 in `backup_props_all.tsv`), i.e. inside the brick-safe menu-write set.

Usage:
```
emulator/run.sh -M sony-a5000,shoot-mode=m          # power up in Manual
emulator/run.sh -M sony-a5000,shoot-mode=p          # Program Auto
```
(`run.sh` builds `shoot-mode` into the `-M sony-a5000,...` string if you pass it after the machine name, or add it
to the `M=` line.) The default `provision/loader.cpio` already contains the parser, so no loader rebuild is needed.

---------------------------------------------------------------------------------------------------------------------

## 4. Verification (QEMU, pristine NAND COW clones)

Fresh `cp -c state/a5000.nand` per run, frozen qemu copy, `-display none`, HMP `screendump` → PNG. Boot to the
shooting screen (~90 s). Only the agent's own qemu (unique NAND name / monitor port / PID) was ever signalled.

| run | how the mode was set | OSD top-left | OSD exposure line | result |
|---|---|---|---|---|
| baseline | stock (no option) | **SCN** | ISO AUTO, ±0.0 | SCN (as reported by the prior agents) |
| enum 4 via loader `bkset` 0x1070009=04 / 0x10708BF=05 | direct loader diag | **M** | 1/640, F--, ISO 250 (manual-exposure UI) | **mode changed SCN→M** |
| enum 1 via loader `bkset` 0x1070009=01 / 0x10708BF=02 | direct loader diag | **P** | ±−0.3, ISO 250, "P*" dial icon | **mode changed SCN→P** |
| `-M sony-a5000,shoot-mode=m` | the committed machine option | **M** | manual-exposure UI | **option drives the same SCN→M change** |

So the shooting mode is changed through the engine's own persisted-dial path (the OSD mode letter and the
exposure UI both follow), and the `shoot-mode` option reaches P/A/S/M from the default SCN. A/S (enum 2/3) are not
yet OSD-verified but follow the verified P/M case order.

Unblocks Gate-3: a still capture can now be attempted in P/A/M (`RE_CAPTURE_COMPLETION.md` §6.1 / Gate-3), which
the fixed-SCN state previously prevented.

---------------------------------------------------------------------------------------------------------------------

## 5. Open items / hypotheses

* **A/S enum** (2/3) OSD-unverified `[?]` — inferred from the verified P (1) / M (4) case order; a boot with
  `shoot-mode=a` / `=s` would confirm.
* **Auto / Superior Auto / Panorama** (enum 8/9, case 0x22) also write `0x1070A5E` / use `model/STILL_REC_PANORAMA`
  and are left `[?]`; not needed to leave SCN.
* Whether a cold boot in P/A/M still needs the `0x51000f=1` cold-boot diagnostic for the capture to complete
  (`RE_STILL_CAPTURE.md` §6) is independent of the mode and unchanged here.
* Driving the on-screen selector by keys (MENU → Shoot Mode → rotate → centre) is the fully in-firmware route but
  depends on the selector rendering, which is currently unreliable in the emulator (`RE_CAPTURE_COMPLETION.md`
  §6.1); the boot-time Backup route is used instead.

# A6000 custom look: where the ISP gamma really lives (verified on hardware, FW 3.21)

Status: **verified on a real A6000 running 3.21** (the camera's `/system/av-cam.bin` is byte-identical to
`firmware/unpacked/.../av-cam.bin`, md5 `ef2e3003...`). Everything here was done RAM-only: reads, and writes to RAM
and to the ISP's volatile SRAM. Nothing was written to flash except the one model-name `bk w` described in section 1.
`[?]` = not proven. Tools: `mods/look/` (see its README for the procedure).

## 0. TL;DR

* **The ISP gamma table is 96 x u32 memory-mapped hardware SRAM at PA `0xF3215300`** (CamCore CC block, base
  `0xF3200000`, offset `+0x15300`, size `0x180`). It is readable and writable from Linux through `/dev/mem` + `mmap`.
* **Writing it changes the image.** With a clean inverted curve in that SRAM, the saved photo came out inverted and the
  live view showed the inverted image (flickering back to normal until the hold loop was made fast, section 7).
* **All RAM-side tables are inert.** The tone curves found in DDR (the 0..1024 u16 tables, the 97-word u32 Creative-Style
  curve and its copy) are staging/working copies. Changing them, even held continuously, changes nothing.
* av-cam rewrites the SRAM on **events** (entering video, mode change, ...), not continuously. The fix that needs no race:
  **patch av-cam's own table writer (`0x43b67414`) to `bx lr`**, write the table once, done (section 7). Verified: S-Log2
  stays applied through live view, stills and video recording, with no flicker. One command: `mods/look/look.sh apply`.
* Recipe Lab (settings layer) cannot do gamma on the A6000 (no gamma/LUT setting). This path bypasses the settings layer.

## 1. Camera and access (what worked, what did not)

| Item | Result |
|---|---|
| Model name | The unit reported `ILCE-6001` (changed earlier with the known `bk w 0x003e0005` trick, OpenMemories-Tweak issue #134). Restored to `ILCE-6000` with `bk.elf w 0x003e0005 49 4c 43 45 2d 36 30 30 30 00 00 00 00 00 00 00`, verified by readback, survives reboot. The model name is **not needed** for any of this. |
| Transport | OpenMemories-Tweak: Developer tab, enable **Wifi**, **Telnet** (port 23, root shell) and **ADB** (port 5555). **Wi-Fi only**: the app documents no USB path, and in Mass-Storage (PID 07C4) and MTP (PID 08B7) mode Windows sees no adb interface. `pmca-console updatershell` is the only USB shell, but it runs in updater mode (no shooting pipeline), so it is useless for this. |
| Power save | Raise "Power save start time" in the camera menu. The camera drops Wi-Fi when it sleeps. |
| File transfer | **Binary pushes over telnet get corrupted** (md5 mismatch, probably IAC 0xFF bytes). Text files are fine. Push binaries with `adb push`. adb's `/` is the telnet side's `/android`, and adb runs as uid 2000 (use telnet for root: `/dev/mem`). `adb push x /x` lands at `/android/x`. |
| Shell | busybox 1.13: no `devmem`, `printf`, `head`, `tail`, `tr`, `awk`, `timeout`, `nohup`, `ps`. Use `busybox <applet>` (it has `md5sum`, `cmp`, `dd`, `sed`, `grep`, `wc`, `od`). Detach with `( trap "" HUP; cmd ) </dev/null >/dev/null 2>&1 &`. |
| RAM | `/android` is a tmpfs (RAM): files are lost at power-off. **Free RAM is tiny** (4-35 MB depending on boot). A 1 MiB working buffer plus a long scan wedged the telnet/adb shells once; a battery pull fixed it. The tool now uses a 64 KiB buffer. Keep `/android` small. |
| Linux RAM map | `/proc/iomem`: `80000000-8fffffff`, `90000000-97ffffff` and `9c000000-bfffffff` are System RAM. MMIO (`0xF3xxxxxx`) is not reachable with `read()` on `/dev/mem`; it needs `mmap` (`gammamod hwrd/hwwr`). `read()` on MMIO fails cleanly, no hang. |

## 2. Memory map facts (3.21)

* av-cam loads raw-contiguous at **PA `0x83541000`** (VA `0x43541000` = PA - `0x40000000`, Thumb-2). Verified on the
  camera: 4 of 5 sampled 4 KiB blocks (offsets 0x1000, 0x10000, 0x40000, 0x100000) are byte-identical to the file. The
  first page differs only in 764 bytes where the file has zeros and the running firmware has written variables.
* The same addresses held after a reboot (tables and code do not move).
* av-cam is position independent: data and strings are reached with `ldr rX,[pc,#imm]` + `add rX,pc` (88,547 such
  references). Log strings have **no plain VA literals**; an xref tool must resolve the PIC pair (the one used was
  `picxref.py`, a few lines of Python on top of capstone; not committed).

## 3. What was ruled out (RAM tables)

1. **Five `0..1024` u16 tables** (`0x83bef5f2` 130 pts; `0x83bfa932`, `0x83c1dfa2`, `0x83c1e72e`, `0x83c1e850` four
   identical 129-pt tables + 0 pad). Inverted with `write`, with `hold` (continuous), at the same time, with a still
   photo, and after Creative Style changes: **no effect, and nothing refills them** (a written value was still there
   after a second). They are static source copies from the firmware image.
   * `make_curve.py` had a bug for the 129-pt + trailing-0 layout (it took the pad as the endpoint and produced an
     all-zero curve). Fixed.
2. **The Creative-Style curve**: `0x83eac668`, 97 x u32, range 0..1023 (`0,15,30,47,67,84...` in Standard, `1,11,21,31,46,58...` in
   Vivid), with an identical copy at `0x83ea5e24`. It is **computed at run time** by the camera app from the Creative
   Style parameters (the shape is not in `av-cam.bin`) and rebuilt on every style change (a restore write is overwritten
   when the style changes). Held inverted for 60 s and 180 s together with a photo, a playback round trip and a style
   change: **no effect**. It is a working copy, not what the ISP reads.
3. **A whole-RAM pattern search** (`gammamod find`, 8-sample sequences of the Standard curve in u32, u16, u16 x4/x16/x64
   and u32 x4/x16/x64): only the two copies above exist; every other hit was the search tool's own buffer or the
   pattern file's cached page.
4. **A whole-RAM change diff** (`gammamod scan` / `scan32` print monotone ramps with an FNV checksum; Standard vs Vivid):
   of ~580 ramps only two changed: the curve above and a 291-word piecewise table at `0x862b1afc` (values 0..1231,
   top end 1173 -> 1198 with Vivid). The latter is not a clean gamma [?]; its purpose is unidentified.
5. **Colour matrix constants** (Recipe Lab: PP3 matrix `1331,-307,-51,-205,1331,-123,-20,-461,1485`, Q10) exist in RAM
   only app-side: i16 at `0x815c2132` and i32 at `0x8539e66c` (outside av-cam). [?] not confirmed as the live matrix.

## 4. Static RE: how av-cam programs the ISP

All addresses are av-cam VAs, 3.21.

* **Module `[CAMERA_MAIN_CC]`** (`0x4379f000`-`0x437a2800`, `0x4381d334`): `SetDevelopParam` `0x4379f194`, `LutMode Change
  CAP/MOV/MON/SA` `0x437a1bd8..1c0e`, `Copy2DLUT`/`Copy3DLUT` `0x437a233c..2424` (both copy `0x654`-byte blocks),
  `CamCore 3D-Lut table Write Start/End` `0x4381d2ec..d31c`. The log-string layer has no "gamma" strings at all.
* **Register-level driver for the CamCore CC block** (`0x43b66xxx`-`0x43b69cbc`). Handle `r0` holds the hardware base
  (`ldr r0,[r0]` = `0x43acee62`). Table writes go through one MMIO word-copy routine `0x43b7f878(dst, src, nbytes)`
  (92 call sites). Layout (offsets from the base):

  | Offset | Size | What |
  |---|---|---|
  | `+0x15300` | `0x180` (96 x u32) | **gamma table** (function `0x43b67414`, wrapper `0x43b6747c`, 7 callers `0x438535a4..0x4386b1d0`) |
  | `+0x15400` | regs `+0x80,+0x84,+0x88,+0xb8` | small gamma-block controls (`0x43b673ec..0x43b6740a`) |
  | `+0x15700` | `0xa0` | table [?] (`0x43b69b22`) |
  | `+0x15b08` | `0x70` | table [?] (`0x43b685d8`) |
  | `+0x16200` | `0xa80` (7 x 96 words) | tables [?] (`0x43b68606`) |
  | `+0x17000..0x17800` | `0x12c,0xf0,0xc0,0xc8,0xa0,0x80...` | the **3D LUT banks** (8 banks of the 0x654-byte LUT; set by `0x43b67a68`, bank writer `0x43b67918`) |
  | `+0x15d40` / `+0x15d44` | reg | LUT write enable (set 1 before, 0 after the 3D LUT banks) / ready bit [?] |

* **The base is `0xF3200000`.** The repo's own boot log `docs/reference/io_after_boot.txt` shows the stock firmware
  writing `0xF3215300, +4, +8, ...` with `0xf, 0x1e, 0x33, 0x43, 0x54`, i.e. the gamma curve, into that SRAM
  (and `0xF3212xxx` control registers before it).
* The A6000 has no gamma-named strings and no Picture-Profile engine (consistent with Recipe Lab's FAQ), but the gamma
  SRAM and its driver are there; they are used for the Creative Style / DRO tone curve.

## 5. The live gamma SRAM: contents

Read with `gammamod hwrd 0xF3215300 96` (stable across reads; Standard Creative Style):

```
15 30 63 67 84 101 118 143 156 167 183 194 223 253 239 253 259 283 287 295 307 ... 1022 1023 (x9)
```

* It follows the RAM curve (`curve[1..96]`: `15,30,51,67,84,101,118,137,...`) but is **not monotone**: small outliers
  (`63` vs `47..51`, `143` vs `134`, a few `1023` spikes around words 56 and 68). Deterministic, so real content, not read
  noise. Likely a dithered or per-entry-adjusted version of the curve [?].
* Restore = write back the 96 words saved with `hwdump` (not the RAM curve).

## 6. Hardware write test (the result)

Procedure: save the 96 words, write `1023 - curve[1..96]` (a clean inverted monotone curve) to `0xF3215300`, watch.

* **The saved still image was inverted.** The SRAM is the real ISP gamma.
* **Live view flickered between normal and inverted** while a timer loop rewrote the table every 8 ms.
* Restore (write the saved words back) works. The stock table is **not one fixed set of 96 words**: it varies with
  scene/exposure (the saved copy differed from a later read from word 2 on), so "restore" means "let the camera
  re-program its own".

## 7. The two ways to make it stick

### 7a. Polling (works, but races)
`gammamod hwwr <pa> <file> <ms>` writes once, then polls four words and rewrites the whole table whenever any differs
(`sched_yield`, real-clock bound). It reported ~3,000 "overwrites" in 60 s, but a later control showed the camera does **not**
rewrite at idle (a written table stayed put for seconds with no poller, and a table was still intact 1 s after a hold
ended), so most of those counts were probably read glitches while the ISP uses the SRAM [?]. Video recording still
flickered with this approach.

### 7b. Patch the camera's table writer (the real fix)
* Events (e.g. starting video) make av-cam re-program the gamma: after a video attempt the SRAM held a *different* table
  (the camera's movie curve, starting `111,126,175,211,...`), not the still curve (`15,30,63,67,...`) and not ours.
* Every caller goes through one leaf function, **av-cam VA `0x43b67414`** (PA `0x83b67414`): `add.w r0,r0,#0x15000;
  mov.w r2,#0x180; add.w r0,r0,#0x300; b.w 0x43b7f878` (MMIO memcpy). Bytes on this unit `00f5a8304ff4c072` equal the
  3.21 firmware file (checked before patching).
* Patch = one aligned 32-bit store of `70 47 a8 30` (`bx lr` + the untouched second halfword). The function returns
  immediately, so the camera can no longer overwrite the table. RAM-only, reversible (write `00 f5 a8 30` back).
* **Result: S-Log2 written once stays applied through live view, stills and video recording, with no flicker.** After a
  video recording the SRAM still equalled the S-Log2 table byte for byte.
* No explicit instruction-cache maintenance was needed in practice (the patch took effect immediately) [?: not proven
  for every case; if the old code ever keeps running, the polling loop is the fallback].
* Revert: restore the 4 bytes; the camera re-programs its own curve at its next event.
* Limit: this blocks *all* camera gamma programming (Creative Style, DRO and movie curves stop updating) until reverted.

`mods/look/look.sh apply|revert|status` does the whole sequence (verifies the writer bytes first, refuses if they differ).

## 7c. Look generator: S-Log2 and friends

The 96 hardware knots are fixed linear 11-bit input positions: `2..64` step 2 (32 knots), `68..128` step 4, `136..256` step 8,
`272..512` step 16, `576..1024` step 64, `1152..1920` step 128, `2047` (this exact axis was found in RAM at `0x84071dc4`).
The SRAM holds the 10-bit output at each knot. `mods/look/make_hwgamma.py` evaluates a transfer function there:

* `slog2` (Sony formula, validated against the published points: 0% -> 90, 18% grey -> 347, 90% white -> 581/582 in
  10-bit legal range), `slog3`, `rec709`, `identity`. `--range legal|full`, `--grey-x N`.
* Scene reflectance is `R = 0.18 * knot / N` with `N` = the knot where 18% grey lands at your exposure (default 76: the
  knot where the stock curve gives ~46%). Sensor clip (knot 2047) is fixed, so there is **no extra dynamic range**:
  with N=76 clip lands at ~83% output. Raise/lower N (or expose differently) to move grey vs. headroom.
* Not reproduced: S-Gamut (a colour matrix, not a curve) and the 8-bit/profile specifics of Sony's real S-Log2 path.
* First hardware result: S-Log2 (legal, N=76) looked flat/log-like in live view, stills and video; steady with the patch.

## 8. Gotchas

* **`hold` timing in the old DDR `write`-path counts sleep requests, not time**, and the camera's `nanosleep` rounds up, so
  a "90 s" hold ran for minutes. The hardware path (`hwwr`) uses the real clock.
* While the writer is patched, Creative Style / DRO / movie curve changes do not reach the ISP. `look.sh revert` first.
* After a battery swap or reboot `/android` is empty and the patch is gone (RAM): everything is stock again.
* Low free RAM: keep buffers small; never run big scans with large files on `/android`.
* Writing `-F` outside `0xF3215300..0xF321547F` is blocked by default in `hwrd/hwdump/hwwr`; the other tables in section
  4 are untested and must be read first.
* Battery pull = stock. SRAM is volatile; nothing persists.

## 9. Open items

1. Format of the SRAM outliers (why non-monotone), and whether the ISP interpolates the 96 words over the full input range.
2. The three other tables (`+0x15700`, `+0x15b08`, `+0x16200`) and the 3D LUT banks (`+0x17000..0x17800`): read, then test.
3. Colour matrix: find where the matrix reaches the hardware (likely the same CC block); the RAM copies at `0x815c2132` /
   `0x8539e66c` are app-side only.
4. Still vs movie: the camera's movie table differs from the still one (saved as `movie_tbl.bin` during testing, not committed).
   A per-mode look would need two tables and a mode switch.
5. Instead of blocking the writer, redirect its source pointer so the camera itself writes *our* table (keeps Creative Style
   etc. working on other tables). The a6300 has the identical writer (`0x43c2b91c`, wrapper `0x43c2b984`).

## See also
* `A6000_ISP_REGISTER_MAP.md` - every ISP register/table we know, colour matrix at `0xF321550C` (verified), writers to patch.
* `A6000_PICTURE_PROFILE_ISP_PLAN.md` - plan to implement a6300 Picture Profiles and other cameras' looks.
* Look generators: `mods/look/make_hwgamma.py` (slog2/slog3/logc3/arri709/rec709), `mods/look/make_hwmatrix.py` (awg3/sat).

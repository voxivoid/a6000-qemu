# RE: why YC output 0 never reaches state 2 — the EE/Image pipeline and the SYSV V phase (fw 1.10)

Follows `RE_LIVEVIEW_REQUEST.md` §5/§9 and `RE_SENSOR_ISP.md` §7. Method: Hex-Rays on a private copy of
`C:\a5000\emu\sensor\camfe.i64` (`C:\a5000\emu\lv2\lv.i64`, restored from `pristine.i64` before every query), plus
dynamic runs of the unmodified firmware on pristine NAND clones (`qemu-system-arm-3a51630`, the last check also on
`-c63c096`) with small `a5ktrace fn=` probe sets, HMP `xp` of guest RAM / SYSV / VPH registers, and gdb used **only as a
diagnostic** (§4). Addresses are av-cam VAs (PA = VA + 0x40000000) unless MMIO. `[?]` = not proven.

---

## 0. TL;DR

* The pipeline does **not** wait for a missing completion IRQ. The develop/resize IRQs 67–75, VFX 77/78, unmodelled
  AIF groups, the front-end units 35/37 and JPEG/IPU blocks are **not needed** to reach YC state 2 and to program VPH
  ch51 (§4: the whole chain ran with none of them modelled).
* **The blocker is timing: the SYSV "V" in the model does not match the engine frame.** The SYSV block (0xF3402000)
  counts and toggles its phase bit at 60 Hz. The engine frame (evsel source 29) runs at 33 367 µs. So at each engine
  frame start the SYSV frame counter has advanced by **2**, and `+0x28` bit0 (the V phase) has the **same value as in
  the previous frame**.
* While `dword_43D872C4 = 2` (2V mode), VD_Pre checks that phase on every frame against its own V counter
  (`dword_43D87478 & 1`, §2.2). With a constant phase the check fails on **every** frame. VD_Pre then re-initialises
  the camera mode (`4376A168` → `437697E0`), which sets the refresh flag `byte 0x43D84A1C = 1`. The next VD_Post
  (`4376FC48`) sees that flag and **cancels every queued Image** (`43A72D86` → `43A72D3E`, then `437E36DC` frees it).
  The EE sequence `43546E40` runs stage 0 and is killed before stage 1. Stage 1 is the only place that calls
  `437737D4` (YC state 2).
* A second consumer of the same counter blocks next. VDF's `UpdateYuvThrough` (`4367EC50`) only hands the YC pin to VPH
  while the **SYSV frame counter equals the pin's frame number**. Late pins are dropped with error 5. With the 60 Hz
  counter the pin was always 1–2 counts behind (§3.3).
* **Proof (r4/r5).** gdb toggled the phase bit once per engine frame at the snapshot store. YC state 2 was then reached
  (`437737D4` from `435475F5`), and VDF received pins. With the VDF frame comparison also forced (r5), the chain ran to
  the end: `VphUpdateYuvCall 43661BC8` → `438F6CF4` (every frame), **VPH `+0x144` = `0x01000233` (bit0 set)**, and
  ch51 was programmed: `0xF3441220 = 0x8CE00000`, `+0x24 = 0x400`, `+0x28 = 0x80`, `+0x2C = 0x203`.
* **Spec (§5).** Clock the SYSV V from the engine frame. One V = 33 367 µs. Each V increments `+0x98`, toggles `+0x28`
  bit0, pulses IRQ 98 and fires evsel source 29 in that order. The panel output vsync (IRQ 101/102) is decoupled from
  it and stays at 60 Hz [?].

---

## 1. The EE sequence is a staged Image functor

| fact | evidence |
|---|---|
| VD_Sequence `4376BDBC` calls `4376FC48(a1, a2, &off_43D84A3C)` with `a1 = *(u8*)(0x43D84A18+4)` (refresh flag, `43A5D39E`) and `a2 = &dword_43D84B60` (the EE functor) only when `43A72304(0x43D84B5C, 0x43D84D04)` holds, `dword_43D84B74 == 0` and `byte_43D8747D != 0` | disasm `4376BE68..4376BEB8` |
| `4376FC48` with `a2 != 0` creates an Image (`437E3910`: pool `dword_43DD3348`, stage byte `+35 = 0`, runnable `+34 = 1`, queue `+4 = 0x43D89580` for type 0) and runs the queue `437E3AA8(0x43D89580)` in the same call | decompile `4376FC48`, `437E3910`, `4376F898` |
| the queue runner calls `43A72C46(img, …, img+34)`: if `+34 == 0` the image is **freed** (`437E36DC`). Otherwise the functor `43A72C1C(img+8)` runs: return 0 → `++stage` and requeue; 1 → requeue; 2 → requeue after a delay; 3/6 → keep; otherwise free | decompile `43A72C46` |
| `4376FC48` with `a1 != 0` and (`byte 604 & 8 == 0`, true here: 604 = 0x03) calls `43A72D86(0x43D89580)`, which sets `+34 = 0` on every queued image (`43A72D3E`) → they are freed at the next run | decompile `4376FC48`, `43A72D86`, `43A72D3E` |
| EE stage selector = `*(u8*)(img+35)` (`43A6EAFC`). Stage 0 (`43546E92…`) posts jobs (`437DFA68` on list `0x43D88F44`, run by HS_Sequence `4376FB3C`) and returns 0. **Stage 1** (`43546FA4…`) calls `437737D4(0, …)` at `435475F0`, only if `byte (v4+0x208) != 0` (`435474C8`). Stage 3 is the capture/branch part | disasm `43546E40…4354785C` |
| Stage 1 bails out (return 5 = image freed) if `43769AB0(&0x43D84A18, …) == 0`. That function needs the refresh object's exposure counter `+20 > +12` (`43A5D3D0`): +12 = frames to skip after a mode change (from `cam+188`, set by `437697E0`), +20 is incremented by the stage-0 job `437DFA68` (`43A5D3C2`) | decompile `43769AB0`, `437697E0`, `437DFA68` |

So YC state 2 needs **several consecutive engine frames without a mode re-init**: Image created → stage 0 → stage 1
(after the skip count) → `437737D4`.

## 2. Why the images are cancelled every frame

### 2.1 Run time (r2, r3, unmodified firmware, `-c63c096` in r6)
* `4376beb8` (`a1` of `4376FC48`): `r0 = 1` in 53 of 56 on-time VD_Sequence runs (r3), 55 of 58 (r6). Images were
  created only in the 3 runs with `r0 = 0`, ran stage 0 at once, and were cancelled and freed at the next on-time
  VD_Post (r2: `img_cancel 43A72D3E`, `img_free 437E36DC`). Stage 1 ran once in r2 and never reached `437737D4`.
* `437697E0` (sets `+4 = 1`, "refresh") is called from `4376A168` with **lr = `4376D937`** on **every** VD_Pre
  (r3: 300/300; r6: 200/200).
* The refresh flag is cleared only at frame start (`4376C3B8` → `43A5D3A2`) when `byte_43D8747D = 1`. VD_Pre sets it
  again in the same frame.

### 2.2 The 2V phase check at `4376D904` (VD_Pre `4376CFDC`)
```
if (dword_43D872C4 > 1) {                         // V per sensor frame (2 here; HMP: 0x83D872C4 = 2)
    ph = cfg->+0x0C;                              // cfg = *(off_43D84A40) = 0x43D87254
    if ((dword_43D87478 & 1) != ph) {
        dword_43D87478 = dword_43D87488 = ph ? 1 : 2;
        4376A168(0);                              // mode re-init -> 437697E0 -> refresh flag = 1
    }
}
...
byte_43D8747C = (dword_43D87478 <= 1);  byte_43D8747D = (dword_43D87478 == 0);   // 4376DA9A..4376DAB6
dword_43D872D4 = 437DD1DC(..., parity = (dword_43D87478 + ph) & 1);              // 4376DCA0..4376DCBA
```
Frame-start callback (evsel source 29 → `4376C1E4`; the code at `4376C2C0..4376C3CC`):
```
437696FC: cfg+0 = SYSV +0x2098 (frame counter), cfg+0xC = SYSV +0x2028 & 1   (snapshot 43AB5486)
if (byte_43D8747D) { dword_43D872C4 = dword_43D87478 = dword_43D872D4; clear refresh (43A5D3A2); }
if (dword_43D87478) dword_43D87478--;
```
With `72D4 = 2` the counter runs 1, 0, 1, 0 … as seen by consecutive VD_Pres (reload 2 → 1, then 0). **The check passes
only if `cfg+0xC` alternates 1, 0, 1, 0 with consecutive engine frames.** That is, SYSV bit0 must toggle once per
engine frame and be 1 on the first V of each sensor frame. A single re-init locks the counter to the phase, after which
no further re-init happens.

Run time (r3, r6): `cfg+0` and the VD_Pre argument increase by **2** per engine frame (e.g. `2a54, 2a56, 2a58 …`),
sometimes by 1 or 3. `cfg+0xC` is the same in long runs of frames (HMP samples: 0 for > 6 s). The re-init records
show `r1 = (7478 & 1)` and `r2 = new value` alternating between the pairs (1,2) and (0,1), which is a mismatch in both
phases. VD_Pre runs without a re-init only where the counter step was odd (`2a52`, `2a59`, `2a5c`, `2a65` …).

### 2.3 Why "V" must be the engine frame (evidence that 1 V = 33 367 µs)
1. The engine identifies its frames by the SYSV counter (`cfg+0`, `437696FC`), and all its logs count in "V"
   (`VD_Sequence: start delayed %dV => %dV`, `HS_Sequence: … %dV #%d`).
2. int_HS divides one V into 8 HS slots of `cfg+0x2C = 0x104A` µs = `cfg+0x24 / 8`, with `cfg+0x24 = 33 367 µs`
   (`RE_ENGINE_TICK.md` §1–2; "HS jump %d %d/8V", `4376C358`).
3. The 2V check of §2.2 is consistent only with one SYSV tick per engine frame.
4. VDF compares the same counter (written by the SYSV IRQ 98 handler `43655A90` into `dword_43D55EF8`) with the engine
   frame number in the YC pin (§3.3).

This contradicts the note in `RE_ENGINE_STABILITY.md` §1.7(3). Running SYSV at 59.94 Hz and firing source 29 on every
2nd tick would make bit0 constant at each frame start. That passes the boot copy gate (§1.4 there), but it fails the
steady-state check of §2.2 on every frame.

## 3. Downstream: VDF and VPH

### 3.1 YC state 2 → SendYCPin → VDF
After `437737D4` (`dword_43D8AF48[0] = 2`), `SendYCPin 43773EA8` (VD_Post) sends the pin. VDF task 10 runs
`SystemCmdPinSend::YcPinExec 43672FC4` → `VdfInputCmdUpdateYuvPin 4367F700` → (mode `& 0xF0 == 0x20`)
`UpdateYuvThrough 4367EC50`. That function is a sub-state machine on the pin (`vtbl+84`): 0x20 setup
(`4367E1B8`, `43680F1C`, `4366ACEC`) → 0x21 (`4367E264`) → 0x22 → `VphUpdateYuvCall 43661BC8` → 0x24.

### 3.2 Frame gate in `4367EC50` (disasm `4367ECD8..4367ED00`)
`cur = dword_43D55EF8` (SYSV frame counter at the last IRQ 98, read by `43655B54`; Hex-Rays shows that function
wrongly as returning zeros). `pin = pin->vtbl[0x58]()`.
* `cur == pin` (`43A2B9F0`, 24-bit) → process.
* `pin > cur` → requeue (wait).
* `pin < cur` → return 5 without a log. The caller logs `43A23668(…, 5, 0x5000000B, 541)`.

The phase `word_43D55F00` (SYSV `+0x28` bit0) is also used in state 0x22 (`v88`) [? exact role].

### 3.3 Run time
* r4 (phase toggled by gdb, §4): pins reach `4367EC50`, but every pin is dropped: `cur/pin` = `43d8/43d6`,
  `4416/4414`, `4527/4525`, `468e/468d`, `4724/4722` … (delta 2, sometimes 1, i.e. one engine frame of the 60 Hz
  counter). The `vdf_err` line is 541 with code 5 every time.
* r5 (the same, plus `4367ECFA` forced "equal"): `43661BC8` and `438F6CF4` run (the setter about every frame, task 43).
  VPH `+0x144` changes `0x03000232 → 0x01000233`. ch51 = `{+0x00 2, +0x20 0x8CE00000, +0x24 0x400, +0x28 0x80,
  +0x2C 0x203}`. The Y address matches the resize ch3 output `0x0CE00000` of `RE_SENSOR_ISP.md` §7.2 [? bus alias of
  the same buffer].
* Expected with the real relationship [?]: the pin carries the V in which its frame was produced. VDF handles it
  within the same SYSV V because nothing advances the counter inside an engine frame. Not proven, because gdb cannot
  change the model's counter rate. Acceptance check 3 in §5.3 settles it.

## 4. Experiments (diagnostic only, not a fix)

| run | binary / setup | result |
|---|---|---|
| r2, r3 | 3a51630, probes only | re-init every VD_Pre. Images cancelled. `437737D4`, `43661BC8`, `438F6CF4` = 0 |
| r4 | gdb at `43769732` (the `STR R3,[R4,#0xC]` of the snapshot): `r3 = t; t ^= 1` per hit | re-inits become rare (they remain where the slowed engine skips frames). Up to 8 images live in parallel. Stage 1 runs. **`437737D4` hit** (10 in 60 s). VDF receives pins. Pins dropped by the frame gate (§3.3) |
| r5 | r4 + gdb at `4367ECFA`: `r0 = 1` | **full chain to VPH ch51** (§3.3) |
| r6 | **c63c096** (with the Sgc1 latch fix), no gdb | same as r3: 200/200 re-inits, 0 state-2 hits, `+0x144 = 0x03000232` |

Notes:
* A breakpoint at `4376D90A` cannot bypass the check by setting `r2`: the flags are already set at that point. Use the
  `+0xC` store instead.
* Twice, a VM pause (gdb detach) left the engine with VD_Sequence (task 101, CPU3) never running again and HS_Sequence
  always "start delayed". This matches the CPU3 spin fixed in `c63c096` (`RE_PLAYBACK_SWITCH.md`) [?]. r6 did not show
  it.
* The "start delayed" warnings are not the blocker. In r3 VD_Sequence was on time in about 1 of 5 frames under the
  plugin, which is enough for images to progress once the re-init is gone (r4).

## 5. Spec for the emulator

### 5.1 What to change (hardware model only)
**Block:** SYSV in `hw/display/cxd90014_vph.c` (PA 0xF3402000; the firmware reads it as 0xF3400000 + 0x20xx), plus the
evsel source-29 timer in `hw/misc/cxd90014_evsel.c`.

1. **One V clock.** Today one 60 Hz `vph_tick` drives SYSV and the panel vsync, and evsel has its own 33 367 µs timer
   for source 29. Instead, make SYSV the master: period `T_V = 33 367 µs` (= `cfg+0x24`, 1001/30 ms) [? period
   register: `+0x3C` was 64800 at "Liro Start" and 27000 later (`4376EA80`); its unit is not decoded]. On each V, in
   this order:
   1. `+0x98` (frame counter) `+= 1`. `+0x94` (sub-count) can stay as it is today.
   2. `+0x28` bit0 = new counter `& 1` (or simply toggle). Bits 1–2 stay 0 [?].
   3. Pulse IRQ 98 (level, as today).
   4. Fire evsel source 29 (frame start, `4376C1E4`), with the same capture semantics as now (`+0x20+4k` = µs
      counter). Sources 28/30/23 keep their current per-frame behaviour.
   Every source-29 event must see a counter exactly 1 higher than the previous event. Source 29 must not drift
   against SYSV.
2. **Panel vsync** (IRQ 101/102, the AIF ch51–58 latch, the compose timer) stays on its own 60 Hz timer [? the real
   relationship between panel refresh and SYSV is not decoded; VDF waits for 101/102 independently of 98].
3. **Polarity.** None is needed. A wrong phase costs one re-init (§2.2), after which the counter locks.

### 5.2 Not needed (checked)
Models for IRQs 67–75, 77/78, the remaining AIF groups, IRQ 35/37 arming, and JPEG/IPU are not on the path to YC
state 2 and ch51 programming (r5 reached ch51 without them). They may matter later for image *content*
(`RE_SENSOR_ISP.md` §4.b).

### 5.3 Acceptance (unmodified firmware, pristine NAND, probes `4376A168`, `437737D4`, `43661BC8`, `438F6CF4`)
1. HMP `xp /2wx 0x83D87254`: `cfg+0` increases by 1 per engine frame, and `cfg+0xC` alternates.
2. `4376A168` with lr `4376D937`: at most a few hits after the shooting screen appears, not one per frame.
3. `437737D4` hit from `435475F5`. Then `43661BC8` and `438F6CF4` hit. No `43A23668` hits with `r3 = 0x21D` and
   `r1 = 5` for most frames.
4. `xp /2wx 0xF3440140` → `+0x144` bit0 = 1, and `0xF3441220` (ch51 Y address) is non-zero. With
   `-global cxd90014-aif.lv-source=bars` (formerly `cxd90014-vph.pattern=bars`, now removed) the bars become visible.
5. Regression: the boot still passes sequencer step 12 (`RE_ENGINE_STABILITY.md` §1, the copy gate needs
   `cfg+0xC == 1` on some frames, which alternation provides) and step 42.

## 6. Open questions
1. Real SYSV V period in this mode and the meaning of `+0x3C` (64800 → 27000), `+0x2010 & 3` (`43AB51BC`, mode, capped
   at 2 in `43655A90`), and `+0x2034/+0x2038` (written by `436A7284` as `2/1` when V-per-frame > 1). Is the real V
   33 367 µs in 2V monitoring, and does the panel run at 59.94 Hz from a separate generator?
2. Why monitoring runs in 2V mode (`dword_43D872D4 = 2` from `437DD1DC` / `43A4220E`; `cam+476 = 1`). With 1V,
   §2.2 is skipped, but the VDF frame gate (§3.2) still needs one SYSV count per engine frame.
3. Whether the VDF frame gate passes with the §5.1 model without further changes (pin = the current V?). The pin
   frame number's origin (`SendYCPin`: `dword_43D8A8E8 = (V − 1) & 0xFFFFFF`, and others) was not traced.
4. The role of `word_43D55F00` (phase) in `UpdateYuvThrough` state 0x22.
5. Image content: ch51 points at `0x8CE00000`, which the resize engine RC ch3 writes (`0x0CE00000`) [? alias]. That is
   the natural injection point for option B2 of `RE_SENSOR_ISP.md` §4.b.

## 7. Reproduce
* IDA: `C:\a5000\emu\lv2\q.py` (copy of `ycreq\q.py`, paths changed). Copy `pristine.i64` → `lv.i64` before each run.
* Probes (`a5ktrace fn=`, `#cap N` lines): `4376BEB8 vdseq_fc48` (r0 = refresh flag, r1 = functor), `4376A168
  modeinit`, `437697E0 refresh_set`, `4376C3B8 refresh_clr`, `43546E80 ee_stage` (r0 = stage), `43A72D3E img_cancel`,
  `437E36DC img_free`, `435474CC st1_208`, `437737D4 yc_state2`, `4367ECDC`/`4367ECE8` (cur/pin), `43A23668 vdf_err`
  (r3 = line), `43661BC8`, `438F6CF4`.
* Diagnostic gdb (do not detach while the engine runs; kill QEMU instead):
  `break *0x43769732` / `commands; silent; set $t = 1 - $t; set $r3 = $t; continue; end`, and optionally
  `break *0x4367ecfa` → `set $r0 = 1`.

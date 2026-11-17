# RE: nondeterministic engine failures — sensor power-on and the av-cam NULL call (fw 1.10)

Two intermittent failures of the unmodified firmware in the emulator. Each has its own root cause and its own fix.
Addresses are av-cam.bin VAs (VA = PA − 0x40000000, Thumb-2) unless they are MMIO (0xF…). Static RE used Hex-Rays on
windev (copy of `camfe.i64`). Dynamic RE used the `a5ktrace` plugin (`pc=` probes), HMP `x`, and one gdb memory write
to an MMIO register (not GIC). Each boot ran on a fresh clone of the pristine NAND. `[?]` = not proven.

Related docs: `RE_ENGINE_TICK.md` (evsel 0xF3C30000, int_HS, §11.4 oops), `RE_SENSOR_ISP.md` §7.4 (symptoms),
`RE_AVCAM_IRQ_BOOT.md` §6 (SYSV registers), `RE_LIVEVIEW_REQUEST.md` (mode-sequencer stall at step 12/33).

---

## 0. TL;DR

| # | symptom | root cause | fix (model) |
|---|---|---|---|
| 1 | in some boots the imager is never powered (no P2S, no IRQ 36/38) | The av-cam **mode sequencer** stalls at step 12 because VD_Pre never copies the step's requests to the modules. In "2V" frame mode, VD_Pre only copies on a frame whose **SYSV 0xF3402028 bit0** (frame phase) is 1. The SYSV model returns a constant 0. Whether a boot is hit depends on whether the first camera request lands before or after the engine switches to 2V mode (a race). | **SYSV +0x28 bit0 must toggle** (V phase). Verified live: writing bit0 in a stuck boot unblocks it, the imager powers on, and the sequencer reaches step 33 like a good boot (§1.5). |
| 2 | `PC=0, LR=4376FBD7` oops in `liro-kliro_100`, and the §11.4 oops at `4376FA2x` in task 66 | **evsel comparator double fire.** The IRQ 84 handler disarms a comparator by writing `+0x114` (the compare value, already in the past) and then `+0x110 = 0`. The model re-evaluates the comparator on the `+0x114` write while it is still armed, and so fires again at once if the main loop runs between the two stores. The second `int_HS` makes count 8 ≠ 7, which logs "int_HS timing error" and runs the error path. That path posts a clear of the VD-callback lists to task 66 (CPU2), which then races task 100 (CPU0) walking the same unlocked `std::list` → use-after-free → `obj->vtbl[3] == 0`. | **Writes to `+0x114+8k` only latch the value.** Only a write to `+0x110+8k` arms (bit0 = 1) or disarms (0) the comparator. In 2 traced runs, 645 of 645 timing errors were same-argument double fires (§2.4). |

The cxd4115 one-shot START fix (`qemu-system-arm-timerfix`) removes `PowerOn Wait No.01 TimerError` /
`AsyncInitial: WaitTimer Open/StartError`, but it does **not** fix problem 1 (§1.6).

---

## 1. Sensor power-on is unreliable

### 1.1 What powers the imager
| step | code | evidence |
|---|---|---|
| imager power command | `4398DB88` (via EvtDrvnTsk task 90): byte `[r0+4]` 1 = `PowerOn 436AF630` + `AsyncInitial 436AFE18`; 2 = `PowerOff 436AF414` | disasm; probe hits in task `liro-kliro_90` |
| posted by | `4398E168` ← state machine `4398E1B8(…, new_state)`: state **0x10 → power on**, 0x12 → power off | disasm `4398E2AC/4398E2BC` |
| state comes from | `437A3F40` maps camera mode code `*(msg[0]+0x10)` through table `0x43B6F09C` (`0x0A10 → 0x10`, `0x0A11 → 0x11`, `0x0A12 → 0x12`, `0x0A82 → 0x82`, `0x0A83 → 0x83`, …). It is called every frame from VD_Pre (`4376CFDC` → `437A3E94` → task 85) | dwords dump; probes: 1 call per frame |
| mode code = module-9 word of the **mode sequencer** | VD_Pre copies the sequencer's request words `0x43DC7120 + 2·m` (`437D86D4(m)`) into the camera-state block `word_43D86D18[m]`. m = 9 is the imager word (`word_43D86D28`) | `4376D2F0..4376D37C` |

So the imager powers on only when the mode sequencer step that carries `0x0A10` is applied **and copied** by VD_Pre.

### 1.2 The mode sequencer (`dword_43DC7104`, "s_mode_sequencer")
* Object: +0x10 table pointer, +0x14 length, +0x18 index, +0x0C "busy", +0x1C + 2m request word of module m (13
  modules), +0x5C + 2m response word of module m.
* `43A70EC0` applies table entries up to the next entry with bit15 set: an entry `v` sets module `v >> 8 − 1`'s request
  (`+26 + 2·(v >> 8)`). An entry with bit15 set is a **wait mask**.
* `437D8810` "CheckReturnAllModlueResponse": the sequencer advances only when request == response for every module in
  the mask. It is called from HS_Main (`4376ABFC` → `437D8708`).
* Responses are written by HS_Main (`4376B216..4376B294`, `437D86F4(m, status)`), but only in the HS slot where
  `byte_43D887AE` = 1 (the sub-micro "slot A permit" byte, RE_ENGINE_TICK §2). They report the modules' **current**
  state words (e.g. module 9 = `*(off_43D84A88)`). A module changes state only after VD_Pre has handed it the new
  request.
* The startup table at `0x43B709C6` (43 entries), selected by the request `CAM_SyncRequest(0x3003)` (`4375C920`,
  identical in good and bad boots):
  ```
  0: 105 285 320 400 500 600 780 910 A10 B00 C01 D00   wait 0x347  (index after = 12)
  13: 281 323 480 702 920 A11                          wait 0x206
  20: A82                                              wait 0x200
  21: wait 0x0A00 … 25: A83 …                          … 33: wait 0x0238 (good boots stop here, see RE_LIVEVIEW_REQUEST)
  ```

### 1.3 Good vs bad boot (live state, HMP `x`)
| item | good boot (q6) | bad boot (f1y, q2, u1) |
|---|---|---|
| sequencer index `0x43DC711C` | 0x21 (33) | **0x0C (12)**, waiting on mask 0x347 |
| requests `0x43DC7120…` | step-25 words (`11A 282 326 481 580 680 702 920 A83 B00 C02 D00`) | step-0 words (`105 285 320 400 500 600 780 910 A10 B01? …`) |
| responses `0x43DC7160…` | = requests | **all initial** (`100 200 300 … 700`), i.e. no module has acted |
| camera-state copy `word_43D86D28 / 2A` | 0x0A83 / 0x0B00 | **0x0A00 / 0x0B01** (the init values from task 65 `4376462x`), never updated |
| `dword_43D872C4` (frame divisor in use) | **1** | **2** |
| `dword_43D872D4` (divisor computed by VD_Pre) | 1 | 2 |
| `dword_43D87478` | 0 | 2 |
| cfg+0xC = `0x43D87260` (SYSV phase bit, §1.4) | 0 | 0 |
| HS chain (`43D887AC..AE`, `43EB23D0`) | normal, AE reaches 1 every frame | **also normal**: the HS_Main reporting step does run |
| `int_HS`/`HS jump` errors | present | present (not the discriminator) |
| errlog `Error : GetImagerTemperature` | 1 | 10–11 (symptom: the imager is off) |

So in a bad boot HS_Main does report, but every module still reports its initial state: **VD_Pre never handed out the
step-0 requests.**

### 1.4 Why VD_Pre does not copy: the 2V phase gate
VD_Pre `4376CFDC` (task 66), disassembly `4376D256..4376D312`:
```
R0 = (sub_43A5C968()==1 && req[0]==0x110 && cam[+0x238] ∉ {0x110,0x101,0x100}) ? …  (0 here: req[0] = 0x105)
copy requests → word_43D86D18[]   only if  R0 == 0  &&
        ( cfg->+0x0C == 1  ||  dword_43D872C4 <= 1  ||  R8 != 0 )
```
* `cfg` = `*(off_43D84A40)` = `0x43D87254`. `cfg+0x0C` is written at every frame start by `437696FC` from `43AB5486`:
  **`(rd 0xF3402028) & 1`** (`43AB37EC(off)` = read `0xF3400000 + off`. The same routine reads the SYSV frame counter
  `+0x2098` into cfg+0 and `+0x2094`). It is the phase of the SYSV V in which the engine frame started [? exact
  meaning: field / first-V-of-pair flag].
* `dword_43D872C4` is set from `dword_43D872D4` at frame start when `byte_43D8747D` = 1 (`4376C3A8`).
  `dword_43D872D4` is computed by VD_Pre (`4376DCB6`) as `437DD1DC(…, parity = (dword_43D87478 + cfg->+0xC) & 1)`.
  That returns 1 or 2 (V per engine frame): when the mode tables call for >1, it returns `43A4220E(…)` (= 2 here) unless
  the parity bit is 1. Task 65 init (`4376A7C4`) starts it at 1.
* `R8` = bit5 of a byte returned by `437DBC7C`, plus conditions on `off_43D84A48` (0 in both boot types).

**The race.** At boot, `dword_43D872C4` = 1, so VD_Pre copies freely. Once the engine computes the 2V divisor (pre-power
mode parameters) and latches `dword_43D872C4 = 2`, new requests are copied only on frames with `cfg+0xC == 1`.
* If `CAM_SyncRequest(0x3003)` arrives before that latch, step 0 is copied, the modules act (imager power-on), and the
  later mode tables bring the divisor back to 1. This is the good boot.
* If the request arrives after the latch, the copy needs SYSV `+0x28` bit0 = 1. The SYSV model (`cxd90014_vph.c`,
  `sysv_read`) returns the last written value, which is always 0. So the copy never happens, the parity term is 0 too,
  `dword_43D872D4` stays 2, and the sequencer waits at step 12 forever. This is the bad boot.
* Trace order agrees. In bad b3 the engine tick/sequencer ran (`437D8708` hits) before `cam_syncreq`; in good b1
  `cam_syncreq` came first. In b3 extra engine timing reports were logged before the request (`43893F98(4,0x203D)`,
  `(4,0x202F)`, `(5,0x1004)×3`, `(3,4,3)`). Anything that slows the guest (plugin probes, host load) makes the late
  case more likely.

### 1.5 Verification (live, unmodified firmware)
Bad boot u1 (timer-fix binary; stuck at index 12, mode 0x0A00, `43D872C4` = 2, 90 s after power-on):
1. gdb `set *(unsigned*)0xF3402028 = 1`. Within 3 s: `cfg+0xC` = 1, `word_43D86D28` = **0x0A10**, and probes hit
   `4398DB88` → **`PowerOn 436AF630` → `AsyncInitial 436AFE18`**. Modules 2/6/8/9 responded (`0x320`, `0x780`,
   `0x910`, `0x0A10`), but the sequencer stayed at 12: modules 0 and 1 were still unanswered with bit0 held at 1.
2. gdb writes 0, 1, 0, 1 about 3 s apart (bit toggling). The sequencer reached **index 33 (0x21)**, `43D872C4` = 1, and
   mode **0x0A83**: exactly the good-boot state.

So the missing hardware behaviour is a SYSV phase bit that changes over time.

### 1.6 Timer-fix binary (cxd4115 one-shot START cleared on match)
| binary | probe load | boots | bad (stuck at 12) |
|---|---|---|---|
| old `qemu-system-arm-2144956` | heavy (a/b/f/g/h/o/q/s series) | 37 | 8 (a1, a3, a4, a6, b3, f1y, o4, q2) |
| old | light (2 probes, e series) | 6 | 0 |
| timer-fix | heavy (t1–t8, r1–r6) | 14 | 0 |
| timer-fix | heavy + spin-lock/memcpy probes (u1) | 1 | **1** |

* The timer fix removes `PowerOn Wait No.01 TimerError!!` and `AsyncInitial: WaitTimer OpenError/StartError` (present
  in good old-binary boots a2, a5, f1x; absent in t1, t5). Keep it.
* It does not fix the stall: u1 stalled with it, in the same state as §1.3. The stall happens before PowerOn, so the
  utimer-11 bug cannot cause it. The lower bad rate seen with it (0/14) is attributed to host load varying between
  series [?]. The old binary also gave only 1 bad boot in 13 (q series) in a quieter period.

### 1.7 Spec for the code agent (problem 1)
Device: SYSV region of `hw/display/cxd90014_vph.c` (0xF3402000, registers at +0x000..+0xFFF of the region; the firmware
reads them as 0xF3400000 + 0x20xx).

1. **`+0x28` (0xF3402028) bit0 = V phase.** On every SYSV tick (`vph_tick`, `frame++`), set bit0 = `frame & 1`
   [? polarity, see 3]. Bits 1 and 2 are read by the IRQ 98 handler `43655A90` and by `43AB5486` (stored as flags at
   +7/+8 of the snapshot) and should stay 0 until decoded [?]. Writes by the guest to +0x28 were not seen. Keep the
   read-as-written behaviour for the other bits.
2. Expected result: `cfg+0xC` (0x43D87260) alternates or drifts between 0 and 1. A 2V-mode stall can last until a frame
   start sees bit0 = 1, then step 12 is passed (as in §1.5). Check: 10+ boots under heavy plugin load, all reaching
   `436AF630` and index 0x21. Probe/HMP: `x /1wx 0x43DC711C` should not stay 0x0C, and `x /1hx 0x43D86D28` should not
   stay 0x0A00.
3. Faithfulness note [?]. The engine frame (evsel source 29, 33 367 µs) and SYSV (1/60 s = 16 667 µs) are two
   free-running timers in the model, so the SYSV phase at an engine frame start drifts (2 × 16 667 ≠ 33 367; the phase
   flips about every 16 s). On the real camera both come from one sync generator: 59.94 Hz SYSV, and the engine frame
   on every 2nd V in 2V mode. That fixes the phase, and it must be 1 at frame start for VD_Pre's 2V gate to work. The
   better model is to run SYSV at 1/59.94 s (16 683 µs) and fire evsel source 29 from the SYSV tick that has bit0 = 1
   (every 2nd tick). Then cfg+0xC = 1 at every 2V frame start. Until that is done, the toggling bit in step 1 is enough
   to unblock the race. `+0x3C` = 64800 (period written by "Liro Start") and the HS mode-change IRQ 99 (`+0x44`) are
   the registers to look at for the exact relationship.

### 1.8 Open questions (problem 1)
* Exact meaning and polarity of SYSV `+0x28` bits 0–2, and how the 2V divisor relates to `+0x3C`/`+0x44`.
* Which module tables make `437DD1DC` return 2 in the pre-power state (`dword_43B25180/43B25270`, cam `+200`, `+603`).
* After the stall is cleared the sequencer stops at step 33 (mask 0x0238) in every boot. That is the SA-core-0 /
  live-view topic of `RE_LIVEVIEW_REQUEST.md`, not this one.

---

## 2. NULL call in av-cam: `PC=0, LR=4376FBD7` (task 100) and `4376FA2x` (task 66)

### 2.1 The crashing code
`4376FBA4` (called from VD_Drive `4376A2C4` at `4376A470`, task **100**, CPU0) walks the **VD-drive callback list**
`dword_43D88F18` (a `std::list`, node = {next, prev, data[7]}: data+0 = 5-word member-function pointer, +20 = arg,
+24 = ref-counted object):
```
for (it = begin; it != end; ++it) { call(data, data[5]); obj = data[6]; if (obj) obj->vtbl[3](obj); }  // 4376FBD4: BLX R3
clear(list)                                                                                           // 43A5DE1C
```
`LR=4376FBD7` with `PC=0` means `obj != NULL` but `obj->vtbl[3] == 0`: the object (or the node) was already freed and
reused. The node pool `43A5DF0A/43A5DDF4` is protected by `wai_sem(17)`. **The list links are not protected by
anything** (`43A5DF2C` insert, `43A5DE1C` clear).

### 2.2 Who touches the same list (dynamic, 200 s trace, run v1)
| function | role | task / CPU (ITRON cfg `cpu=`) | hits |
|---|---|---|---|
| `437702E4` "SetVDDrvCallback" (from the EE sequences `4354B530` …) | push_back | 101 / CPU3 | 346 |
| `4376FBA4` | walk + release + clear (normal path) | 100 / CPU0 (posted to EvtDrvnTsk `0x43D84BB8`, tid 0x64) | 4991 |
| `43A5DE54` → `4376FA64` (list `43D88F54`) + **`4376FA10` (list `43D88F18`)** | release + clear **without** calling | 66 / CPU2 (EvtDrvnTsk `0x43D86CF8`, tid 0x42) | 323 |

`4376FA10` is the function of the RE_ENGINE_TICK §11.4 oops (`ldr r0,[r0,#24]` on a freed node). So both observed oopses
are the two sides of one race: task 66 clears the list on CPU2 while task 100 walks it on CPU0.

### 2.3 Why task 66 clears the list: the int_HS error path
`int_HS` `4376CD90` (comparator 1 callback, IRQ 84 on CPU2) counts HS slots in a frame: `dword_43EB23D0` (count) must
equal `dword_43D86D14` (expected). The frame start sets 0 / 1, and HS 1..7 re-arm comparator 1 at `arg + cfg[0x2C]`
(1/8 frame). HS 7 does not re-arm, so the frame start expects count = 7 ("HS jump %d %d/8V" if not). On a mismatch
(`4376CE30`) it logs **`int_HS timing error %d/8V->%d/8V`** and posts `43A5DE54` to task 66. That is the list clear of
§2.2: an error-recovery path the real camera does not normally take.

Observed in every run: `int_HS timing error 7/8V->8/8V` together with `HS jump … 8`. There is an **8th** int_HS after
HS 7, with no re-arm in between.

### 2.4 Why there is an 8th int_HS: comparator double fire in `cxd90014_evsel.c`
IRQ 84 handler `436AC440`: `st = rd +0xF4; wr +0xFC = st` (W1C); then for each set bit k: `43A490BE` = **disarm** =
`436AC81C(k, cmp_value, 0)`, which stores **`+0x114+8k = cmp_value` and then `+0x110+8k = 0`** (`436AC830..`). Then it
runs the callback `43A490E8`.

Model (`ev_write`): every write in `0x110..0x15F`, including the compare register, calls `ev_cmp_arm()`. On the
`+0x114` store the comparator is still armed and `cmp_value` is already in the past, so `timer_mod(now)` schedules an
immediate fire. If the QEMU main loop gets the BQL between the guest's two stores (MTTCG, more likely when the host is
loaded or the plugin is active), `ev_cmp_fire` sets `stb` bit k again. The disarm store comes too late, IRQ 84
re-asserts, and the handler runs the same callback a second time with the same argument.

Evidence (runs v1/v2, 200 s each): **323/323 and 322/322** timing errors are preceded by two `4376CD90` calls with the
**same r0** (the comparator target, e.g. `00566C43` twice), each from its own `436AC440` entry. No timing error had
any other cause. On real hardware a compare register write while armed with an already-passed value cannot match
(an equality comparator [?]), so there is no second fire.

Other consequences of the same artefact: the extra fire also hits the other comparators (VD_Post/LM, sources armed with
`436AC384`). That is one source of the `EC_VD_Post had been still running`/`HS jump` noise.

### 2.5 Other contributing factors (artefacts, lower priority)
* All event-group-A sources (29 frame start, 28, 30, 23) fire at the same instant in `ev_vd` (RE_ENGINE_TICK §1). That
  produces the `VD_Sequence/HS_Main start delayed` warnings, but not the list clear.
* The push (task 101) vs walk (task 100) on `43D88F18` is also unlocked. On hardware it is separated in time (push in
  HS_Sequence, walk at VD_Drive). With the double fire removed, no overlap was seen in the traces (0 list ops on
  another CPU during a VD walk in 2 × 200 s) [? keep the evsel event phases distinct if a crash remains].

### 2.6 Spec for the code agent (problem 2)
Device `hw/misc/cxd90014_evsel.c`, group B comparators:
1. `ev_write`: for `off = 0x114 + 8k` (compare value) **only store** the value. Do not call `ev_cmp_arm`.
2. For `off = 0x110 + 8k` (arm): store. If bit0 = 1, schedule the fire at `cmp − counter` µs (keep "already passed →
   fire at once": the guest arms late under slow emulation, and the int_HS chain re-arms relative to the last target).
   If bit0 = 0, `timer_del`.
3. Optional hardening: in `ev_cmp_fire`, set `stb` only if `R(0x110+8k) & 1` is still set. This protects against a
   timer that expired just before a disarm write was processed.
4. Check: with the plugin loaded (same probe set: `pc=0x4376CE4C`, `pc=0x4376CD90`, `pc=0x436AC440`), there should be
   no `int_HS timing error` / `HS jump … 8` lines, and `43A5DE54` should have 0 hits in 200 s. Long runs (> 30 min with
   the plugin) should show no oops at `LR=4376FBD7` / `4376FA2x`.

### 2.7 Open questions (problem 2)
* Whether a timing error can still arise from real lateness (a comparator callback delayed past the next frame start
  under very slow emulation). If so, the firmware's own error path runs and the race in §2.2 is reachable again. That
  would be a firmware SMP bug that the real camera never exercises.
* The `liro` thread crash at ~28.7 s in a USB run was not reproduced here. It is presumed to be the same race [?].

---

## 3. Tools / reproduce
* Mode/sequencer state (HMP): `x /1hx 0x43D86D28` (0x0A00 = stuck), `x /1wx 0x43DC711C` (12 = stuck, 33 = normal),
  `x /1wx 0x43D872C4`, `x /1wx 0x43D87260`, `x /8wx 0x43DC7120` (requests), `x /8wx 0x43DC7160` (responses).
* Recovery test: gdb `target remote :<gdbport>`, `set *(unsigned*)0xF3402028 = 1`, then 0/1 a few times.
* Probes: `pc=0x4398DB88 pc=0x436AF630 pc=0x436AFE18` (power path), `pc=0x4375C920` (CAM_SyncRequest, r1 = request),
  `pc=0x437D8708` (sequencer tick), `pc=0x4376CE4C` (int_HS error), `pc=0x436AC440` + `pc=0x4376CD90` (double fire),
  `pc=0x4376FBA4/0x4376FBF4` (VD walk), `pc=0x437702E4` (push), `pc=0x43A5DE54` (error clear).
  Extra slowdown to provoke both races: `pc=0x4028a258,r0=0x1,pc=0x4017c220,r0=0x1` (kernel `_raw_spin_lock`,
  `memcpy`; the r0 filter drops every line).
* errlog decoding: `43894848` r0 = format string VA → av-cam.bin offset `r0 − 0x43541000`.

# Still capture: the Sgc1 exposure completion and the Iif frame end (fw 1.10)

Scope: the gate after the RAW-buffer allocation (`RE_STILL_CAPTURE.md` §5). Task 107 waits forever in step
`4385947C` on av-cam event flag 22 bit 0x100. This document finds the hardware events that set that bit, the next
missing event behind it, and gives a spec for the code agent. RE only; no emulator code was changed.

Evidence:
* Static: local disassembly of the live av-cam dump (VA 0x43500000, 10 MiB, from the `RE_STILL_CAPTURE` runs; the
  IDA host was unreachable). Thumb addresses below are av-cam VAs; GOT base = 0x43EDAACC.
* Dynamic: binary `910eae2`, pristine NAND clone per run, guest root shell, `a5ktrace` probes (`fn=`, `ts=1`), gdb
  stub, HMP `xp`. Gate 1 passed with the diagnostic of `RE_STILL_CAPTURE.md` §7 B (RAM Backup 0x51000F := 1 at
  `k_upm_setup_boot_factor`).
  * s1: lever off with probes on the Sgc1 job, its events and the IRQ 35 ISR.
  * s2: the same, then the two Sgc1 events **forced by hand** (§4), then the Iif frame end forced by hand.
  * s3: SD card (`mksd.py`), AUTOREPAIR dialog → database created → shooting screen; live-view probe on CamCore
    start; shutter press.

`[?]` = not proven.

---------------------------------------------------------------------------------------------------------------------

## 0. Summary

| Question | Answer |
|---|---|
| What sets flag 22 bit 0x100 for the lever-off capture | Not `43A59CEA` (that one belongs to the other exposure step `437D1070`). The lever-off capture runs exposure step **`43858D80`**, which passes **`43A59D42`** (`set_flg(22, 0x100)`, trace 0x4013) as the first completion callback and **`43A59D62`** (`set_flg(22, 2)`, trace 0x4003) as the second one to the Sgc1 job (`4376F288` → `43A46C10` → `436A7284`, mode 3). |
| Which hardware events run those callbacks | Two status bits of the **0xF3120000 input unit (IRQ 35)**: **`+0x48` bit 0** ("event A") runs the Sgc1 handler `436A75CC`, which calls callback 1 (flag bit 0x100) and parks callback 2. **`+0x4C` bit 0** ("event B") runs `43A46E62`, which calls callback 2 (flag bit 2). Enables: **`+0x50` bit 0** for `+0x48` bit 0, **`+0x54` bit 0** for `+0x4C` bit 0 (both already 1 at the shooting screen). |
| Why it hangs in the emulator | `cxd90014_campig.c` never sets bit 0 of `+0x48` or `+0x4C`, and its IRQ output only looks at bits 24..27 of `+0x50`. IRQ 35 never fires (s1: no `436A6904` hit in the whole run). |
| What the block is | The **Sgc1 sync generator** (SG) of the input unit, registers 0xF3120200..0x2FF: `+0x210` = run (bit 0) / continuous (bit 8), `+0x220` = frame size `(V−1) << 16 | (H−1)`, `+0x224` = line of event A. Live view programs `V·H = 900 900` clocks per frame = **27 MHz / 29.97 Hz**, the engine's own 33 367 µs frame. A still exposure is a **one-shot** frame (`+0x210 = 0x001`). |
| Proof | s2: forcing event A by hand (calling the ISR's A-list dispatch `436A6414(0x10000001)`) → `Sgc1EvA` → `43A59D42` → task 107 leaves the 0x100 wait and waits for bit 2 (`4385960F`). Forcing event B (`436A65A8(0x10000001)`) → `Sgc1EvB` → `43A59D62` → step 4 ("NextSection") runs. |
| Next gate (found and forced in s2) | **CamCore::Iif** (0xF3120400..): `+0x410` bit 0 is the input-frame "run" bit. Software only ever writes 1 (`436A5C64`); the hardware must clear it at the end of the input frame. While it stays 1, every CamCore start fails with **0x208** ("CamCore Start Error!: 208", `43858954`), so the capture section loops every 50 ms. Writing 0 by hand → second Sgc1 one-shot (mode 3, 1336 × 7868 clocks) → `EndRawSeq 437C3B54` → the camera goes back to monitoring. |
| Same bug in live view | Yes. s3: at the shooting screen 7 of 8 CamCore starts returned **0x208** (only the first succeeds). This is a candidate cause of the "live view stops before YC state 2" blocker [?]. |
| After both fixes (forced) | Task 115 waits for flag 22 bits **0x440** (`4385B82A`): completion callbacks of two RAW-development CamCore jobs (§6). Not decoded here; next RE topic. `sub-micro power off (factor 7 …)` was **not** reached in the forced run. |
| Normal shutter capture | Not reached. s3 (SD, `lens=none`, "Release w/o Lens" left at its default) shooting screen: a full shutter press did not start a capture (no `EnterCap`), and MENU then did not open, so the setting could not be changed in that run. Statically, every still exposure goes through the same Sgc1 one-shot (`4376F288`/`43A46C10` call sites in §1.3), so a shutter capture is expected to hit the same two gates [?]. |

---------------------------------------------------------------------------------------------------------------------

## 1. The exposure step and its callbacks

### 1.1 Lever-off capture (s1, s2)
Probe trace (s1, all in task 107 unless noted):
```
EnterCap 436F94A0 (66) → RawDevInit 437D0C18 → Sgc1_210 = 0 (436A7111, Sgc1_meas) → S4 437CCFE8 (72)
→ CapExpose 43858D80 → SgcWrap 4376F288(r0 = 3 = mode, r1 = rec+0x408 = 9CAB0208, r2 = 0)
→ Sgc1Exec 436A7284(mode 3) → +0x210 := 0x001 (43A4697C, lr 436A75AD)
→ WaitStep 4385947C → F22_wait 43A59C7A(0x100, 1) … never returns
```
Before the lever, live view called `Sgc1Exec` with mode 1 (V 0x333, H 0x44C) and, after it, mode 0 (V 0x339,
H 0x444); both write `+0x210 = 0x101`.

### 1.2 `43858D80` (capture exposure, called by the step sequencer)
`r7` = mode 4 if `rec[0x5F8] == 0 && rec[0x4A14] == 0 && rec[0x607] != 0 && *(GOT[0x5E20]+0x60) == 0`, else 3
(`43858EF8..43858F22`). Callbacks (`43858F3C..43858F74`): callback 1 = GOT[0x5B54] = **`43A59D42`** when
`rec[0x5F8] == 0`, else GOT[0x25C] = `43A5EEF2`; callback 2 = GOT[0x1188] = **`43A59D62`**. Then
`4376F288(r7, rec+0x408, r8, &cb2, &cb1)` → `43A46C10(Sgc1, mode, rec+0x408, flag, &cb2, &cb1)` →
`436A7284(Sgc1, mode, V = rec[0x24], H = rec[0x28], …)`.

`4385947C` then waits for 0x100 (0x102 if `rec[0x4A2C] != 0`), and later for 2 (`43859606`).

Flag 22 setters (all `43893F98(4, code, bits)` + `set_flg(22, bits)`):

| function | bits | trace code | referenced from |
|---|---|---|---|
| `43A59CEA` | 0x100 | 0xF008 | GOT 0x589C ← `437D114A` (step `437D1070`) |
| **`43A59D42`** | **0x100** | 0x4013 | GOT 0x5B54 ← `43858F3C`, `438590F0`, `4385952A` |
| **`43A59D62`** | **0x002** | 0x4003 | GOT 0x1188 ← `43858F64`, `43859180` |
| `43A59DA2` | 0x002 | 0x4005 | step `437D1070` (callback 2 there) |
| `43A59E4E` / `43A59E6A` | 0x040 / 0x400 | 0x4009 / 0x400D | §6 |

### 1.3 Every still exposure uses the same primitive
`4376F288` is called from `43858F84` (above), `438589B2` (second section, §5), `438593CC`, `43859540`, `4385C54E`;
`43A46C10` directly from `437D1192` (step `437D1070`, callbacks `43A59DA2`/`43A59CEA`) and `4385910A`
(callbacks `43A59D62`/`43A59D42`, then `wait(0x102)`). All of them complete only through the two events of §2.

---------------------------------------------------------------------------------------------------------------------

## 2. CamCore::Sgc1 (0xF3120200..0xF31202FF) and its two events

Object `0x43D10388` (vtable `0x43E8B718`, base `0xF3120000` at `+4`, interrupt object `0x43D10414` at `+0xC`).

### 2.1 Job start `436A7284(obj, mode, V, H, …)`
* Stores **callback 1 at `obj+0x50`** and **callback 2 at `obj+0x64`** (20-byte functors; the caller's last
  stack argument goes to `+0x50`). This happens on every call, also on the "same parameters" early exit.
* `+0x230` bit 0 := 0 (`43A46BC0`), `+0x228` := flag, `+0x244` := flag (`43A46BFC`, `43A46BD4`).
* Early exit (no register writes, returns 0) if `mode ≤ 2` and mode, `+0x2C/+0x30`, V−1, H−1 equal the last job.
  Live view calls it every frame with the same mode 1 parameters, so this path is the normal live case.
* `obj[0x20] = mode`, `obj[0x34] = V−1`, `obj[0x38] = H−1`; `tbb` on `mode−1`:

| mode | branch | writes (offsets from 0xF3120000) |
|---|---|---|
| 1 | `436A738E` | sync routing `+0x38` of 0xF3120000/0xF3000000/0xF3100000 under lock 19; `+0x208` bit 0 = 0, `+0x258` = 0, `+0x224` = V−1−100 |
| 2 | `436A742E` | as 1, routing chosen from `+0x34·+0x38` vs `6000·GOT[0x228]` |
| 3 | `436A749C` | `0xF312003C` bit 0 = 0 (`436A70BC`), `+0x38` bit 0 = 0 of units 0/1, **`+0x208` bit 0 = 1**, `+0x258` = 0, `+0x224` = V−1 |
| 4 | `436A74C6` | as 3 but `+0x208` bit 0 = 0, `+0x258` = 1 |
| 0 / other | `436A7344` | routing only |

  then for all: **`+0x220 = (V−1) << 16 | (H−1)`** (`43A45C46`), `+0x264`/`+0x26C` or `+0x264` bit 0 = 0, `+0x248`
  (threshold pair from `+0x30`; `0x3FFE3FFE` when 0), and finally **`+0x210 = 1 | cont << 8`** (`43A4697C`) with
  `cont = 1` for modes 1/2/0 and **`cont = 0` for modes 3/4**. Before that write, modes 3/4 test `+0x210` bit 0
  (vtable slot 5, `43A4692A`) and return **0x108** if it is still set, i.e. a one-shot that never ended. So bit 0
  is expected to drop at the end of a one-shot frame.

Registers seen (HMP `xp`):

| state | `+0x200` | `+0x208` | `+0x210` | `+0x220` | `+0x224` | `+0x248` | `+0x50` | `+0x54` |
|---|---|---|---|---|---|---|---|---|
| shooting screen (mode 1) | 1 | 0 | **0x101** | 0x0332044B (819 × 1100) | 0x2CE (718 = V−101) | 0x3FFE3FFE | **1** | **1** |
| stuck lever capture (mode 3) | 1 | **1** | **0x001** | 0x053700C9 (1336 × 202) | 0x537 (V−1) | 0x3FFE3FFE | 1 | 1 |

Live view: 819·1100 = 825·1092 = **900 900 clocks = 33.367 ms at 27 MHz** = the engine frame (`RE_ENGINE_TICK`), so
the SG clock is **27 MHz** and `+0x220` holds V−1 (lines) and H−1 (clocks per line). The first lever exposure is
1336 · 202 clocks = **9.99 ms**, the second one (§5) 1336 · 7868 = **389 ms** [? H as clocks/line for still modes].

### 2.2 Interrupt plumbing of the 0xF3120000 unit (IRQ 35, CPU 2)
* ISR `436A6904`: `r4 = +0x48` (`43A45FA4`), `r5 = +0x4C` (`43A45FA8`). If `r5`: write `+0x4C = r5` (W1C,
  `436A62B0`), then **B-list** dispatch `436A65A8(r5 | 0x10000000)`. If `r4`: write `+0x48 = r4` (`436A62A4`), then
  **A-list** dispatch `436A6414(r4 | 0x10000000)`.
* Both lists share one table of 16 entries × 24 bytes at **`0x43D6F998`** (GOT[0xA88]) `{tag, functor[5]}`; an
  entry fires when `(status | 0x10000000) & tag == tag`. The A list grows up from index 0 (count at `0x43D6FB18`,
  = 1), the B list grows down from 15 (first index at `0x43EA7BA4`, = 15). Neither list removes entries on dispatch.
* Registration (interrupt object vtable `0x43E8B6D8`): slot 2 `43A46272` → A list (`436A6318`), then **ack
  `+0x48` and `+0x50 |= bits`**; slot 3 `43A4651C` → B list (`436A64A8`), then **ack `+0x4C` and `+0x54 |= bits`**.
  So `+0x50` enables `+0x48` and `+0x54` enables `+0x4C`, bit for bit.
* Sgc1 init `436A770C` registers (table dump):
  * entry 0 (A list) tag **0x10000001** → `436A75CC` (Sgc1 event A),
  * entry 15 (B list) tag **0x10000001** → `43A46E62` (Sgc1 event B).

### 2.3 The two handlers
* **Event A `436A75CC`** (`+0x48` bit 0). By `obj[0x20]` (mode): 0 = timestamp + multi-frame count (`0xF312003C & 3`,
  re-arms `+0x230`/`+0x244` while frames remain); 1 = timestamp, and if `0xF312003C & 3 == 0` re-routes `+0x38` of
  the three units under lock 19; 2 = timestamp; 3/4 = nothing. Then for every mode: **move callback 2 from `+0x64`
  to `+0x78`, call callback 1 (`+0x50`) and clear it.**
* **Event B `43A46E62`** (`+0x4C` bit 0): **call the functor at `+0x78` (callback 2) and clear it.**
* In live view all three functors are empty (`43A3DC69` invoker, null target; gdb read of `obj+0x50..0x88`), so
  firing A/B every live frame has no side effect beyond the mode-0/1 bookkeeping above.

---------------------------------------------------------------------------------------------------------------------

## 3. CamCore::Iif (0xF3120400..0xF31204FF): the input-frame run bit

* RTTI `N6Camera7CamCore3IifE`, object `0x43D0F578` (vtable `0x43E8B610`, base `0xF3120000`), held at `+4` of the
  CamCore object `0x43D6A190` (handle 0x10E1, vtable `0x43E8B088`).
* Iif start (slot 4, `436A5BD4`): optional test-pattern SG setup (only when `obj[0x8C] != 0`), then **`+0x410 = 1`**
  (`43A45CF4`, the only caller is `436A5C64`) and `+0x310 = 1` (`43A45C78`).
* CamCore start (slot 3, `43A40058`): starts the sub-blocks, then `r5 = 0x20A | +0x74` if `+0x47C` bit 0
  (`43A45DBA`), **`r5 = +0x74 | 0x208` if Iif slot 5 (`43A45D8A`: `+0x410` bit 0) is set**, then calls Iif start
  anyway and returns `r5`.
* No software path writes `+0x410` bit 0 back to 0, so the **hardware clears it at the end of the input frame**
  (same pattern as Sgc1 `+0x210` bit 0 for a one-shot).
* Values during the stuck capture (s2, HMP `xp 0xF3120400`): `+0x410 = 1`, `+0x420 = 0x056F0F40`,
  `+0x42C/+0x43C/+0x444 = 0x055F0F3F`, `+0x450 = 0x056B0F40`, `+0x46C = 0x055F0557`, `+0x470 = 1`, `+0x474 = 0x80`,
  `+0x478 = 0x100`, `+0x47C = 0`, `+0x484 = 0x557`. All of 0x400..0x4FF is plain catch-all register file today.
* Effect today:
  * capture: `43A8CDE8` (step state 2) → `438584FC` → `43A3F6DA(0x10E1)` returns 0x208 → "CamCore Start Error!:
    208" (`43858954`, string `0x43BFF549`) → return 0 → `43A72C46` retries after `dly_tsk(50)` forever (tasks 72 and
    107 alternate; `/proc/<pid>/wchan` = `hwtimer_schedule_timeout`);
  * live view (s3, probe `43A40110`): 8 CamCore starts, 1 × 0, **7 × 0x208**.
* A second accessor family writes `+0x410 = 1` of another CamCore block (`43A43530`, called from `436A556E`) [? which
  unit; same self-clear rule is likely].

---------------------------------------------------------------------------------------------------------------------

## 4. Proof by hand (s2)

No firmware or device change. gdb has no Python here, so these are plain CLI scripts. The injection point is the
`dly_tsk` veneer `4355D498` called every 12 ms by task 43 (`lr = 43904371`, CPU 1, no locks held): the thread is
diverted into the IRQ 35 dispatch and returns to the same instruction, after which the saved registers are put back.
```
hbreak *0x4355d498 if $lr == 0x43904371
continue
set $sr0=$r0
set $slr=$lr
set $ssp=$sp
set $r0=0x10000001          # (+0x48 or +0x4C) | 0x10000000, bit 0 set
set $lr=0x4355d499
set $pc=0x436a6414          # event A: A-list dispatch.  Event B: 0x436a65a8
delete
hbreak *0x4355d498 if $sp == $ssp
continue
set $r0=$sr0
set $lr=$slr
delete
detach
```
(Raising IRQ 35 through GICD_ISPENDR is not usable: the ISR would read the device's real `+0x48`, and an HMP `xp`
of the GIC distributor crashed QEMU.)

| step | observed (probes, task) |
|---|---|
| stuck | `F22_wait(0x100)` from `438594A9` (107), +0x210 = 0x001, +0x48 = 0x02000000, +0x4C = 0 |
| event A forced | `Sgc1EvA 436A75CC` (43) → `F22_100b 43A59D42` (43) → `F22_wait(2)` from **`4385960F`** (107) |
| event B forced | `Sgc1EvB 43A46E62` (43) → `F22_2 43A59D62` (43) → `S4 437CCFE8` (107, "NextSection") |
| new loop | step state 2 every 50 ms, `ccs_err` 0x208 (§3) |
| `set *(int*)0xf3120410 = 0` (same hook) | `IifStart`, `ccs_err` = 0 → `SgcWrap(3)` from `438589B7` → `Sgc1Exec(3, V 0x538, H 0x1EBC)` → `+0x210 = 1` → `F22_wait(1)` (72), `F22_wait(0x200)` (107) pass → `S4` → **`EndRawSeq 437C3B54`** (96) → S4 in tasks 102/73/76 → live mode 0 `Sgc1Exec` (66) and IRQ 36/37 traffic resume |
| stop | `F22_wait(0x440)` from `4385B82F` (task 115), no `ExitCap`, no `sub-micro power off`; VM running |

The second Sgc1 one-shot got no events in this run (only the first job was forced) and the flow still went on; its
callbacks are not waited for on this path [?].

---------------------------------------------------------------------------------------------------------------------

## 5. Spec for the code agent

All in `hw/misc/cxd90014_campig.c`, instance 0xF3120000 only (new bool property, e.g. `input-unit`, set by the
board for that instance). Units 0xF3000000/0xF3100000 keep today's behaviour except for the IRQ rule in 5.1.

### 5.1 Status/IRQ rule (all instances)
* `st48` / `st4c` are 32-bit status words, W1C, any bit (not only 24..27).
* IRQ level = **`((st48 & R(0x50)) | (st4c & R(0x54))) != 0`**. Today `(st48 | st4c) & R(0x50) & 0x0F000000`.
  Evidence: §2.2 registration (`43A46272` / `43A4651C`). For units 0/1 nothing changes in practice (`+0x54` = 0,
  `+0x4C` never set).

### 5.2 Sgc1 sync generator (0xF3120000 only)
State: `running` (= `+0x210` bit 0), frame start time, a QEMU timer.
* Clock **27 MHz** (H). `V = (R(0x220) >> 16) + 1`, `H = (R(0x220) & 0xFFFF) + 1`, frame = `V·H / 27` µs
  (live 33 367 µs). Clamp to [1 ms, 2 s] and fall back to 33 367 µs if `+0x220` is 0 (hypothesis: the clamp).
* Write `+0x210` with bit 0 = 1 while stopped → start a frame now. Bit 0 = 0 → stop (cancel the timer, no events).
  A write while running only updates the register (live view rewrites the same value).
* Event A: at line `L = min(R(0x224) & 0x3FFF, V−1)` of each frame, i.e. `L·H / 27` µs after the frame start:
  `st48 |= 1`. Event B: at the frame end: `st4c |= 1`. Update the IRQ after each.
* At the frame end: if `+0x210` bit 8 = 1 (continuous, modes 0/1/2) start the next frame; if bit 8 = 0 (one-shot,
  modes 3/4) **clear `+0x210` bit 0** and stop. Do B before clearing, in the same timer callback.
* `+0x210` bit 16 keeps reading 0. The `+0x214`/`+0x218` latch stays as it is, but may now return the real line
  counter while running (`lo` = current line, must stay in `5 .. V−3`; RE_PLAYBACK_SWITCH §6.3) and `0x3FFF` when
  stopped. Optional.
* Keep the existing per-frame path completion/GPIO pulse timer independent of the SG (its phase is already
  arbitrary). [? whether the Pig path bits should follow the SG frame instead]
* Timing [?]: the order A before B and both after the start write are what the firmware needs; the exact µs do not
  matter for the gate. For the still modes the 27 MHz rule gives 10 ms and 389 ms.

### 5.3 Iif input-frame end (0xF3120000 only)
* Model `+0x400..+0x4FF` in the device (extend the region of this instance to 0x500 bytes; all registers read back
  as written, `LOG_UNIMP` for offsets not listed here).
* **`+0x410` bit 0**: set by software; **cleared by the device at the end of the next input frame** after it was set:
  the next Sgc1 frame end (5.2) if the SG is running, otherwise the next campig frame tick (33 367 µs) [? source of
  frames when the SG is stopped, e.g. the test-pattern path of `436A5BD4`]. Clear it in the same callback before
  event B is raised, so that a callback that restarts CamCore sees it free.
* `+0x410` bit 16 reads 0 (busy, `43A45DA2`). `+0x47C` bit 0: read back (software clears it, `43A45DCE`).

### 5.4 Comments / provenance
Cite `436A7284` (job programming, 0x108 on a still-running one-shot), `436A6904`/`436A6414`/`436A65A8` (ISR and
lists), `43A46272`/`43A4651C` (`+0x50`/`+0x54` enables), `436A75CC`/`43A46E62` (handlers), the 900 900-clock live
frame (27 MHz), `43A40058` + `436A5C64` (Iif run bit set only by software, 0x208 error). Mark the 27 MHz still-mode
timing, the Iif clear point and the SG-stopped fallback as hypotheses.

---------------------------------------------------------------------------------------------------------------------

## 6. The next gate (not decoded): flag 22 bits 0x440 in task 115

`4385B7C0..4385B82A`: `43A3F3D8(handle = sp[0x184], rec[0x428], …, callback = GOT[0x2840] = 43A59E4E (bit 0x40)
or GOT[0x25C]…)` starts a CamCore job (`436A2AF8(handle)` → `43A3FF9A` → slot 3), then `wait(0x400 | 0x40 or 0x20)`.
Bit 0x400 (`43A59E6A`, GOT 0x4CF4) is installed by `437D452A`/`437D4638` into `rec+0x4E7C+0xA4` for another job
(`43A7062A`). These look like the RAW development passes (memory → CamCore → YC) [?]. In the forced run they were
reached with only the first exposure's events and one Iif clear forced, so part of this stop may disappear once §5
fires every event of every job [?]. Start from probes on `43A3FF9A`, `43A3FF7E`, the callbacks above and the IRQ
35/36/37 dispatch (`436A6414`, `436A65A8`, `43A465F2`, `436A6940`) after §5 is in.

---------------------------------------------------------------------------------------------------------------------

## 7. Acceptance tests

Fresh NAND clone per run, `A5K_SHELL` on. Until the loader fix of `RE_STILL_CAPTURE.md` §6 is in, apply its §7 B
diagnostic (0x51000F := 1) at boot.

1. **Live view unchanged, IRQ 35 alive.** Shooting screen as before (`screendump`, MENU, PLAY). Probes `436A6904`,
   `436A75CC`, `43A46E62`: about 30 hits/s each while monitoring. `xp 0xf3120210` reads 0x101.
2. **CamCore start succeeds in live view.** Probe `43A40110` (r0 = result of CamCore start): **0** on every hit (today
   7 of 8 are 0x208). Check whether live view now gets past YC state 2 (`RE_LIVEVIEW_REQUEST.md` §9) [?].
3. **Lever-off exposure completes.** `sendkey o` at the shooting screen. Probes in order: `CapExpose 43858D80`,
   `Sgc1Exec 436A7284` (r1 = 3), `Sgc1EvA 436A75CC`, `43A59D42`, `Sgc1EvB 43A46E62`, `43A59D62`, `S4 437CCFE8`; no
   `43858954` error loop (probe `43858958`: r0 = 0); second `Sgc1Exec` (r1 = 3) from `438589B7`; `EndRawSeq
   437C3B54`. After the second one-shot `xp 0xf3120210` reads 0x000 (bit 0 dropped) until live view restarts it.
4. **Lever off powers off.** `sub-micro power off (factor 7, warm, wake 81 81 81)` and HMP `info status` =
   `paused (suspended)` (`RE_REBOOT.md` C.6). **Expected to still need §6**; if task 115 stops at `4385B82F`
   (`/proc/<pid>/wchan` = `twai_flg`), record it and hand §6 to RE.
5. **Shutter capture.** SD image, enable "Release w/o Lens" in the menu (`RE_LENS_EMOUNT.md` §6), full press
   (`sendkey shift-s 500`): same probe sequence as test 3 from the shutter path; if §6 is solved, a file appears on
   the card (`-trace sdcard_normal_command` CMD24/25, then `mdir`/`mtools` on the image) [?].

---------------------------------------------------------------------------------------------------------------------

## 8. Reproduce

* Runs: `A5K_SHELL=<p+2> A5K_NAND=<clone> A5K_QEMU=<copy> [A5K_SD=<img>] ./run.sh -display none -monitor
  tcp:127.0.0.1:<p>,server,nowait -gdb tcp::<p+1> -S -d plugin -D <log> -plugin
  tools/liba5ktrace.dylib,fn=<file>,fncap=30,fnwords=4,libobj=1,s=0,ts=1`, then the gate-1 gdb script.
* The probe file is re-read on change (resets the hit counts), so load the capture set just before `sendkey o`:
  `43858d80 CapExpose`, `437d1070 CapExpose2`, `4376f288 SgcWrap`, `436a7284 Sgc1Exec`, `43a4697c Sgc1_210`,
  `436a75cc Sgc1EvA`, `43a46e62 Sgc1EvB`, `436a6904 Isr35`, `43a59d42 F22_100b`, `43a59cea F22_100a`,
  `43a59d62 F22_2`, `43a59c7a F22_wait`, `4385947c WaitStep`, `437ccfe8 S4`, `437c3b54 EndRawSeq`,
  `436f94d4 ExitCap`, `43858958 ccs_err`, `436a5bd4 IifStart`, `43a40110 CCstartRet`, `43a72c56 StepCall`
  (w[2] = step function), `43a72c5a StepRet` (r0 = 2 → retry in 50 ms), `43a8b55c StepState`.
* Registers: HMP `xp /24wx 0xf3120200`, `xp /32wx 0xf3120400`, `xp /24wx 0xf3120000`. Do not `xp` the GIC
  (0xF8000000..) — it crashed QEMU.
* av-cam data: PA = VA + 0x40000000 held for `0x43D10388` but **not** for `0x43D6A190` (`xp` returned other
  data); read objects with gdb at a breakpoint in av-cam (e.g. `hbreak *0x43a40058`, `x/32wx $r0`).

---------------------------------------------------------------------------------------------------------------------

## 9. Implementation notes (code agent, QEMU branch `sgc1`, commit `427b591` on `18850ab`)

`hw/misc/cxd90014_campig.c`, new property `input-unit` (set by the board for 0xF3120000 only; that instance maps
0x500 bytes, units 0/1 keep 0x400). New trace events `cxd_campig_sg_start/sg_event/sg_stop/iif_end`.

* **5.1** as specified for all instances: `IRQ = (st48 & +0x50) | (st4c & +0x54)`, both status words full-width W1C.
* **5.2** as specified (27 MHz, `+0x220`/`+0x224` latched at each frame start, clamp [1 ms, 2 s], 33 367 µs fallback,
  write bit 0 = 1 while stopped starts, bit 0 = 0 stops, bit 8 continuous, one-shot clears bit 0 after B). The
  `+0x214`/`+0x218` latch is unchanged (optional item not done).
* **One deviation needed: B is held until A is acked.** First run with the plain spec: the first lever one-shot
  (`+0x224` = V−1, A only 202 clocks = 7.5 µs before B) had both bits pending when the ISR ran; `436A6904` dispatches B
  before A, so `43A46E62` found `+0x78` empty, then A moved callback 2 there and nothing ever called it → task 107
  waited for bit 2 (`4385960F`) forever. Now, if an enabled `+0x48` bit 0 is still pending at the frame end, the
  frame end (Iif clear, B, next frame / one-shot stop) waits until the guest's W1C of `+0x48` bit 0. hypothesis:
  on the real camera the ISR finishes within that line; the hold only keeps the A → B order (§5.2 "Timing").
  A continuous frame then restarts at `max(nominal end, now)`.
* **5.3** as specified: `+0x410` bit 0 cleared at the next SG frame end before B, or at the next campig frame tick
  while the SG is stopped (both hypothesis); `+0x410` bit 16 reads 0; `+0x47C` reads back; other 0x400..0x4FF offsets
  read back with `LOG_UNIMP`.

Results (fresh NAND clones, Backup 0x51000F = 1 via a loader cpio with `a5kcopy -bkset 0x0051000f 01`, `A5K_SHELL`,
probe `43894784 LOG` with `fnr1s=1`):

| check | `18850ab` | `427b591` |
|---|---|---|
| "CamCore Start Error 0x208" log lines, boot to ≈ 210 s | 435 | **0** |
| CamCore start result (probe `43A40110`) | 1 × 0, 520 × 0x208 | **481 × 0** |
| `+0x410` at the shooting screen | 1 | 0 |
| IRQ 35 / `436A75CC` / `43A46E62` at the shooting screen | 0 | 1058 / 529 / 529 in 20.9 s uptime (≈ 25/s each event) |
| `xp 0xf3120210` / `+0x48..+0x54` | 0x101 / 0x02000000 0 1 1 | 0x101 / 0 0 1 1 |
| YC chain (`437737D4`, `43661BC8`, `438F6CF4`, `437E3910`, `43546E40`) | runs in both (≈ 1 `437737D4` per s, same order of counts) | no visible change |

The RE_LIVEVIEW_REQUEST §9 blocker ("`437737D4`/`43661BC8` 0 hits") no longer holds on `18850ab` either, so the Iif
fix is not what unblocks YC state 2; nothing else changed visibly there.

Lever off (`sendkey o` at ≈ 300 s, §8 probe list): `EnterCap` → `RawDevInit` (`+0x210` := 0) → `CapExpose 43858D80`
→ `SgcWrap(3)` → `Sgc1Exec(3, V 0x538, H 0xCA)` → `F22_wait(0x100)` (107) → `43A59D42` → `43A59D62` (both in the
IRQ 35 path) → `F22_wait(2)` passes → `S4` → `ccs_err` r0 = **0** → `SgcWrap(3)` from `438589B7` → `Sgc1Exec(3, V
0x538, H 0x1EBC)` → `F22_wait(1)` (72), `F22_wait(0x200)` (107) pass → `S4` → **`EndRawSeq 437C3B54`** (96) → `S4` in
102/73/76 → live view back (`+0x210` = 0x101). **Stop:** `F22_wait(0x440)` from `4385B82F` in `liro-kliro_115`
(`wchan` = `twai_flg`, tasks 72/107 in `trcv_mbf`); `43A59E4E` (bit 0x40) fired once, bit 0x400 (`43A59E6A`) never.
No `sub-micro power off (factor 7 …)`; VM running. This is the §6 gate. Baseline `18850ab` in the same setup stops
at `F22_wait(0x100)` from `438594A9`, as in §1.1.

No regression: shooting screen OSD identical to `18850ab`; MENU opens, PLAY shows "no memory card" (a `sendkey p
300`/`m 300` hold is more reliable than the 100 ms default in both binaries); `pmca-console updatershell` → OK →
`sub-micro power cycle (factor 13)` → updater stage 2 → shell answers `help`. Idle power save (factor 8, warm, DDR
kept) and lever OFF/ON HOT resume work and MENU opens afterwards. Observed but not caused by this change [?]: after a
HOT resume the kernel panicked in 2 of 2 runs with the 0x51000F = 1 loader on **both** binaries (`18850ab` too), and
once (1 of 4 runs) with the stock loader on `427b591` (av-cam oops `PC 4394AA4C` memcpy from a NULL `[r4+0xC38]` at
`437DEE48`, task `liro-kliro_113`, 15.7 s after the resume); `18850ab` with the stock loader resumed cleanly in 1 of
1 run. See the open items.

Open items: (1) §6 (flag 22 bit 0x400 in task 115). (2) The HOT-resume panics above — the boot-mode 1 loader plus a
HOT resume looks like a separate topic (RE_STILL_CAPTURE §6.1: 0x51000F = 2 belongs to the HOT path) [?]; the single
stock-loader oops (3 later runs clean, MENU opens) needs more runs to decide whether the SG changes the resume timing. (3) Real SG clock / still
timing and the A → B hold are hypotheses. (4) `+0x214` latch could return the real line counter.

---------------------------------------------------------------------------------------------------------------------

## Gate 3: IDT/FOPD divide-by-zero — producer analysis (2026-11-16)

Scope: the 3rd still-capture gate traps in the av-cam software unsigned-divide runtime (a genuine `x / 0`). This
section identifies the divide call site, the data it divides, the producer that should fill that data, and the
emulator gap. RE only; no emulator code was changed. All addresses are av-cam.bin VAs (IDA load base
**0x43541000**, Thumb); the live-dump base used elsewhere in this file (0x43500000) differs by 0x41000, so a VA here
= (that file's VA) only after accounting for the load base — use the `boot_avcam.bin.i64` base 0x43541000.

Evidence: static only, IDA Pro 9.4 + Hex-Rays on `boot_avcam.bin.i64` (the 10 MiB av-cam dump). Device-model quotes
are from `qemu-overlay/hw/misc/cxd90014_opd.c` / `cxd90014_idt.c`. `[?]` = not proven.

### 0. Summary

| Question | Answer |
|---|---|
| What the trap is | `0x439B193C`/`0x439B1946` is the **div-by-zero handler of the Thumb software unsigned-divide runtime** `sub_4355C3A8` (`__udivsi3`/`__aeabi_uidiv`): `if (divisor == 0) JUMPOUT(0x439B1946)`, and `sub_439B193C` = `MEMORY[0] = 0; STRB R3,[R3]` (deliberate null-store abort). `0x4355C5FA → B.W 0x439B1946` is the same path in the signed variant. So the trap is a real divide-by-zero, divisor = 0. |
| Who divides | **`sub_437904F8`** (and its twin **`sub_43790948`**, chosen by `*(a2+1461)`), the OPD develop-param transform. Inner loop: `v40 = (u16*)(ctx + 132*row + 492)`, `row = 0..8`, inner `col = 0..10`; `divisor = *v40` (u16 @ `ctx+0x1EC`), `dividend = *((u32*)v40 + 1)` (u32 @ `+4`); it calls `sub_4355C3A8(dividend, divisor)` **unconditionally** → aborts when `divisor == 0`. Matches the oops exactly: `r4 = ctx = rec+0x3658`, `r7 = v40 = ctx+0x1EC` (row 0, col 0), both u16@[r7] and u32@[r7+4] = 0. |
| What the data is | A **9×11 = 99-zone grid** of 12-byte entries `{u16 count, u32 sum, u32}` at `ctx+0x1EC` (ctx = capture record `rec+0x3658`, the FOPD/IDT context = KickFOPD's first arg). The transform outputs `count` and `sum/count` (a per-zone average) into the OPD develop hw-param buffer. This is **per-zone OPD photometry** (sum + pixel count), i.e. detection *results*, not static geometry. |
| Consumer call chain | `Stage_OPD` **`sub_4385B90C`** (`Stage_OPD.cpp:428`, pipeline stage 42; `v0 = sub_43A59C60()` = capture record = rec) → **`sub_43A7B9A6`**`(v0+1044, v0+8)` → `sub_437904F8(v0+13912 /*=rec+0x3658=ctx*/, v0+4180, v0+6588)`. The divide runs only when `*(ctx+32) ∉ {8,27,20,10,16}` **or** `*(a2+416)==0` (`sub_43A7B9A6` gate) — so it is capture-mode dependent, which is why the forced lever-off run of `RE_RAW_DEVELOP.md` (a different mode/branch) reached the IRQ-69 wait without hitting this divide. |
| Who should produce the grid | The **IDT/OPD develop-param pipeline** `sub_4377DEB4` (stage markers `sub_43806B6C(32..54, 0/1)`), reached via `sub_4377D7DA` (stage 19) ← `sub_4377D7D4`. Its final stage **`sub_43785F24`** does `sub_4394A9D0(a17+240, a15+820, 1188)` — a **memcpy of the exact 1188 = 0x4A4-byte grid** (99×12) from the global working buffer `&unk_43D995B4 + 0x820` into a develop context. The grid values in `unk_43D995B4` are computed by the earlier pipeline stages (`sub_43780DD8`, `sub_437923E8`, …) from the OPD detection results + IDT param tables (`idt_cam.bin`). |
| **Emulator gap** | **`cxd90014_opd.c` and `cxd90014_idt.c` raise completion IRQs but write no result data.** Their own headers: OPD = *"No data is moved (the OPD tables / results are not written)."*; IDT = *"No image data is moved (the FOPD output at bus 0x1EC00000 is not written)."* So the OPD detection block never fills the per-zone accumulators. Every zone's `count` (the divisor) stays 0 → `sum/count` → divide-by-zero. Gate 2 (the flag-22 completion waits) passes **because only the IRQ is needed there**; gate 3 needs the *data*, which no device produces. |
| **Minimal faithful fix** | When an OPD detection pass completes (`cxd90014_opd.c` `opd_done`, i.e. `OPD_CTRL +0x0C` bit 1 0→1), the model must deposit **non-zero per-zone detection results** where the firmware reads them — at minimum per-zone `count > 0` for all 99 zones. Two candidate sinks (confirm which by the watchpoint below, then model that one): **(a)** the on-chip OPD result tables **0xF3031000..0xF30317FF** (512 words, cleared pre-pass by `436ADB7A`, "read back" today); **(b)** the DRAM FOPD/OPD output buffer at **0x9EC00000** (bus `0x1EC00000`; = `F30317C0` output address written by KickFOPD `439739F0`). A faithful `count` = the per-zone accumulation pixel count implied by the detection-window geometry the firmware programs into the OPD registers; `sum` can be `count * mid-gray` (any value keeps the pipeline numerically sane). This is a **hardware-model data-production gap, not a register-address gap.** |

### 1. The divide (firmware)

`sub_4355C3A8(numerator, divisor)` (Hex-Rays): `if (divisor == 0) JUMPOUT(0x439B1946);` — the `__udivsi3`/`__aeabi_uidiv`
runtime. `0x439B193C`: `MEMORY[0] = 0;` then `STRB R3,[R3]` with `R3 = 0` (null-store abort). Both the `CBZ R0 →
0x4355C600 → B.W loc_439B1946` veneer at `0x4355C5FA` and the direct `a2==0` branch land here.

`sub_437904F8(ctx, a2, a3)` inner body:
```
v40 = (u16*)(ctx + 132*row + 492);        // row = v18 = 0..8     (492 = 0x1EC, 132 = 0x84 stride)
...
v25 = *v40;                                // u16 count   (divisor)
v26 = *((u32*)v40 + 1);                    // u32 sum      (dividend)   @ ctx+0x1F0+...
*(u16*)v41    = v25;
v27 = sub_4355C3A8(v26, v25);              // UNCONDITIONAL divide  -> abort if count==0
*(u32*)(v41+4) = v27 >> 1;
*(u32*)(v41+8) = *((u32*)v40 + 2) >> 1;
...  v40 += 6 (u16, = +12 bytes); col = v31 = 0..10;  row stride += 132
```
Twin `sub_43790948` is identical with `v35 = ctx + 132*row + 500` (reads `*(u16*)(v35-8)` = `ctx+0x1EC`,
`*(u32*)(v35-4)` = `ctx+0x1F0`) and a `(8 * div) >> 8` scale. The oops `r7 = 0x9cab3644 = rec+0x3658+0x1EC`,
`r4 = 0x9cab3458 = rec+0x3658`, `rec = 0x9caafe00`; `0x9caafe00 + 13912 = 0x9cab3458` (stage dispatch
`v0 + 13912`).

### 2. The grid and its producer

Grid = `[9][11]` of `{u16 count; u16 pad; u32 sum; u32 w2}` (12 bytes) at `ctx+0x1EC` (= `rec+0x3844`), 1188 bytes.
`count` is the per-zone pixel/accumulation count (divisor); `sum` the accumulated value (dividend); the consumer emits
`count` and `sum/count` = the per-zone average → OPD develop hw params → OPD MMIO (`sub_439799D0`, e.g.
`MEMORY[0xF30317F0]`, `MEMORY[0xF30317C0]`).

Producer pipeline `sub_4377DEB4` (callees pass the static param tables `&word_43D98CD4`, `&unk_43D11200`, … and the
shared working buffer `&unk_43D995B4`): stage 32 `sub_43780DD8`, stage 35 `sub_437923E8`, …, stage 53
`sub_43785F24`. `sub_43785F24` line "`sub_4394A9D0(a17+240, a15+820, 1188)`" copies the finished 99-zone grid from
`unk_43D995B4+0x820`. `sub_43780DD8` fills `unk_43D995B4` from the detection results + IDT tables (its result reads
render as base-lost `MEMORY[small]` loads in Hex-Rays — the OPD/IDT result source). `[?]` the single store that lands
the grid at `rec+0x3844` specifically: `sub_43785F24` targets `a17+240` of a develop context whose base did not
resolve to `rec+0x3658` here, so there is at least one further copy between the pipeline output and `rec+0x3844` —
name it with the watchpoint in §4.

### 3. Why it is zero in the emulator

The OPD detection block and the IDT/FOPD develop block are **modelled only as IRQ/status generators**:

* `cxd90014_opd.c`: `opd_done()` sets `status bit0` in enabled units and lowers the IRQ-line gate; **it writes no
  per-zone results** to the 0xF3031000 tables or to DRAM. Header: *"No data is moved (the OPD tables / results are
  not written)."* The 0xF3031000..0x17FF tables "read back as written" (and were cleared to 0 by the firmware's
  `436ADB7A` before the pass).
* `cxd90014_idt.c`: raises IRQ 72 events 1/2 (→ `set_flg(22, 0x400)`) but *"No image data is moved (the FOPD output
  at bus 0x1EC00000 is not written)."*

So the detection results the develop-param pipeline consumes are **all zero**. `idt_cam.bin` (→0x83501000) and
`sa_dfdet.bin` (→0x9EC00000) are loaded by `provision/loader.sh -loadtable`, so static IDT/defect data is present;
the missing thing is the **runtime per-zone photometry the OPD hardware would accumulate during the detection pass**.
Every `count = 0` → `sum/count` → the `__udivsi3` div-by-zero handler → gate-3 trap.

This is the same device family as gates 1-2 (unmodelled cxd90014 engine events), one level deeper: gates 1-2 needed
the hardware *events*; gate 3 needs the hardware-produced *data*.

### 4. Spec for the follow-up agents

**Decisive experiment (RE, to pin the exact sink before coding).** Reproduce the gate-3 trap (the run that captured
the oed `r4/r7`), then with gdb:
1. Hardware watchpoint on `rec+0x3844` (the grid) and on `&unk_43D995B4 + 0x820` (the grid source). Record the last
   writer of each and its source operand — this names the exact store into `rec+0x3844` left `[?]` in §2.
2. Probe `sub_4377DEB4`, `sub_43785F24` (the 1188-byte memcpy), `sub_43780DD8`; and dump OPD MMIO `0xF3030000`/
   `0xF3031000..0x17FF` and DRAM around `0x9EC00000` right before `Stage_OPD` (`sub_4385B90C`) runs. This decides
   whether the develop stage reads the per-zone results from the **on-chip table 0xF3031000** or the **DRAM buffer
   0x9EC00000**.

**Code (cxd90014_opd.c, after the watchpoint confirms the sink).** In `opd_done()` (the completion of an
`OPD_CTRL +0x0C` bit-1 start), additionally write plausible non-zero per-zone results into the confirmed sink:
* 99 zones (9×11), per-zone `count > 0` (e.g. a constant derived from the programmed detection-window size, or any
  fixed positive value as a first cut), and `sum = count * mid-level`. The firmware only requires `count != 0`;
  magnitudes affect the computed OPD gains, not whether the capture proceeds (mark magnitudes `H`).
* Leave the IRQ/status behaviour unchanged (gates 1-2 still pass).
* If the sink is the DRAM buffer at 0x9EC00000, the write goes through the device's DMA/`address_space` to that PA
  (its address is what `439739F0` programmed into `F30317C0` / `+0x17C0`); if it is the on-chip table, write the
  0xF3031000-region registers.

**Acceptance.** With the data produced: the still-capture no longer traps at `0x439B193C`; `Stage_OPD`
(`sub_4385B90C`) completes `sub_43A7B9A6` without the div-by-zero; the capture advances past gate 3 to the next
stage. Cross-check: no regression in gates 1-2 (flag-22 `0x100`/`2`/`0x440` waits still pass) and in live view.

Provenance: `sub_4355C3A8`/`0x439B1946` (udiv + div0), `sub_437904F8`/`sub_43790948` (the divide and the 99×12 grid),
`sub_4385B90C`/`sub_43A7B9A6` (Stage_OPD consumer), `sub_4377DEB4`/`sub_43785F24` (producer pipeline + 1188-byte
memcpy), `cxd90014_opd.c`/`cxd90014_idt.c` headers ("no data is moved"). Hypotheses marked `[?]`/`H`: the exact
`rec+0x3844` store, the on-chip-table-vs-DRAM sink, and the result magnitudes.

### 5. Sink confirmation (watchpoint, 2026-11-16)

Runtime follow-up to pin where `opd_done()` must deposit the per-zone data. Method: fresh NAND COW clone, `run.sh`
with `-monitor`/`-gdb`/`A5K_SHELL`, boot to the shooting screen (~160 s), `arm-none-eabi-gdb` on the stub +
HMP `xp`. No flash writes; emulator only. Two boots (b1, b2). `[?]` = not proven.

**What was confirmed**

1. **The consumer reads the divisor from DRAM, not from MMIO.** The trapping divide `0x4379067a` loads the divisor
   with `0x43790674 LDRH R1,[R7]`; at the capture, `R7 = rec+0x3844` (= FOPDctx `rec+0x3658` + 0x1EC), a DRAM
   **capture-record** address. The still-capture record is a heap object at `0x9cab….` (the oops `rec = 0x9caafe00`),
   **distinct** from the live-view/default engine record (b1: `sub_43A59C60` → `rec = 0x43dcbb54`, in the av-cam
   image region; note `0x43dcbb54` is the same record as the `RE_ENGINE_OOPS.md` task-113 engine context). So
   `opd_done()` **cannot** write where the consumer reads — the data must flow HW → harvest → develop context →
   global → capture record.
2. **The 99-zone grid is zero everywhere, because no OPD per-zone results exist.** b1, shooting screen, idle record
   grid `rec+0x3844`: `div=0x0000 dvd=0x00000000`. Global grid source `unk_43D995B4+0x820` (fixed VA **0x43D99DD4**):
   `0`. b2, live view, break at the producer first stage `sub_43780DD8` (`a1=ctx 0x43d94d84`, `a2..` = the static
   param tables `0x43d98cd4`, `0x43d11200`, `0x43d979fc`): `unk_43D995B4+0x820` is `0` **before and after** the call
   — the pipeline leaves the grid source zero in live view (the 99-zone metering only fills it during a still
   capture [?]).
3. **The OPD result area is empty.** b1/b2 HMP `xp`: `0xF3031000..0x100C = 0`; `0xF3031090 = 0`, `0xF3031094 = 0`
   (per-zone count / validity, the gate of the area path — see §2 static note below); `0xF3031098 = 0x11d49f00`,
   `0xF303109C = 0x11d4a000` (OPD working-buffer **bus** addresses, IIB bus `0x11xxxxxx`, config not results);
   `0x9EC00000 = 0x91130aaa "ZITTOOLS…"` = the static `sa_dfdet.bin` loaded by `loader.sh -loadtable`, **not**
   runtime detection output. So every per-zone `count` the develop stage divides by is 0.
4. **`sendkey o` is power-OFF, not a capture.** The lever-off key powers the guest down (`info`: sub-micro
   `next 0x1f (off)`, shell dies); the producer pipeline `sub_4377DEB4` is seen running (it also runs every live
   frame, ctx `0x43d94d84`) but no `EnterCap`. The **gate-3 divide is only reached by a real shutter capture** (SD +
   *Release w/o Lens*, `RE_LENS_EMOUNT §6`), which was **not** driven to the trap in this budget — so the final
   harvest step was not observed live (see the open experiment below).

**Static note on the per-zone count.** In the OPD read family, `sub_4396C18C` computes a per-window count as
`((x1+1-x0)*(y1+1-y0)) >> 2` (detection-window **area / 4**) from window coordinates in `&unk_43E1C46C`, taken
**only when `MEMORY[0xF3031090] == 0`** (else the hardware-reported value is used). So `count` is a
detection-window pixel count: it is 0 when the window geometry is degenerate **or** when the hardware path is
selected and the hardware reports 0. The 99-zone grid producer `sub_43780DD8` does not compute area itself — it
**copies** the per-zone fields from a source buffer (`v333[…]`, a develop context), so the count originates one
level further up, in the OPD result harvest.

**Conclusion / remaining ambiguity.** The grid is zero because the OPD block produces no per-zone detection data
(the model `cxd90014_opd.c` moves none). The firmware harvests those results from the OPD result area — the on-chip
register/table region **0xF3031000..0xF30317FF** (cleared pre-pass by `436ADB78`, read by `sub_4396BA6C` /
`sub_4396C18C` `0xF3031090/94`, `sub_439AE6FC` `0xF3031168`, `0xF30313F0..3FC`, …) and/or an OPD DMA output buffer
on the IIB bus (the `0x11d4….` pointers in `0xF3031098/9C`, and the develop output at bus `0x1EC00000` = CPU
`0x9EC00000`). **Which of the two the 99-zone {count,sum} harvest reads is not pinned** (the shutter capture was not
reached), so the exact sink is still `[?]` between:
* **(a) on-chip OPD result table 0xF3031000..0xF30317FF** — the per-zone count/validity at `0xF3031090/94` and the
  result registers; this is the `opd_done()` sink if the harvest reads MMIO.
* **(b) an OPD DMA output buffer in DRAM** at the bus address `0xF3031098/9C` program (`0x11d4…`, CPU address to be
  resolved) — this is the sink if the harvest reads a DMA'd buffer.

**Open experiment for the follow-up RE/code agent (the one step left).** Drive a real shutter capture to the divide
(SD image via `tools/mksd.py`; enable *Release w/o Lens* with `sendkey m 300` menu navigation, `RE_LENS_EMOUNT §6`;
`sendkey shift-s 500`). At the shooting screen set, in order: a breakpoint at `sub_4396C18C`/the OPD harvest family
and at `sub_43780DD8`; when the capture runs, record the **source operand** of the loads that fill the per-zone
`count` — its address resolves (a) vs (b). Also HMP `xp 0xf3031000`/`0x11d49f00`/the F30317C0 output during the pass
to see which region the model would need to fill. gdb hardware watchpoints on `rec+0x3844` are usable once `rec` is
read at the divide (`R4`), but the record is a per-capture heap object, so set it only after the capture's `rec` is
known. (Note: `commands … continue` auto-continue blocks combined with a hardware watchpoint desynced this gdb/stub
— use plain breakpoints or the `a5ktrace` plugin for ordering.)

**Refined fix spec (for the code agent).** When an OPD detection pass completes (`cxd90014_opd.c` `opd_done`,
`OPD_CTRL +0x0C` bit 1 0→1), deposit **non-zero per-zone results** into the sink confirmed by the experiment:
* per-zone `count > 0` (the firmware divides `sum / count`; the window-area path alone gives `count = area/4`, so a
  faithful value is the programmed detection-window area / 4, but any positive value clears the trap — magnitude `H`);
* set `0xF3031090` (per-zone count / validity) non-zero so the firmware's count comes out > 0, whichever path it
  takes;
* if the harvest reads a DMA buffer (sink b), write the per-zone `{u16 count, u32 sum}` records there instead, at the
  CPU address behind the `0xF3031098/9C` bus pointer / the `F30317C0` output.

Confidence: consumer-reads-DRAM-record, grid-all-zero, OPD-area-empty, lever-off-is-power-off — **V** (runtime).
count = window-area/4 and the 0xF3031090 gate — **H** (static, `sub_4396C18C`). The exact harvest sink
(0xF3031000 table vs DMA buffer) and the result magnitudes — **[?]**, pending the shutter-capture experiment above.

### 6. Capture harness attempt — reached CapStart, blocked before RawDevInit (2026-11-16)

Goal: drive a real shutter still-capture to the gate-3 divide (`0x439B193C`) to confirm the sink (a on-chip
`0xF3031000` vs b DMA buffer) and then implement/verify `cxd90014_opd.c`. Method: SD image (`tools/mksd.py`, 4 GiB
FAT32), COW NAND clone, `-monitor`/`-gdb`, `a5ktrace` plugin, HMP `sendkey`/`screendump`. Emulator only, no flash
writes. **Outcome: the trap was not reached** — recorded here per the budget stop-rule; the implementation is left
for a follow-up once the trap is reachable (sink still `[?]`). Build HEAD: `sony-a5000` with the Sgc1/Iif
(`a44b7b9`) and IDT/OPD (`3bf04eb`) models.

**How far it gets.**
* SD + blank card → AUTOREPAIR dialog → "create database?" → `sendkey ret` → DB created → shooting screen (SCN,
  RAW+J). The DB persists in the SD image across boots.
* `0x0051000F = 01` (COLD), so the DMM/RAW-buffer gate (§RE_STILL_CAPTURE) is satisfied.
* **`lens=ekit` + SD:** a full shutter (`sendkey shift-s`) **does** fire **`EnterCap 436F94A0`** (lr `437C43CB`,
  i.e. from **`CapStart 437C43A8`**), and `CapStart` runs to **`StepMachStart 43A72BA2`**. This is **further than
  the prior docs' "shutter capture not reached"** (`RE_STILL_CAPTURE §0`, `RE_CAPTURE_COMPLETION §0`). **But the
  capture then aborts before `RawDevInit 437D0C18`** — no `RawDevInit`, no `CapExpose`, no `Stage_OPD`, no `ExitCap`;
  the capture tasks return to idle (`trcv_mbf`), 148-shot counter unchanged, no error dialog. The RAW-develop / OPD
  path (which contains the gate-3 divide) is never entered. Cause (hypothesis): with `lens=ekit` the body treats a
  lens as present but the stage-C UART lens never readies (`RE_LENS_EMOUNT §8`), so the capture is cancelled.
* **`lens=none` + SD:** a shutter press does **not** fire `EnterCap` at all (the body blocks release with no lens).
  The documented unblock is **"Release w/o Lens" = Enable** (`RE_LENS_EMOUNT §6`, menu `CustomReleaseWOLens`), which
  must be set through the MENU.

**Why the harness stalled (tooling limits, all under host load ≈ 4 from a concurrent engine session).**
1. **MENU navigation is unreliable.** The top tile menu renders fully (6 tiles: 카메라/사용자정의/무선/애플리케이션/
   재생/설정) but the **arrow→ADC mapping navigates erratically**: a single `sendkey up` moved the highlight
   *left* (설정→재생), `left` moved it down, etc., and presses queue and fire after my `screendump` so the shown
   frame lags. The arrow ADC levels in `hw/arm/sony_a5000.c` (`up=182,left=356,down=520,right=686,centre=45` on ADC
   unit 1) are a hypothesis and may not match the firmware's menu key decode; this needs its own RE before menu
   automation is dependable. `ret` (centre) works on a modal dialog (AUTOREPAIR) but did not reliably enter a tile.
   So I could not get to *Release w/o Lens*.
2. **gdb watch/breakpoints desync the stub under the live multi-vCPU capture.** Any `commands … continue`
   auto-continue, and even two plain breakpoints hit during a running capture, produce "Cannot execute this command
   while the target is running" and lose sync (the VM must be recovered by a detach). This gdb has **no Python**
   (`python` unsupported), so a synchronous gdb-driven stepping loop is not available either. The reliable tool is
   the `a5ktrace` TCG plugin (PC-hit logging), but it cannot read a load's source address, and its `fncap` is
   per-probe (live-view probes like `StepCall`/`Sgc1Exec` exhaust the cap, capture-only probes do not).

**Re-confirmed runtime facts (this session).** At the shooting screen / menu: OPD on-chip table `0xF3031000` = 0 and
`0xF3031090` = 0 (per-zone count/validity), `0xF3031098/9C` = `0x11d49f00 / 0x11d4a000` (OPD DMA working-buffer bus
pointers); the global grid source `unk_43D995B4+0x820` and the idle record grid are 0. So the per-zone {count,sum}
is zero everywhere, as in §5 — the OPD block produces no detection data.

**To finish (for the next run, when host load is low — the engine session idle, to avoid the task-113 vCPU-starvation
oops and the input lag).** Pick ONE of:
* (i) **`lens=none` + enable *Release w/o Lens*.** Either fix/verify the arrow ADC levels so the menu is navigable
  (RE the firmware key decode), or set the setting without the menu (the userland path is PARAMID `0x2EC` →
  `MSGID_SET_NO_LENS_RELEASE_CMD` → liro `0x4085`; find the av-cam flag the liro handler sets and poke it, or persist
  the setting in the SD/NAND). Then `sendkey shift-s` should run the full capture.
* (ii) **Find and clear the `lens=ekit` capture-abort condition** between `CapStart`/`StepMachStart` and `RawDevInit`
  (decompile the capture-context init `437E3910` / the first step after `43A72BA2`; probe `ExitCap 436F94D4` and the
  step dispatcher `43A72C56` w[2]=step fn to see the last step before the abort).
Then, at the capture, break (plain `break`, one at a time, no auto-continue) at `437904F8`/`43790674`; when it stops,
read `$r7` (= &divisor) and single-step the loads that fill `rec+0x3844` back to their source — that resolves sink
(a) `0xF3031000..0x17FF` vs (b) the DMA buffer at `0xF3031098/9C → 0x11d4….`. Only then implement `opd_done()`
(per §5 refined spec) and verify the trap is gone. Implementation was **not** committed this session because it
cannot be verified without reaching the trap (AI_WORKFLOW: commit only verified states).

### 6.1 Harness, second attempt — menu solved, Release w/o Lens enabled, but SCN full-press does not capture (2026-11-16)

With the reliable menu method (one key, ~450ms hold, ~4s settle — `RE_ENGINE_TICK §11.6`; 100% in this run) the
earlier "menu unreliable" blocker is gone. Steps that **worked**:

* SD + AUTOREPAIR → DB created → shooting screen (SCN, RAW+J). `0x51000F = 01`.
* **Enabled "렌즈 없이 촬영 = 가능" (Release w/o Lens = Enable)**: MENU → 사용자 정의 (Custom, tile col1/row0) →
  page **3** (RIGHT ×2) → item 2 "렌즈 없이 촬영" → `ret` → 가능 → `ret`. Verified on screen (가능).
* **Half-press (S1) works**: `sendkey s <hold>` runs AE metering — the OSD changes `ISO AUTO → ISO 3200`. So the
  shutter GPIO (`a5k_shutter` p0.1/p0.11), AE, and live view are all fine, and AE does **not** circular-depend on the
  missing OPD data (it produced a value).

What **did not** work — the actual blocker now:

* **A full press (S2) does not start a still capture.** `sendkey shift-s`, `sendkey spc` (both set S2), and an
  S1-held-then-S2 overlap (`s` held 2–3 s, `spc` added while held, giving a clean S2 1→0 edge under S1) all produced
  **no `EnterCap`** and no develop (probes `EnterCap 436F94A0`, `RawDevInit 437D0C18`, `CapExpose 43858D80`,
  `StageOPD 4385B90C`, `OPDxform 437904F8`, `439B193C` — 0 hits across ~10 presses). One lone `EnterCap` (lr
  `437C43CB`, from `CapStart`) was logged early in the session and aborted before `RawDevInit` (as in §6), but it did
  not reproduce. The 148-shot counter never changes.
* **Mode is fixed at SCN** (Scene). The emulator has **no mode-dial key** (`hw/arm/sony_a5000.c` keys: arrows, ret,
  m, p, o, s, space, ]/[, t/w — none sets the exposure-mode dial), so I cannot leave SCN for P/A/M. In SCN, `ret`
  opens the scene selector (`RE_ENGINE_TICK §261`); here it rendered a **black screen** (no visible scene list).
  Hypothesis (`[?]`): the SCN still-release path needs a scene selected / an AF focus confirm that the no-lens state
  does not provide, so S2 is rejected before `CapStart` completes — i.e. a mode/focus gate, not the lens-release
  gate (which is now enabled).

Tooling notes: `a5ktrace` `fncap` is **per-probe** and the `fn=` file **is reloadable** (rewrite → counts reset),
so capture-only probes are not exhausted by live view; HMP `xp` and `screendump` are reliable; `sendkey` for the ADC/
sub-micro menu keys is reliable with the settle method, but the GPIO **full shutter** did not drive a capture in SCN.

**State at stop.** Gate-3 divide **not reached**; the OPD result table `0xF3031000` = 0 and `0xF3031090/98/9C` =
`0 / 0x11d49f00 / 0x11d4a000` as before; sink (a on-chip vs b DMA buffer) still `[?]`; `opd_done()` **not**
implemented (unverifiable without the trap). **Next blocker to clear first:** make a still capture actually fire from
the fixed SCN mode with no lens — options: add a mode-dial control (or default the board to a P/Auto exposure mode)
to leave SCN; or find why the SCN S2 release is rejected (probe the S2/shutter-release handler and the AF/scene
precondition between the S2 GPIO edge and `CapStart`/`EnterCap`); or select a scene in the SCN selector (needs the
selector to render). Then resume at §5/§6 to confirm the sink and implement `opd_done`.

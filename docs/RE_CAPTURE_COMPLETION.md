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

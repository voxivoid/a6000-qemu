# PLAY / AUTOREPAIR after the camera stop: the Sgc1 line-latch spin (fw 1.10)

Scope: why, with the null SA model in place, PLAY without a card sometimes gives `EE_STOP_CMP` late or never and a
black screen that ignores keys, and why with an SD card the repair model is created but the screen stays black and the
card sees no writes. Facts carry their evidence. Guesses are marked **[?]**.

Addresses: av-cam VAs (load address 0x43541000, Thumb-2) unless written `libObj+X` (offset in `/usr/lib/libObj.so`).
Static RE: Hex-Rays on a private IDB copy (`C:\a5000\emu\play\pl.i64`, copied from `sa\sa.i64`; pristine copy
`pristine.i64.bak` next to it), local `../re/ida/libObj.so.c`. Dynamic RE: emulator binary
`qemu-system-arm-3a51630` (null SA), pristine NAND clones, a private copy of `tools/a5ktrace.c` that adds a host
timestamp to every `A5KF` line, `-d unimp`, HMP `info registers` per CPU, HMP `pmemsave` + thread walks, and gdb
memory writes to one io-f0 register (not GIC).

Related: RE_SD_MEDIA_SERVICE.md §10 (status before this work), RE_SA_DSP.md §6/§9, RE_ENGINE_STABILITY.md §1 (mode
sequencer, SYSV phase), RE_LIVEVIEW_REQUEST.md §2 (path selector / mode sequencer), RE_UI_BRINGUP.md §4.3/§5
(dmpNative UpdateManager, VDF SEND_COMP), RE_SENSOR_ISP.md §2.3 (CamCore units).

---------------------------------------------------------------------------------------------------------------------

## 0. Summary

| Question | Answer |
|---|---|
| Root cause (both problems) | **Missing hardware completion in the CamCore input unit 0xF3120000: the "Sgc1" line latch.** On camera stop, av-cam releases sync resource 4 (`SyncResMgr::Free` case 4 → `43A5DAB2`), which runs `CamCore::Sgc1` vtable slot 3 `436A70DC` in **task 101 (pinned to CPU3)**. Inside an interrupt-disabling lock it writes **`0xF3120214 = 1`** and polls **`+0x214` bit0 until it reads 0** (`43A46B1A`). The register is plain io-f0 RAM, so bit0 stays 1 and **CPU3 spins forever with IRQs masked** (PSR I = 1 in every sample). |
| Q1: why `EE_STOP_CMP` is late or missing | The spin is entered in some runs only (resource 4 held or not at the stop, §2.4). When it is, everything that needs CPU3 stops: av-cam "any-CPU" tasks queued there, Linux threads on its run queue. Whether the `0x3007` reply → `{17,32}` → `EE_STOP_CMP` chain still gets through depends on which of those it needs at that moment **[?]**. Without the spin the chain completes in 0.1–2.4 s every time (§2.5). A second, independent delay: `0x3007` is queued behind a still running startup path (`0x3003`); in one SD boot that path took 72 s (§2.6) **[?] cause**. |
| Q2: what the playback view waits for after `EE_STOP_CMP` | Nothing beyond the stop. In good runs the "No memory card" view (no card) or the "No images in this view" index (card with an empty database) appears within 1–5 s. No JPEG/SA decode, VPH layer change or VDF mode switch was needed for either view. The black screen is a **downstream effect**: Appframework is blocked in `drawFramebuffer` on the dmpNative UpdateManager mutex, waiting for a VDF OSD `SEND_COMP` that never comes because the VDF pin task does not run (§3). |
| Q3: the repair model | `ModelRepairToInstance` → `ActionSetInit` → `ActionGetRepairTime` (ObjCntMgr async) → `ActionGetRepairTimeCmp` → dialog **"Image database file not found. Create? Time required 00:00:12"**. OK → `ActionExeRepair` → **CMD24 ×109, CMD25 ×171** → `ActionRepairCmp` ~25 s later → "Complete" → shooting screen with "148" shots left. This works when the latch completes. In the black runs the spin starts 1–3 s after `ModelRepairToInstance`, so the repair never gets to its first media request (§4). |
| Classification | One missing hardware behaviour (Sgc1 latch completion, §5). Everything else observed (missing `EE_STOP_CMP`, black screen, keys ignored, no SD writes) is downstream. |
| Verified fix (by hand) | gdb: `0xF3120218 = 0x00800080`, `0xF3120214 = 0` as soon as `wr 0xF3120214 = 1` shows up. Two boots (a5 no card, s1 with SD) then ran: playback view, back to shooting, AUTOREPAIR dialog, repair with card writes, playback index (§6). |

---------------------------------------------------------------------------------------------------------------------

## 1. Runs

| run | card | latch spin | result | notes |
|---|---|---|---|---|
| a1 | no | yes, ~1 s after PLAY | black > 60 s | `SEQ_done(0x3007)` 2.1 s after PLAY; RAM dump + thread walk (§3) |
| a2 | no | **no** | `EE_STOP_cmp` 3.2 s after PLAY, "No memory card" view at +5 s, CPUs idle | control |
| a3, a4 | no | yes (`Sgc1_meas` at +1.1 s / +0.5 s, task 101, CPU3) | no `SEQ_done`, no `BG_reply`, no `EE_STOP_cmp` in 40 s, black | HMP: CPU3 PC = `43A4698A` in 8/8 samples, PSR `0x200001B3` |
| a5 | no | yes, at +31 s (after the view was up) | watcher poke → view stays, CPU3 idle again | then PLAY ×4, see §6 |
| s1 | yes | yes, 0.8 s after `ModelRepairToInstance` | watcher poke → repair dialog, repair, card writes, shooting, playback index | §4 |
| s2 | yes | yes, 2.3 s after `ModelRepairToInstance` | black, MENU dead | control, no poke |

Earlier runs of the previous agent (`scratchpad/nullsa/runs`), checked for `wr4 0xf3120214 = 0x1`:

| run | spin | screen after PLAY / at boot |
|---|---|---|
| r1, r3 (no card) | yes (13.4 M / 12.7 M polls) | black, no `EE_STOP_cmp` |
| r4 | yes | `EE_STOP_cmp` arrived **after** the spin had started, screen black for minutes (the RE_SD_MEDIA_SERVICE §10 ">2 min" case) |
| b5 (SD) | yes | black |
| r2, b2, b3 (no card) | no | "No memory card" view; r2: `EE_STOP_cmp`, MENU works |

The spin is present in every black run and absent in every good run (8 + 7 runs).

---------------------------------------------------------------------------------------------------------------------

## 2. Q1: the camera stop and the latch spin

### 2.1 The stop chain (confirmed, all good runs)
1. `EE_STOP_CMD` (libObj `ActionExeEeStop`) → CamMgr `SequenceEEStop` → av-cam `0x4081` → `BackgroundCmd::SndStart`
   → CameraBackground `{16,32}` → state 1 → `sub_17A0E8(0x3007)` → av-cam, infinite wait (RE_SD_MEDIA_SERVICE §3.1).
2. av-cam: `CAM_SyncRequest_Wrapper 4375C920` → `437D8560(0x3007)` (path selector). Table `0x43EC6420`, 249 entries
   `{cur_state, event, fn, event}`. For state 2 (monitoring), event `0x3007` → `437D9051` → path array `0x43B71AB8`
   = u16 `{count 1, 6}`: one transition 2 → 6. (State 0 → `437D9225`, 0x20 → `437D9231`, 8 → `437D90B5`, 0x2C →
   `437D8E9D`.)
3. VD_Pre (`437D85D8`) sets up the mode-sequencer table for 2 → 6 with `43A70EF4`: **step array `0x43B71526`, 11
   steps** (run a1: `0x43DC7114 = 0x43B71526`, `+0x18 = 0x0B`, index 0x0A).
4. HS_Main `437D8708`: when the last step's wait mask matches and the path is done, `dword_43DC70E0 = 0x3007` and the
   busy bytes `0x43E717EC/ED` are cleared.
5. Task 68 polls `43A5C48A` (from the per-frame callback `4376A78C`), consumes `dword_43DC70E0` (`437D87B0`) and sends
   the reply with `4375A338` (`SEQ_done` probe). CameraBackground answers `{17,32}` (`BG_reply`) and `EE_STOP_cmp`
   (`libObj+3CDDD4`) fires.

Timing without the spin: a2 PLAY → `SEQ_done` 3.2 s, `EE_STOP_cmp` +10 ms; s1 (second PLAY) 0.13 s.

Correction to RE_SD_MEDIA_SERVICE §10: "sequencer on step array `0x43B71526`, all modules matched" is the normal
**finished** state of the 2 → 6 stop sequence, not a stall.

### 2.2 The spin (run a1, `-d unimp`, line order)
```
wr4 0xf3120248 = 0x3ffe3ffe        43A469C2: thresholds
wr4 0xf3120210 = 0x0               43A4697C: config
rd4 0xf3120210 -> 0x0              vtbl+0x14 (43A4692B): bit0
wr4 0xf3120214 = 0x1               43A4688E: latch request
rd4 0xf3120214 -> 0x1              43A4698A: busy, polled ...
rd4 0xf3120214 -> 0x1              ... 92 368 185 times until the run was stopped, one write only
```
HMP samples (a3, a4): CPU3 `R15 = 43A4698A`, `R14 = 43A46B37`, `R0 = 0xF3120000`, `PSR = 0x200001B3` (SVC, Thumb,
**I = 1**) in 8 of 8 samples over 40 s. The other three CPUs keep running.

### 2.3 The code
`CamCore::Sgc1` (RTTI `N6Camera7CamCore4Sgc1E`, vtable `0x43E8B718`; object `0x43D10388`, register base at `+4` =
0xF3120000). Slot 3 = `436A70DC`:
```
a1[8] = 4 (mode); a1[12] = 0; a1[13] = 1; a1[14] = 1
+0x248 = (0x3FFE << 16) | 0x3FFE                     43A469C2
+0x210 = 0                                           43A4697C(…,0,0,0)
loop:
    lock 19 (4355D5EC, IRQs off: PSR I = 1 while inside)
    0xF3120038/0xF3000038/0xF3100038 = 0x100         43A468EA / 43A468BC ×2
    s = rd(0xF312003C) & 3                            436A70CC
    en = rd(+0x210) & 1                               vtbl+0x14
    43A46B1A:  +0x214 = 1;  while (rd(+0x214) & 1) ;  r = rd(+0x218)
               lo = r & 0x3FFF;  hi = (r >> 16) & 0x3FFF
    ok = (lo == 0x3FFF) ? !en : (lo > 4 && lo < a1[14] - 2)     unsigned compare, a1[14] = 1 → any lo ≥ 5
    if ((a1[19] == NULL || a1[19](a1[14]) == 0) && ok) break
    unlock 19; dly_tsk(1)
+0x200 = 0                                           43A45C26(…,0): stop the input
unlock 19
a1[18] = s; a1[16] = hi; a1[17] = lo
do {} while (rd(+0x210) & 0x10000)                   vtbl+0x18 (43A46968)
```
(disassembly `436A7140…436A7180` confirms the 0x3FFF test, `lo > 4` and the unsigned `CS/CC` compare with `[R4+0x38]-2`.)
After it, `43A5DAB2` reads `{hi, lo, s}` back (`43A46B4C`), logs them with `43893F98(4, 0xF00B, hi<<16 | lo, s)`,
calls slot 2 (`43A46E10`, `+0x200 = 1`) and sets flag 26 bit 0x100 (`4355D578(26, 0x100)`).

Interpretation **[?]**: a snapshot of the input unit's line/position counters, used to stop the sensor input at a safe
point inside a frame (`lo` must be ≥ 5 and below `V − 2`; `0x3FFF` = no sync). The only caller of the latch is this
function, so the value only reaches the 0xF00B diagnostic.

### 2.4 Why only some runs
`43A5DAB2` is case 4 of `SyncResMgr::Free` (`436F9BEC`). That case runs only if sync resource 4 is held at the stop
(`BYTE1(dword_43D763B0[11·4]) != 0`, plus `43A5149E`), so it depends on the live-view state at the moment of the stop
**[?] which state allocates resource 4**. Observed: during PLAY (a1, a3, a4), 30 s after the playback view was up (a5),
or right after the AUTOREPAIR model switch (s1, s2).

Once CPU3 is dead, the outcome depends on what is queued there:
* a1: `SEQ_done` and the view switch were already on other CPUs (`SEQ_done` ran on CPU1) → reply sent, screen still
  black (§3).
* a3: the 2 → 6 sequence finished (`0x43DC70E0 = 0x3007` in the dump) but `SEQ_done` never ran: task 68
  (`cpu=any`, pri 48) did not run again **[?] queued on CPU3**.
* r4 (previous agent): `EE_STOP_cmp` arrived after the spin had started, screen black.

### 2.5 Without the spin
a2: PLAY → `EE_STOP_issue` +0.9 s → `SEQ_done`/`BG_reply` +3.2 s → `EE_STOP_cmp` +3.2 s → view at +5 s. s1 second
PLAY: `EE_STOP_cmp` 0.13 s after `EE_STOP_issue`.

### 2.6 Second delay source: request queued behind the startup path **[?]**
In s1 the AUTOREPAIR switch sent `0x4081`/`BGCmdStart` at t = 19.0 s, but the boot's own `0x3003` path only finished at
91.6 s (`SEQ_done` n=1), and `0x3007` was accepted at 91.7 s (done 92.9 s, `EE_STOP_cmp` 92.9 s). In s2 the `0x3003`
path finished at 16.8 s, before the stop. `437D8560` does not refuse the request; it is held in
`dword_43DC7100` until the current path finishes. What held the startup path for 72 s in s1 was not captured (no
sequencer dump at that time). Candidate: the 2V phase gate drift of RE_ENGINE_STABILITY §1.7 note 3 (the SYSV phase
at an engine frame start flips about every 16 s) **[?]**. This is likely the "≈15 s" `EE_STOP_CMP` delay that was
seen before, but it is not proven.

---------------------------------------------------------------------------------------------------------------------

## 3. Q2: why the screen stays black (run a1, RAM dump + thread walk)

User stacks of im.elf threads (`uth.py`: user `pt_regs` at `stack+0x2000−80`, stack words resolved against the VMAs)
and futex owners (`ARM_ORIG_r0` = futex address, glibc mutex `+8` = owner TID):

| thread | where | meaning |
|---|---|---|
| **Appframework** (TID 396) | `pthread_mutex_lock` ← `5D579C` ← `5D4FC8` ← `5DCCE6` ← `5DB588` ← `5342B4` (dmpNativeSendWedgeCommands) ← `53233C` ← `4EFADE` ← `70CD64` ("drawFramebuffer") | waits for the **UpdateManager mutex** `0x068E59EC` (singleton `5D695C`, mutex at `+8`) |
| mutex owner TID 336 (THREAD_INFRA0) | `ioctl(/dev/dmpgles2, 0xC004820D)` in `5EA4C0` ← rm kick thread `5E9120` | idle (it took the mutex in the last `end_swap`, as designed) |
| grm_dn_vdf_if | `rcv_msg` (`5DC0C8` ← `5D4DF4`) | idle, waiting for VDF messages on 0x80003B |

RE_UI_BRINGUP §5.2: `end_swap` takes the UpdateManager mutex and keeps it until VDF answers the OSD update with
`MSGID_PIN_SEND_COMP`. a3/a4 confirm the missing answer: the last `vdf_send_update` (`libObj+5DC3D4`, +2.3 s after
PLAY) has **no `vdf_update_done`** (`libObj+5DCB48`) after it. VDF's `OsdPinExec 436731D8` runs in liro task 10
(`cpu=any`, pri 31) and stops being called at the same time. The UI thread is therefore stuck in the next frame:
**no drawing, no key handling** (MENU/PLAY ignored). This is a consequence of CPU3 being dead **[?] exact reason task 10
stops; it is not decoded**. No VDF/VPH register work or SA job was pending.

In the good runs (a2, a5, s1) the playback views appear without any SA start (`cxd_sa_start` trace: none after PLAY)
and without new VPH layer programming. The "No memory card" view and the empty playback index need no image decode.

### 3.1 If images were on the card [not tested]
Thumbnails / single image would use the SA JPEG decoder (`SA_JPEG_DEC`, programs `sa_jpeg_dec_{1core,1unit}.bin`,
RE_SA_DSP §6). With the null SA every function start returns "finished" (int_fact 1) without writing the output
buffer, so the expected behaviour is **no hang, black or stale thumbnails** **[?]**. A minimal real implementation would
be per-program HLE of the decode call: read the JPEG from the input buffer named in the start mailbox, write YCbCr 4:2:2
into the output buffer, report int_fact 1. The mailbox layout of the JPEG client (`tsk_jpeg`, result ids 0x13–0x15)
is **not decoded here**; that needs its own RE pass with a card that holds images.

---------------------------------------------------------------------------------------------------------------------

## 4. Q3: the repair model (run s1, latch completed by the watcher)

Probes `libObj+54F570 ActionSetInit`, `+54F7B0 ActionGetRepairTime`, `+54F98C ActionGetRepairTimeCmp`,
`+5501F4 ActionExeRepair`, `+5503F8 ActionRepairCmp`, `-trace sdcard_normal_command`:

| t (s) | event |
|---|---|
| 93.14 | `ModelRepairToInstance` (`libObj+54EB68`, r0 = 0x5E) |
| 93.18 | `ActionSetInit` (r1 = 6) |
| 93.95 | `Sgc1_meas` (task 101) → spin → poke at 95.26 |
| 95.82 | `ActionGetRepairTime` (r1 = 7): `IssueCommandAsync` to the ObjCntMgr instance |
| 96.68 | `ActionGetRepairTimeCmp` (r1 = 8): estimate received |
| ~100 | dialog: "Image database file not found. Create? Time required 00:00:12" [OK] [Cancel] |
| 183.34 | Enter → `ActionExeRepair` (r1 = 0x80000004) |
| 183–208 | **CMD24 ×109, CMD25 ×171** (plus CMD12/13/18) |
| 208.35 | `ActionRepairCmp` (r1 = 0x33) → "Complete." [OK] |
| 229 | Enter → `0x3003` start path → shooting screen, "148" shots left |
| 307 | PLAY → `EE_STOP_cmp` +0.13 s → playback index "No images in this view" |

In s2 (no poke) the spin started at 23.1 s, 2.3 s after `ModelRepairToInstance` at 20.8 s. The repair model never got
past its first actions, so no media request was issued. This is why the card saw only reads. MENU on the dialog had no
visible effect in s1 (`0x3006` was requested; the dialog stayed) **[?]**, which is plausibly the real behaviour of a
modal dialog.

So the repair model, libmpr and the SD path need **no change**. A blank `mksd.py` card triggers AUTOREPAIR because it
has no image database, which matches RE_SD_MEDIA_SERVICE §6's guess.

---------------------------------------------------------------------------------------------------------------------

## 5. Classification

| item | class | evidence |
|---|---|---|
| `0xF3120214` bit0 never clears, `+0x218` never written | **missing hardware behaviour** (completion of a register-level latch) | §2.2, §2.3 |
| CPU3 spinning with IRQs off | downstream (firmware polls inside an IRQ-off lock; real hardware must answer within that window) | §2.2 |
| `EE_STOP_CMP` missing or late | downstream of the above; plus the queued-request delay of §2.6 **[?]** | §2.4–§2.6 |
| black screen, MENU/PLAY ignored | downstream: VDF `SEND_COMP` missing → UpdateManager mutex → Appframework blocked | §3 |
| repair model stops, no SD writes | downstream | §4 |
| second EE start stuck with module 1 at `0x1A` (a5 only) | **open** | §7 |

---------------------------------------------------------------------------------------------------------------------

## 6. Spec for the code agent: Sgc1 line latch in the 0xF3120000 input unit

### 6.1 Where
`hw/misc/cxd90014_campig.c` already models the first 0x100 bytes of the unit at 0xF3120000 (IRQ 35). Add a second
small region for the Sgc1 registers, **0xF3120200–0xF31202FF**, on top of the io-f0 catch-all (the rest of the 64 KiB
block stays regfile). Only the 0xF3120000 unit has an Sgc1 object (`0x43D10388`, base literal `0xF3120000` in
`436A70DC`, `436A70CC`, `436A70BC`).

### 6.2 Registers (offsets from 0xF3120000; accessors in av-cam 1.10)
| off | access | behaviour | evidence |
|---|---|---|---|
| +0x200 | RW | input run (1 = run, 0 = stop). Latch only | `43A45C26`, written 0 after the latch loop and 1 by `43A46E10` |
| +0x210 | RW | config: bits 7:0, 15:8, 23:16 from `43A4697C(a2,a3,a4)`; **bit0 read as "enable"** (`43A46920`, decides the 0x3FFF case); **bit16 read as busy** (`43A4695E`, polled until 0 after the stop). Latch; bit16 must read 0 unless a real busy phase is modelled | §2.3 |
| **+0x214** | W / R | **bit0 = latch request / busy.** Write 1 → latch the counters into +0x218 and **clear bit0**. Reads return bit0 = 0 once the latch is done. The poll runs with IRQs masked on a pinned CPU, so complete it **synchronously in the write handler** (or at the latest on the first read). Never leave it pending on a timer | §2.2, §2.3 |
| **+0x218** | R | latched result: `[13:0]` = `lo`, `[29:16]` = `hi` **[?] semantics**. Must not be 0..4 in `lo` (the loop retries forever with 1 ms sleeps) | §2.3 |
| +0x248 | RW | thresholds `hi << 16 + lo`, written `0x3FFE3FFE`. Latch | `43A469C2` |
| other +0x2xx | RW latch + `LOG_UNIMP` | `+0x220`, `+0x224`, `+0x228`, `+0x230`, `+0x244` are written/read during live view (a1 log) and must keep reading back as written | a1 `-d unimp` |

### 6.3 The latched value
The firmware accepts `5 ≤ lo ≤ 0x3FFE` (any `hi`), or `lo = 0x3FFF` when `+0x210` bit0 = 0. Recommended:
1. **Preferred [?]:** if the unit is running (`+0x200` bit0 = 1 and the campig frame timer of this unit is active), latch
   the current position inside the engine frame: `lo = 5 + (now − frame_start) · (N − 7) / FRAME_US`, with N a
   nominal line count (for example 1125 [?]), so that `5 ≤ lo ≤ N − 3`. Put the same value into `hi`, or 0 [?]. That
   is the "position inside the frame" reading of §2.3.
2. Otherwise latch `0x3FFF3FFF` ("no sync"). In the only caller `+0x210` was just written 0, so this ends the loop
   as well.
3. Validated value: `0x00800080` (the gdb poke used in a5 and s1, §1). A constant in range is an acceptable first
   step if 1. is too much; document it as `hypothesis:`.

Do not raise IRQ 35 for the latch. No guest-RAM access.

### 6.4 Acceptance
Probes: `436a70dc Sgc1_meas`, `43a4698a Sgc1_poll` (cap 5), the RE_SD_MEDIA_SERVICE §8 set, `libObj+54f570/54f7b0/
54f98c/5501f4/5503f8`, `-trace 'sdcard_normal_command'`.
1. `Sgc1_poll` hits ≤ 2 per `Sgc1_meas`. No `rd4 0xf3120214` flood in `-d unimp`. HMP never shows CPU3 at
   `43A4698A` for more than one sample.
2. No card, 5+ boots: PLAY → `EE_STOP_cmp` within 5 s every time → "No memory card. Cannot play back." view. PLAY again
   → shooting screen. MENU opens the menu from both screens.
3. SD (`mksd.py` image), 3+ boots: AUTOREPAIR dialog "Image database file not found. Create?". OK → CMD24/25 →
   "Complete." → OK → shooting screen. PLAY → empty index view.
4. The `43893F98(4, 0xF00B, …)` diagnostic after the latch carries the latched value (optional check, probe
   `43893F98` with r1 = 0xF00B).

---------------------------------------------------------------------------------------------------------------------

## 7. Open questions
1. **Second EE start stuck (a5).** After PLAY (spin fixed by poke at +31 s) → PLAY back to shooting: path `0x3003`
   from state 1, start table `0x43B709C6` at index 42, every module matched except **module 1 (camera state): target
   `0x0102`, actual `0x011A`**. Screen black, later keys ignored. Not seen in s1 (the same return to shooting worked).
   Possibly linked to the late latch at +31 s and the poked value, to the `0xF312003C & 3` sync status read in the
   loop, or to the RE_SA_DSP §9 "module 1 stayed 0x1A" note **[?]**. Next step: compare `0x43DC7160`, `0x43D874F0`
   and the Sgc1 object (`0x43D10388`, words 8–19) between a good and a bad return once §6 is in.
2. Meaning of `+0x218` (`lo`, `hi`), `+0x248` and `0xF312003C & 3`, and the callback at Sgc1 `+0x4C` (`a1[19]`).
3. Which live-view state allocates sync resource 4 (§2.4), and why task 68 / task 10 do not run while CPU3 is dead
   (liro scheduling of `cpu=any` tasks) **[?]**.
4. The 72 s startup path in s1 (§2.6) and the earlier "≈15 s" `EE_STOP_CMP` delay: confirm with a sequencer dump
   (`x /12xw 0x83DC70E0`, `x /16xh 0x83DC7120`, `x /16xh 0x83DC7160`, `x /1xw 0x83D87260`, `x /1xw 0x83D872C4`) while
   it waits.
5. Playback with images: SA JPEG decode client mailbox layout (§3.1).
6. Setup → Format → OK was not exercised in this pass. With §6 in, the model switch should no longer hang, so
   RE_SD_MEDIA_SERVICE §7.4 item 2 can be re-tested.

---------------------------------------------------------------------------------------------------------------------

## 8. Reproduce
* Plugin: copy of `tools/a5ktrace.c` with `(g_get_monotonic_time() − t0)/1e6` added to `A5KF` and `A5KMARK` lines.
  For `LIB.so+OFF` probes the plugin needs one `pc=` probe that triggers the libObj VMA scan, e.g.
  `pc=0x400f9940,r0=0xdeadbeef` (as in the nullsa scripts).
* Probe file used (a3/a4):
  ```
  437d8560 SEQ_req     43a70f76 PATH_setup   4375a338 SEQ_done   43a522fe AV_4081
  436731d8 VDF_OsdPinExec   436a70dc Sgc1_meas   4376f320 Sgc_exec
  libObj.so+3dd708 EE_STOP_issue   libObj.so+3cddd4 EE_STOP_cmp   libObj.so+54eb68 ModelRepairToInstance
  libObj.so+70a780 VM_requestModel libObj.so+849318 BGCmdStart     libObj.so+17a25c BG_reply
  libObj.so+17a0e8 BG_avreq        libObj.so+5dc3d4 vdf_send_update libObj.so+5dcb48 vdf_update_done
  ```
* Sequencer dump: `xp /12xw 0x83DC70E0` (path selector: `+0` done event, `+4` next state, `+8` table, `+0x10` path,
  `+0x14` path index, `+0x18` request, `+0x1C` current state, `+0x20` pending request), `xp /8xw 0x83DC7100`,
  `xp /16xh 0x83DC7120` (targets), `xp /16xh 0x83DC7160` (actuals), `xp /2xb 0x83E717EC`.
* Hand fix while testing: watch the `-d unimp` log for `wr4 0xf3120214 = 0x1`, then
  `arm-none-eabi-gdb -batch -ex 'target remote :PORT' -ex 'set *(unsigned*)0xF3120218=0x00800080'
  -ex 'set *(unsigned*)0xF3120214=0' -ex detach`.
* Thread analysis: `pmemsave 0x80000000 0x20000000`, then walk `init_task` (RE_SD_MEDIA_SERVICE §1 layout). User regs
  at `stack + 0x2000 − 80`; `uregs[17]` (`ARM_ORIG_r0`) of a thread in `futex` is the futex address; the owner TID of a
  glibc mutex is at `+8`.

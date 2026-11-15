# RE: the av-cam task-113 NULL `memcpy` oops (`PC 0x4394aa4c`, `LR 0x437dee4d`) — fw 1.10

Scope: the intermittent kernel oops in the av-cam camera engine seen (a) after a USB app install, at a shutter half
press that switches playback → shooting (`RE_USB_APP_INSTALL.md` §5), and (b) at 70 s in a `console=ttyAM0` boot
(`RE_GUEST_SHELL.md` §2). Also the "stalls on rapid key presses" report. No emulator code was changed for this
document.

Addresses are av-cam.bin VAs (VA = file offset + 0x43541000, Thumb-2). `[?]` = not proven.

Evidence:
* Static: av-cam.bin (objdump, base 0x43541000), `re/ida/av-cam.itron_cfg.txt`, `re/ida/av-cam.itron_calls.tsv`,
  `re/ida/gfx/gfx_avcam_str.tsv` (string → function), `re/ida/liro.ko.c`. (windev/IDA was unreachable during this
  work; everything below comes from disassembly and dynamic traces.)
* Dynamic: binary `910eae2`, fresh clone of the pristine NAND per run, no SD, `-display none`. Runs r1–r7. Probes with
  `tools/a5ktrace` (`fn=` file, `fncap` large) built locally with a host wall-clock timestamp added to every line (µs),
  plus `-msg timestamp=on -trace dmp_smaph_job -trace gic_set_irq` in r5, and macOS `sample` profiles of the QEMU
  process (r4, r7; r7 with `-name a5k,debug-threads=on` so vCPU threads are named).

---------------------------------------------------------------------------------------------------------------------

## 0. Summary

| Question | Answer |
|---|---|
| What crashes | `Sync_Kick_IDT` (`437dec84`), run as a deferred job on av-cam **task 113** (EvtDrvnTsk object `0x43D84AF0`, CPU0, ITRON pri 12). At `437dee32` it loads `src = obj->+0xC38` and at `437dee48` calls `memcpy(sp+0x54, src, 4)` (`4394a9d0`); `src` is NULL → fault at `4394aa4c`. `obj` (r4) is one of a pool of 12 "Sync" sequence objects (`0x43DC8594 + k·0xD70`, k = 0..11). |
| Who sets / clears `obj+0xC38` | Set by the EndExpose VD callback `435480b0` (`Sequence_Common.cpp`, task 66, CPU2) at `435480fe` (`+0xC34` = `0x43DD29CC`, `+0xC38` = `0x43DD2A04`), which then **posts** `Sync_Kick_IDT(obj)` to task 113. Cleared (= 0) by the first step of every sequence, `437dbe04` (task 101, CPU3) at `437dc534`. That step also clears the two guards that would stop a stale kick (`+0xD50` resource mask, `+0xD58` error flag). |
| Root cause | A firmware race that only opens when **task 113 runs the posted kick later than the next reuse of the same sequence object**. Sequence n posts `Kick_IDT(X)`; sequence n+1 is started on the same object X (the pool hands X out again) and zeroes `X+0xC38`; the stale kick then runs before sequence n+1's EndExpose sets `+0xC38` again → NULL. The firmware has no lock or "pending job" check between the pool and task 113's queue; it relies on task 113 (an RT task on CPU0) running within one engine frame of the post. |
| Which emulated behaviour opens it | **No register, IRQ or completion of the engine models is wrong on this path.** The window opens when the **vCPU0 host thread does not run** for tens of milliseconds while vCPU2/vCPU3 keep running and the engine timers keep firing on QEMU_CLOCK_VIRTUAL (= host time without icount). On real hardware the four cores cannot be starved, so a pri-12 RT task wakes within µs; in MTTCG each vCPU is a host thread that the host can deschedule (host load, other QEMU instances, background QoS), and long BQL-held work stalls a vCPU too. |
| Proof | Unthrottled runs: kick latency (post → `Kick_IDT` entry) median 0–0.3 ms, max 15 ms; reuse gap (EndExpose(X) → next init(X)) min 24 ms, 1 % ≈ 41 ms → no crash. Same machine with the QEMU process forced to background QoS (`taskpolicy -b -p <pid>`, vCPU threads get starved) and playback/half-press/key cycles: kick latency up to seconds, **12 stale kicks pending across a reuse in ~100 s (r7)** and **the exact oops in r6** (`PC 4394aa4c LR 437dee4d`, task `liro-kliro_113`, r4 = `43dcbb54`), with the probe trace EndExpose(X) → init(X) → Kick_IDT(X) with `src = 0` (§3). |
| Key-press stalls | The GPU model runs P3D lists, PSC fills and PPF copies **inside the MMIO store**, on the vCPU that kicked them (always **vCPU0**, the same CPU as task 113) and under the BQL. During menu navigation vCPU0 spends 5–21 % of its time in `smaph_write`, and the vCPU threads spend up to 22 % (vCPU0) / 63 % (vCPU3) waiting for the BQL (§4). Each P3D list is short (p99 0.09 ms, max 5.4 ms over 24 180 lists), so this is a contributor to vCPU0 latency and to sluggish input, not the oops by itself [? user-visible stall length not measured]. |
| Fix (spec §5) | (1) **Engine frame CPU barrier**: an engine timing event (SYSV V → evsel frame, evsel comparators) is delivered only after every running vCPU has gone through its main loop since the previous event; a starved vCPU then delays the engine clock instead of falling behind it. (2) Optional: make the GPU kick asynchronous (completion + IRQ 76 from a BH), so vCPU0 is not blocked in device code. (3) Operational notes. Acceptance test: the throttled reproducer of §2 must give 0 oops and 0 stale kicks. |

---------------------------------------------------------------------------------------------------------------------

## 1. Static analysis

### 1.1 The crashing function: `Sync_Kick_IDT` `437dec84`
Strings referenced only by this function: `MakeDistComp time out`, `Sync_Kick_IDT: overflow %dV #%d => %dV #%d %dus`,
`Sync_Kick_IDT: overflow %dV => %dV %dus`. Flow (r0 = r4 = sequence object):
```
437dec90  if (obj->+0xD58) return;                          // error/abort flag
437deca0  if (!436f9b28(obj->+0xD50, +0xD54)) { obj->+0xD58 = 1; return; }  // resource mask check
          ...  build an IDT (OPD statistics) request on the stack from obj fields and the camera state 4376a720()
437dee32  r1 = obj->+0xC38                                   // pointer set by EndExpose
437dee48  memcpy(sp+0x54, r1, 4)                             // <-- NULL here (memcpy = 4394a9d0, fault 4394aa4c)
437defac  twai_flg(26, 4, TWF_ANDW, &p, 1)                   // wait for MakeDistComp (set by 437deacc)
          ... kick the IDT driver (43974c44 or 4397503c), overflow check against the V counter, return
```
* `436f9b28(mask64)` (`436f9b28..436f9b6c`): returns 0 if the engine is not running (`43a5146e`) or if a resource
  named in the mask is not held; **a zero mask returns 1** (check passes).
* The oops registers match this exactly: `r4 = obj`, `r5 = 0x43D84A3C`, `r6 = 0x43EDAACC` (GOT), `r1 = 0`,
  `r2 = 0xfffffffc` (len 4 − 4), `LR = 437dee4d`.

### 1.2 Where `obj->+0xC38` comes from: EndExpose VD callback `435480b0`
`435480b0` is a VD-drive callback (registered by the EE sequences `43546e40`, `4354d618`, `4354f6b0`, `43551478`
through GOT entry `0x43EDD6A4` → `43770328`/`4376fc04`; strings `Sequence_Common.cpp`,
`Sync_EndExpose: DIST_RSLT_NG MakeDistortionCorrectionParamForSync error!`). It runs on **task 66 (CPU2)**. Same guard
prologue as `437dec84`, then:
```
435480f6  obj->+0xC34 = &tbl+0x284   (= 0x43DD29CC)
435480fe  obj->+0xC38 = &tbl+0x2BC   (= 0x43DD2A04)        // probe at 43548102: r2/r3 = these values
435481ea  clr_flg(26, ~4)                                  // MakeDistComp not done yet
...
43548e28  sel = (([cam+8]+0x259) >> 4) & 7                 // executor selector, observed 4
43548ed4  4376ab98(functor{437dec85}, obj, 0)              // post Sync_Kick_IDT(obj) to task 113
43548edc  (debug byte [GOT+0x2bc]+0x29 set: call 437dec84 directly instead)
```
`437deacc` (MakeDistortionCorrectionParamForSync, task 121, CPU3) sets flag 26 bit 2 later.

`4376ab98` posts three messages to EvtDrvnTsk `0x43D84AF0` (`438948b4`): `clr_flg(20, ~bit)` (`43a514c9`), the job,
`set_flg(20, bit)` (`43a514d1`). The only waiter on flag 20 is `SyncResMgr::Destroy` (`436fa1d0` → `43a514d8`,
`twai_flg(20, 0x7f, AND, forever)`): **resource teardown waits for task 113's queue to drain, sequence-object reuse
does not.**

### 1.3 Who zeroes it: the sequence's first step `437dbe04`
`437dbe04` is the first step of every Sync sequence (functor built by `437dd328`, GOT `0x43EE09A4`; called by the
sequence runner `43a722e3` on **task 101, CPU3**). It re-initialises the whole object:
```
437dbe22  obj->+0xD50/+0xD54 = 0        // resource mask: Kick_IDT's check now passes
437dbe2e  obj->+0xD58 = 0               // error flag cleared: Kick_IDT's early-out now off
437dc530  obj->+0xC34 = 0
437dc534  obj->+0xC38 = 0               // the pointer Kick_IDT dereferences
```
The only other writer of `+0xC38` near this code is the object copy `43a71aea` (`43a71f56`).

### 1.4 Task layout (ITRON cfg)
| task | entry | CPU | pri | role here |
|---|---|---|---|---|
| 66 | EvtDrvnTsk | 2 | — | VD_Pre / VD callbacks: EndExpose `435480b0` |
| 101 | — | 3 | — | sequence runner: init step `437dbe04` |
| 121 | — | 3 | — | MakeDistComp `437deacc` |
| 113 | EvtDrvnTsk `43a96f35` | **0** | 12 | deferred jobs: `Sync_Kick_IDT` and ~25 posts/s through `4376b4f0` |
| 1 / 110 / 99 | | 0 | 5 / 6 / 7 | the only higher-priority av-cam tasks on CPU0 |

liro creates each ITRON task as a kernel thread bound to its CPU (`os_thread_bind` → `kthread_bind`) with an RT
priority derived from the ITRON priority (`os_tsk_setprio`, `159 − pri` [? exact policy mapping]). Linux CFS work on
CPU0 cannot delay it; only IRQs, higher-priority RT tasks and (in the emulator) the host scheduler can.

---------------------------------------------------------------------------------------------------------------------

## 2. Reproducer

The race window is a property of the vCPU0 thread being starved by the host, so the reproducer starves it on purpose:

1. Fresh pristine-NAND clone, start as usual (`run.sh -display none -monitor tcp:...`), wait for the shooting screen.
2. Put the QEMU process into the macOS background QoS band: `taskpolicy -b -p <qemu pid>` (Linux host: e.g. run it
   under `chrt --idle 0` / heavy competing load [?]). Every vCPU thread now gets descheduled irregularly.
3. Run key cycles over HMP, 8–20 times:
   `sendkey p` · wait 2.5 s · `sendkey s 300` (half press = switch to shooting) · 12 × (`right`/`left`/`up`/`down`
   60 ms, 80 ms apart) · wait 2 s.
4. Check `tools/klog.py <port>` for `Unable to handle kernel NULL pointer` / `PC is at 0x4394aa4c`.

Results with binary `910eae2`:
| run | conditions | cycles | oops | stale kicks pending across a reuse (§3.2) |
|---|---|---|---|---|
| r3, r4 | normal QoS, cycles / rapid keys | 6 + ad hoc | no | 0 |
| r5 | background QoS from 4 min (heavy trace on) | boot only | no | 3 |
| r6 | background QoS after the UI was up | 8 | **yes** (kernel 93.3 s) | 1 (the crashing one) |
| r7 | background QoS after the UI was up | 20 | no | 12 |

Probe file used (`fn=` of `tools/a5ktrace`, one address + label per line):
```
437dbe04 seq_init        r0 = obj, task 101
435480b0 vdcb_endexp     r0 = obj, task 66
437dec84 kick_idt        r0 = obj, task 113
437decaa kick_maskfail
437dee48 kick_memcpy     r1 = src (0 = the crash)
437defbe kick_tmo        MakeDistComp time out
437df064 kick_ovf1       errlog args r1..r3 = V numbers / µs
437df084 kick_ovf2
437df08e kick_exit
437deacc mkdist
4376ab98 post113         (all posts to task 113)
```
Analysis: for each object X, pair EndExpose(X) with the next `kick_idt(X)` (= kick latency) and with the next
`seq_init(X)` (= reuse gap). A **stale kick** is a `seq_init(X)` that comes before the `kick_idt(X)` of the previous
EndExpose(X). It crashes if that kick then runs before the new sequence's EndExpose(X).

A timestamp on every probe line is needed for the latencies (the local build added `g_get_real_time()` to the
`A5KF`/`A5KT` lines; see §5.4).

---------------------------------------------------------------------------------------------------------------------

## 3. Dynamic evidence

### 3.1 Normal timing (no starvation)
Per object, the cycle is `seq_init(X)` (task 101, CPU3) → `vdcb_endexp(X)` (task 66, CPU2) → `mkdist(X)` (task 121) →
`kick_idt(X)` (task 113, CPU0) → `kick_memcpy` (`r1 = 0x43DD2A04`) → `kick_exit`. 12 pool objects seen
(`43dc8594`, `43dc9304`, … `43dd1964`, stride `0xD70`); the same object is often handed out to the next sequence
again (e.g. `43dc9304` twice in a row).

| measure (r3, r4, r6 before throttling) | value |
|---|---|
| kick latency EndExpose(X) → kick_idt(X) | median 0–0.3 ms; max 5–15 ms (one 68 ms in r3 while an HMP `info registers -a` loop ran at ~3000 cmds/s) |
| reuse gap EndExpose(X) → seq_init(X) | min 24 ms, 1 % ≈ 41 ms, median ~200 ms. The short gaps occur while sequences run every frame (≈ 34 ms period, e.g. 4.9–5.4 s after the first sequence, and after mode changes) |
| seq_init(X) → EndExpose(X) (the crash window if a kick is stale) | 59–483 ms |
| jobs on task 113 (r4, `438949aa`/`438949ae`) | 4 665 jobs, max duration 6 ms; post → start ≤ 6 ms |
| `MakeDistComp time out` / `kick_maskfail` | 0 |
| `Sync_Kick_IDT: overflow` | 1 per boot (1 V late, at the first sequence) |

So in normal operation the kick runs 25–40 ms before the earliest possible reuse; nothing in the engine models makes
it late.

### 3.2 With vCPU starvation (r6, the reproduced oops)
Probe times are host wall clock relative to the first probe (the throttled guest runs slowly, so the steps are
seconds apart):
```
 84.636  cpu2 vdcb_endexp  r0=43dcbb54          EndExpose(X): +0xC38 = 0x43DD2A04, Kick_IDT(X) posted to task 113
 86.822  cpu3 seq_init     r0=43dcbb54          next sequence takes X again: +0xC38 = 0, +0xD50 = 0, +0xD58 = 0
 90.019  cpu0 kick_idt     r0=43dcbb54          the stale kick finally runs on CPU0, guards pass
 90.019  cpu0 kick_memcpy  r0=4a92fedc r1=00000000
```
klog:
```
[   93.299881] Unable to handle kernel NULL pointer dereference at virtual address 00000000
[   93.315581] PC is at 0x4394aa4c
[   93.318754] LR is at 0x437dee4d
[   93.321382] r10: 00000000  r9 : 00000000  r8 : 00000000
[   93.321527] r7 : 00000000  r6 : 43edaacc  r5 : 43d84a3c  r4 : 43dcbb54
[   93.322983] r3 : 00000000  r2 : fffffffc  r1 : 00000000  r0 : 4a92fedc
```
Same signature as (a) (`r4 = 43dc9304`, kernel 358 s) and (b) (`r4 = 43dca074`, kernel 70.7 s): only the pool object
differs. Under throttling, kick latency rose to median 30 ms and up to 9.3 s (r6), and r7 logged 12 stale kicks in
~100 s without crashing (in those the stale kick ran after the new EndExpose had set `+0xC38` again, or the new
sequence had already set `+0xD58`).

### 3.3 What starves vCPU0 in the emulator
* **Host scheduling.** Each MTTCG vCPU is a host thread. With several QEMU instances and agents on one host (load
  average 8–10 on 12 cores during this work) or a throttled process, one vCPU can lose its host CPU while the others
  and the main-loop timers continue. Engine time is QEMU_CLOCK_VIRTUAL = host time, so the SYSV V clock, the evsel
  frame and comparators (`cxd90014_vph.c`, `cxd90014_evsel.c`: `timer_mod(now + period)`) keep driving CPU2/CPU3.
  This is the mechanism reproduced in §3.2. Case (a) ran during a USB install session with a TLS server and other
  agents on the host; that host load starved vCPU0 there **[? inferred, not traced]**.
* **BQL-held device work on vCPU0.** The GPU model (`dmp_smaph.c` `smaph_write`) runs whole P3D command lists (vertex
  fetch, shader interpreter, rasteriser), PSC fills and PPF copies synchronously inside the MMIO store. The `sample`
  profile (r7, named threads) shows this happens on **"CPU 0/TCG"** only: the GLES driver kicks the GPU from CPU0, the
  CPU of task 113. While a list runs, vCPU0 cannot run any guest code, and every other vCPU that touches MMIO waits
  for the BQL. Per-list cost is small (r5: 24 180 lists, p50 0.01 ms, p99 0.09 ms, max 5.4 ms; PSC/PPF not timed),
  so this adds milliseconds, not the tens of milliseconds the race needs.
* **Blocking chardev writes.** `pl011.c` uses `qemu_chr_fe_write_all()`. With a socket backend whose client stops
  reading (e.g. a guest-shell socket left connected but idle while the kernel console prints), the vCPU doing the
  printk blocks with the BQL held until the client reads [? not reproduced here]. Both (a) (shell on UART2) and (b)
  (`console=ttyAM0`) had a Linux serial console active.
* For (b) the exact cause of the vCPU0 delay at 70 s is **[?]** (UART0 shared by Linux and av-cam, userland blocked
  on the tty; not traced). The crash itself is the same race.

### 3.4 Why the real camera does not crash
On hardware the four Cortex-A5 cores run all the time. A post to task 113 is a cross-CPU wakeup of a CPU0-bound RT
kthread with only three higher-priority av-cam tasks on that CPU, so it runs well within one engine frame (the
emulator's unthrottled numbers agree: ≤ 15 ms vs a 24–41 ms minimum reuse gap). The firmware's missing lock between
the sequence pool and task 113's queue is never exercised. The emulator has to keep the same invariant: a CPU must
not fall more than a fraction of an engine frame behind the engine clock.

---------------------------------------------------------------------------------------------------------------------

## 4. The key-press stall observation
`sample` of the QEMU process during menu navigation (5 s, ~1 ms interval):

| thread | r4 (unnamed, 10 keys) | r7 (named, 10 keys) |
|---|---|---|
| vCPU0 in `smaph_write` (P3D/PSC/PPF) | 547 / 2628 samples (21 %) | 179 / 3690 (5 %) |
| vCPU0 waiting for the BQL (`bql_lock_impl`) | 797 / 2628 | 830 / 3690 (22 %) |
| vCPU3 waiting for the BQL | 1666 / 2628 (63 %) | — |
| main loop thread waiting for the BQL | 1893 / 2628 (72 %) | — |

The UI redraw for each key press is rendered by vCPU0 inside device code under the BQL; the main loop (timers,
display refresh, input) and the other vCPUs queue behind it. That fits "stalls on rapid key presses" and makes vCPU0
(task 113's CPU) the busiest vCPU exactly when keys are pressed. How long a single visible stall lasts was not
measured **[?]**.

---------------------------------------------------------------------------------------------------------------------

## 5. Spec for the code agent

The firmware is correct for real hardware and must not be patched. The engine models (SYSV, evsel, VPH, SA, AIF) need
no register or IRQ change for this bug. The machine must keep the hardware's timing invariant: **no vCPU may fall
behind the engine clock by a significant part of a frame.**

### 5.1 Engine frame CPU barrier (primary fix)
Add a small helper (e.g. `hw/misc/cxd90014_cpusync.c`, or static code in `hw/arm/sony_a5000.c` exported through a
header) and use it in the two engine clock sources:

* API sketch: `A5kCpuGate *a5k_cpu_gate_new(void (*fire)(void *), void *opaque);`
  `void a5k_cpu_gate_request(A5kCpuGate *g);` (called from the timer callback instead of firing directly).
* `a5k_cpu_gate_request`: bump a generation number; for every vCPU that is not stopped, `async_run_on_cpu(cpu,
  ack_fn, RUN_ON_CPU_HOST_INT(gen))`; record the outstanding count. A vCPU runs queued work at its next exit from the
  TCG loop (a running vCPU is kicked out at once; a halted vCPU is woken, runs it and halts again), so a vCPU that the
  host is running acknowledges within µs; a starved one acknowledges only when it is scheduled again.
* `ack_fn` (runs with the BQL): ignore stale generations; decrement; when the count reaches 0, call `fire(opaque)`.
  If the guest requests another event while one is still waiting, keep only one pending (do not queue a backlog).
* Users:
  1. `cxd90014_vph.c` SYSV V tick (`vtick`, the master V clock that also drives the evsel frame through its GPIO
     line): the V edge (status/IRQ 98, `+0x28` phase toggle, evsel source 29) is delivered from the gate. Re-arm the
     next V tick from the **delivery** time (as now: `now + period`), so a delayed V stretches the frame instead of
     producing a burst.
  2. `cxd90014_evsel.c` comparator fire (`ev_cmp_fire`, IRQ 84/83 group B) and the free-running `ev_vd` path when
     SYSV is not the source: deliver through the same gate, keeping the existing "disarmed meanwhile" check at
     delivery time.
* Leave the evsel 1 MHz counter (`ev_counter`, virtual time) as it is: the firmware already tolerates late frames
  (it logs `HS_Main: start delayed` / `Sync_Kick_IDT: overflow`); what it cannot tolerate is CPU0 running behind the
  others.
* Machine property `cpu-sync=on|off`, **default on**, with a comment that cites this document: on hardware the CPUs
  cannot be starved, so the barrier restores stock behaviour; `off` keeps the current free-running timers for
  comparison.
* On reset / `system_reset`: drop pending gate requests (generation bump) so no stale `fire` runs after the reset.

Why this is enough: with the barrier, by the time the frame (or HS comparator) that leads task 101 to the next
`seq_init` is delivered, vCPU0 has been scheduled since the previous event. A kick posted at EndExpose is then at most
one event period old when the reuse can happen, and CPU0 needs only µs of guest time to start it. With the comparators
gated too (8 per frame), the bound is about 4 ms, far inside the 24–41 ms margin.

### 5.2 GPU kick without blocking vCPU0 (optional, for the key-press stalls)
`hw/display/dmp_smaph.c`: on the `+0x1800` (P3D), `+0x1C` (PSC) and `+0xC18` (PPF) kicks, latch the parameters and
return; run the job from a bottom half (`qemu_bh_schedule`) and set the status bit + raise IRQ 76 when it finishes.
This matches hardware, where the GPU runs concurrently with the CPU and reports completion by IRQ. Before switching,
check in `grm_gles.ko` that the driver waits for IRQ 76 / reads `+0x34` and never polls the status right after the
kick **[?]**; if it polls, set the status bit at the end of the BH and keep the IRQ. Keep the synchronous path behind a
property for debugging. Expected effect: vCPU0 keeps running guest code during rendering, so task 113 is not delayed
by UI work.

### 5.3 Operational notes (docs only)
* README "Guest shell": the PL011 output path blocks QEMU (`qemu_chr_fe_write_all`) when a socket client stops
  reading. Keep `a5kshell.py` draining, or disconnect it.
* README / AI_WORKFLOW: running many emulator instances on one host, or a QEMU process at background QoS, can starve
  vCPUs; without §5.1 this produces the task-113 oops.

### 5.4 Tool (optional)
`tools/a5ktrace.c`: an option `ts=1` that prefixes `A5KT`/`A5KF` lines with `g_get_real_time()` (µs). The analysis in
§2/§3 needs it.

### 5.5 Acceptance test
1. Build; boot a fresh pristine-NAND clone to the shooting screen (default `cpu-sync=on`).
2. Reproducer of §2 with background QoS and **3 runs × 20 cycles**: no `Unable to handle kernel NULL pointer` /
   `PC is at 0x4394aa4c` in the klog, and the probe analysis shows **0 stale kicks** (every `kick_idt(X)` comes before
   the next `seq_init(X)`). Reference without the fix: oops in r6 within 8 cycles, 12 stale kicks in r7.
3. Same with `-M cpu-sync=off`: the old behaviour (stale kicks appear) — shows the property works.
4. Unthrottled non-regression (`cpu-sync=on`): boot to the shooting screen within ±10 % of the current time; kick
   latency median ≤ 1 ms; the live-view path still works (`lv-source=bars` shows bars); no new
   `int_HS timing error` / `HS jump … 8` lines (RE_ENGINE_STABILITY §2 probes); playback ↔ shooting, MENU and the USB
   install test (`RE_USB_APP_INSTALL.md` acceptance test) still pass.
5. If §5.2 is done: menu navigation renders the same screens; `sample` shows no `smaph_write` → `p3d_run_list` on the
   vCPU threads.

---------------------------------------------------------------------------------------------------------------------

## 6. Open questions
* [?] Exact cause of the vCPU0 delay in case (b) (`console=ttyAM0`, kernel 70.7 s) and in case (a) (USB install
  session). Both are explained by the same race; the starvation source was not traced in those runs.
* [?] liro's exact Linux scheduling class/priority for ITRON tasks (`os_tsk_setprio` → `sub_B3C`).
* [?] What else task 113 executes (the ~25 posts/s through `4376b4f0`; all jobs ≤ 6 ms here).
* [?] Whether the GLES driver tolerates an asynchronous GPU completion (§5.2).
* Not related but seen: in steady shooting mode the Sync sequences run 2–4 times per second, every frame only during
  start-up and after mode changes. Whether the real camera runs them every frame was not checked.

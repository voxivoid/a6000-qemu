# `hwtimer_on: counter(N) has caught up expire(M)!` — timer warnings in the emulator

Scope: why the kernel prints `hwtimer_on: counter(N) has caught up expire(M)!` about once a second in the
`sony-a5000` emulator while the real camera's kernel-log sample has none; what `hwtimer_on` actually checks; why the
emulated counter is already past the compare point at the instant the timer is armed; and whether this is a fidelity
bug in `hw/timer/cxd4115_timer.c` or an unavoidable emulation artefact. **RE only; no emulator code was changed.**

Related: `docs/RE_UART_RESUME.md` (these warnings feed the UART2 resume deadlock; §6 open question 1 handed this
topic off), `docs/RE_ENGINE_TICK.md` §8 (hwtimer0..3 = ch5..8, the free-run fix), `docs/RE_ENGINE_OOPS.md`
(QEMU_CLOCK_VIRTUAL = host time without icount; vCPU starvation under host load), `docs/HARDWARE.md` (timer block
0xF2403000, ch n at +0x100·n, IRQ 153+n). `[?]` = not proven.

Evidence:
* Static: `re/vmlinux.elf` (objdump). `hwtimer_on` 0x4005261C, `hwtimer_mod_timer` 0x40052CB8, `hwtimer_intr`
  0x40052728, `hwtimer_read_clock_ch` 0x400524C0, `hwtimer_schedule_timeout` 0x40052E68. Format string at 0x402F7DD0:
  `"timer.%s: counter(%ld) has caught up expire(%ld)!\n"` (the `%s` argument is the `"hwtimer_on"` function tag).
  `hw/timer/cxd4115_timer.c` (this repo). `re/kernel_syms.txt`.
* Dynamic: binary `sgc1m` (`qemu-hwt`), pristine NAND clone per run, camera boot to the shooting screen then idle,
  no SD. TCG plugin `tools/liba5ktrace.dylib` (`ts=1`) probing `hwtimer_on`'s compare point (0x40052640) and the warn
  printk call (0x40052654), plus `hwtimer_mod_timer`'s clock read (0x40052d2c). Host load (`vm.loadavg`) logged every
  10 s; the host carried 12 other agents (load average 16–52 across the runs).
  * **r1** — four probes, 300 s: 62 551 arms of `hwtimer_on`, 665 warn-printks. Per-channel: cpu0→ch5 (46 458
    arms), cpu1→ch6, cpu2→ch7, cpu3→ch8 (per-CPU clock+compare timers).
  * **r2b** — no plugin, `busybox dmesg` after 278 s: **99** `caught up` lines (≈ 0.36 /s), overshoot median 70
    ticks, max 49 973; clustered toward the idle-suspend window near the run end.
  * **r3** — single warn probe, 300 s, load 24–37: **218** warn-printks.
  * **r4** — `-icount shift=auto`, single warn probe, 300 s, load 16–21: **90** warn-printks; still present,
    overshoot median 265 ticks, max 1808.
  * Counter-skew probe (HMP `xp` of +0xC on ch5..8): channels monotonic and correctly rated; inter-channel skew
    grows with host load (ch6–ch5 ≈ 0.26 ms at load 25, ≈ 4.9 ms at load 80), i.e. the four per-CPU counters drift
    apart exactly when the host is busy.

---------------------------------------------------------------------------------------------------------------------

## 0. Summary

| Question | Answer |
|---|---|
| What does `hwtimer_on` check? | After arming a channel (write CMP at +0x8, write CTRL at +0x0) it **reads back COUNT at +0xC and verifies the compare point is still in the future** (`(int32_t)(COUNT - expire) < 0`). If the counter has already reached/passed `expire`, the match IRQ would never arrive, so it warns, bumps `expire` to `COUNT + 10` ticks and rewrites CMP — looping until the compare point is ahead of the counter. |
| Why is the emulated counter already past expiry? | The timers are per-CPU **clock + compare** channels (ch5..8, CTRL 0x1132 → 1 MHz, 1 tick = 1 µs). The caller (`hwtimer_mod_timer` / `hwtimer_intr`) reads the counter, derives `expire`, clamps it to **no nearer than COUNT + 10 ticks** (10 µs of head-room), then inserts into a per-CPU sorted list under a spinlock and only then calls `hwtimer_on`. The emulated counter is driven by `QEMU_CLOCK_VIRTUAL`, which in **non-icount TCG tracks host wall-clock time while the guest executes**. The real time that elapses across the read → list-insert → arm → verify window (guest instructions + 3–4 BQL-serialised MMIO accesses, plus any host descheduling of the vCPU thread) regularly exceeds the firmware's 10 µs margin, so by the verify read the counter has "caught up". |
| Is the register model wrong? | **No.** The CXD4115 semantics in `cxd4115_timer.c` (load/compare at +0x8, live free-running COUNT at +0xC, prescaler from CTRL[1:0], free-run bit5, reload bit8, IRQ-enable bit1) are faithful. The counter is monotonic and correctly rated; the warning is the firmware's own race self-check firing on emulation jitter, not on a wrong value. |
| Model bug or emulation artefact? | **Emulation artefact.** The 10 µs critical section is sub-µs on real silicon, so the real camera never trips the check. In QEMU the section spans real host time (or, under icount, a whole round-robin quantum spent spinning on the per-CPU hwtimer spinlock held by another vCPU). Both are TCG/host-scheduling effects, not a property of the modelled registers. |
| Does a `cxd4115_timer.c` fix apply? | **No model fix is warranted** (details and the reasoning against each candidate in §5). The warnings are benign to the guest (the firmware's `+10` retry always succeeds) and are only a problem downstream, through the UART2 printk-under-lock deadlock, which `docs/RE_UART_RESUME.md` already fixes at the console token (`loglevel=4`). §6 lists the acceptance tests any future mitigation must pass. |
| Load-driven? | **Yes.** Warning count rises with anything that inserts host time into the read→arm window: host load, and TCG-plugin instrumentation (665 warns with 4 probes vs 99 with none at comparable load). icount decouples from host load but does not remove the warnings (round-robin quantum + guest spinlock). |

---------------------------------------------------------------------------------------------------------------------

## 1. What `hwtimer_on` does (0x4005261C)

`hwtimer_on(ch_base /*r0*/, expire /*r1*/, ctrl /*r2*/)`:

```
str   r1, [r4,#8]      ; CMP  (+0x8) = expire
str   r2, [r4,#0]      ; CTRL (+0x0) = ctrl (START already set; re-armed in place)
ldr   r2, [r4,#12]     ; COUNT(+0xC) read back
rsb   r1, r3, r2       ; r1 = COUNT - expire
cmp   r1, #0
ldmlt ... (return)     ; (int32_t)(COUNT - expire) < 0  -> compare point still ahead -> OK
printk "...counter(%ld) has caught up expire(%ld)!"   ; else: it is already past
ldr   r3, [r4,#12]     ; COUNT again
add   r3, r3, #10      ; expire := COUNT + 10
b     ...              ; rewrite CMP/CTRL and re-verify (loop)
```

So the warning means: *at the moment the channel was armed, its free-running counter had already reached the compare
value, so the hardware match interrupt for this expiry can never fire.* The driver recovers by pushing the compare
point 10 ticks ahead and retrying. This is a **pure software race check** on the counter the model returns from +0xC;
it does not read any state the model gets wrong.

### Callers and the 10-tick margin
* `hwtimer_mod_timer` (0x40052CB8): reads the per-CPU clock (`hwtimer_read_clock_ch` → COUNT of the CPU's own
  channel), and if the requested expiry is within 10 ticks of / behind COUNT it clamps `expire = COUNT + 10`
  (`cmn r0,#10 / bmi`). It then unlinks/relinks the timer in a per-CPU list under `_raw_spin_lock_irqsave`, and if this
  timer is now the earliest it calls `hwtimer_on`. The clock read and the `hwtimer_on` verify read are therefore
  separated by the list work **and** the spinlock acquisition.
* `hwtimer_intr` (0x40052728): the match ISR. It walks the expired list; for an entry whose deadline is still ahead it
  re-arms with `hwtimer_on(ch, deadline)` — and when the ISR itself ran late, that deadline is already behind COUNT,
  producing the same warning from hard-IRQ context (the variant that feeds the UART deadlock).

The margin is **10 ticks = 10 µs** at the ch5..8 rate (CTRL 0x1132, prescaler `0b10` → 4 MHz ≫ 2 = 1 MHz). On real
hardware the few CPU cycles between the two counter reads advance it well under one tick, so `COUNT - expire` is
always negative and the branch is never taken.

## 2. Why the emulated counter outruns the margin

`cxd4115_timer.c` derives COUNT from virtual time:

```c
int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
uint64_t ticks = muldiv64(now - c->base_ns, chan_hz(c), NANOSECONDS_PER_SECOND);
return c->base_count + (uint32_t)ticks;   /* chan_count() */
```

This is the correct hardware behaviour: a free-running counter advancing at `chan_hz`. The problem is what
`QEMU_CLOCK_VIRTUAL` is between the caller's clock read and `hwtimer_on`'s verify read:

* **Without icount (default, MTTCG).** `QEMU_CLOCK_VIRTUAL` is slaved to the host monotonic clock while the guest
  runs. Everything that happens in the read→arm window costs real host time and therefore advances the counter:
  - ~40 guest instructions of `hwtimer_mod_timer` list work;
  - 3–4 MMIO accesses (read clock, write CMP, write CTRL, read COUNT), each a `prepare_mmio_access` BQL round trip
    (`system/physmem.c`), which can block while the main loop or another vCPU holds the BQL;
  - any point at which the host scheduler deschedules this vCPU thread (each MTTCG vCPU is a host thread — see
    `docs/RE_ENGINE_OOPS.md`). Under the shared-host load here (12 agents, load 16–52) that is routinely tens of µs,
    occasionally tens of ms (r1 overshoot max 75 726 ticks ≈ 76 ms — a long deschedule).
  The firmware's 10 µs head-room assumes silicon-speed execution; the emulated window exceeds it whenever the host is
  busy, so the counter "catches up" and the warning fires. This is the same host-time-leakage that
  `docs/RE_ENGINE_OOPS.md` identifies for the engine stale-kick window.

* **With icount (`shift=auto`, r4).** `QEMU_CLOCK_VIRTUAL` now advances with guest instructions, independent of host
  load — yet 90 warnings still appear, with overshoots up to 1808 ticks. Cause: icount forces **single-threaded
  round-robin TCG**. When `hwtimer_mod_timer` on one vCPU hits the per-CPU hwtimer spinlock (or any contended lock)
  held by a task on another vCPU, the holder cannot run until the round-robin scheduler switches to it; the spinning
  vCPU burns its instruction quantum (advancing virtual time by hundreds of ticks) before the lock is released. So
  icount trades host-time jitter for scheduler-quantum jitter; it reduces but does not remove the warnings.

Either way the inflated interval is **wall-clock / scheduling time injected into a guest critical section**, not a
miscount by the timer model.

## 3. Verdict: emulation artefact, not a model bug

The register model is faithful on every value the firmware reads here:
* COUNT (+0xC) is monotonic, free-running, and rated at 1 MHz for CTRL 0x1132 (verified: ch5..8 counts advance
  linearly and the inter-channel skew is zero at idle, growing only with host load).
* CMP/CTRL/free-run/reload/IRQ semantics match `hwtimer_on`'s and `hwtimer_intr`'s access patterns (already V in
  `docs/HARDWARE.md`; the free-run bit5 fix in `docs/RE_ENGINE_TICK.md` §8 made the per-CPU clock keep counting
  through a match, which is the behaviour `hwtimer_on` relies on).

The warning is the firmware defending against a real-hardware race (an expiry programmed into the past). On silicon
the race never happens because the arm sequence is sub-µs; in the emulator it happens because the arm sequence spans
host/scheduler time. **No change to the modelled registers would make the guest's 10 µs critical section atomic with
respect to virtual time** — that is a property of the execution model (TCG + host scheduling), not of the device.

Real-camera logs confirm the real device does not trip it: `re/real_kernel_log.txt` (146 lines around a suspend) and
`re/dmesg_live.txt` (93 lines, boot to ~18 s plus a late window) contain zero `caught up` lines — short samples, so
`[?]` on the absolute rate, but consistent with a race that silicon never loses.

## 4. The counter is correct; the warnings are benign to the guest

The firmware's `+10` retry always succeeds (the loop terminates as soon as the freshly-read COUNT + 10 is ahead of
itself), so the timer is still armed and still fires; no deadline is lost on this path. The only harm is downstream:
the hard-IRQ-context variant in `hwtimer_intr` queues a printable `<4>` message under PREEMPT_RT, which the UART2
console path later flushes under the port lock and deadlocks — **already fixed** in `docs/RE_UART_RESUME.md` by adding
`loglevel=4` to the debug console token so the `<4>` lines are filtered before the port write. The stock cmdline
(`console=ttynull`) never prints them at all.

## 5. Why no `hw/timer/cxd4115_timer.c` fix

Candidate model changes were considered and rejected:

1. **Snapshot / freeze COUNT during the arm window.** The model cannot know where the guest's critical section begins
   or ends; freezing the counter around MMIO would desynchronise it from `sched_clock` (same channel read via
   `cxd4115_read_cycles`) and break timekeeping. Rejected — breaks fidelity.
2. **Return `expire - 1` from the verify read when CMP was just written.** This would special-case the exact
   read-back `hwtimer_on` does, lying about the counter. It would mask the warning but corrupt any other reader (the
   kernel clocksource, `hwtimer_read_clock`) and still leave `hwtimer_intr`'s late-ISR variant. Rejected — a hack that
   violates "model the hardware, never fake firmware-visible state" (`AI_WORKFLOW.md` §1).
3. **Raise `chan_hz` precision / change the prescaler.** The rate is correct (V). Changing it would desynchronise
   `sched_clock`. Rejected.

The artefact is removed only by making virtual time not leak host/scheduler time into the guest critical section,
which is a **machine/accelerator-level** concern, not a device register concern:
* `-icount shift=auto` reduces the rate and makes it host-load-independent but does not eliminate it (§2, r4) and
  slows the whole machine (single-thread round-robin); it is a debugging aid, not a default.
* The real fix for the only observed harm (the UART deadlock) is the console `loglevel=4` token already in
  `docs/RE_UART_RESUME.md`.

**Recommendation:** leave `cxd4115_timer.c` unchanged; treat the warnings as a known cosmetic TCG-timing artefact,
documented here and silenced on the debug console by the existing loglevel fix. Promote `[?]` on the real-camera rate
if a longer real-kernel-log sample becomes available.

## 6. Acceptance tests (for any future mitigation, if one is ever pursued)

A mitigation is only acceptable if **all** hold (binary = camera boot, pristine NAND clone per run):

1. **No warnings at idle.** Boot to the shooting screen and idle 5 minutes with no key input; `busybox dmesg` shows
   **zero** `hwtimer_on: counter(...) has caught up expire(...)` lines (today: ~0.3–0.7 /s).
2. **Timers still correct — boot.** Time from power-on to the shooting screen (grid + `SCN` + `NO CARD`, as in r2b's
   screendump) is unchanged within the 2 s polling resolution versus the stock build.
3. **Timers still correct — power save.** Idle power-save suspends the VM ~5 min after the last key (sub-micro off,
   factor 8, `docs/RE_REBOOT.md`), and `sendkey o` (power lever) resumes to the shooting screen.
4. **Timers still correct — HOT resume.** After an APO + lever HOT resume (DDR kept, `docs/RE_UART_RESUME.md` §4), the
   guest shell answers `cat /proc/uptime` and a 256 KiB `a5kshell.py push`/`pull` round-trips with matching md5
   (clock/scheduling intact).
5. **Clocksource intact.** `sched_clock` / `cxd4115_read_cycles` read the same channels (+0xC); kernel uptime
   timestamps advance monotonically and at real rate (dmesg timestamps vs host elapsed within ±2 %).
6. **Stock unchanged.** Without debug options, `/proc/cmdline` is byte-identical to `a5k_default_cmdline`
   (`console=ttynull`, no `loglevel=`).

Negative control: a build that shortens the arm window artificially (e.g. by freezing COUNT) must be shown to fail
test 5 — proving the test can detect a broken clocksource.

## 7. Reproduce

* `A5K_NAND=<clone> A5K_QEMU=<qemu-hwt> emulator/run.sh -display none -monitor tcp:127.0.0.1:46630,server,nowait
  -plugin tools/liba5ktrace.dylib,ts=1,words=0,pc=0x40052654 -d plugin -D <log>` — every `A5KT ... pc=40052654` line
  is one warn printk; `r2`,`r3` on the line are COUNT and `expire`, `(r2-r3)` the overshoot in ticks. `lr=40052de0`
  = armed from `hwtimer_mod_timer`, `lr=400527e8` = from `hwtimer_intr`.
* Counter check: HMP `xp /1wx 0xf2403500+0x100*ch+0xc` (ch 5..8) — monotonic, 1 tick/µs; sample two channels
  alternately to see the load-dependent skew.
* icount comparison: add `-icount shift=auto`; warnings drop but persist and become host-load-independent.
* Clean count (no plugin overhead): boot with `A5K_SHELL=<port>`, after 4–5 min `a5kshell.py run 'busybox dmesg'` and
  grep `caught up`.

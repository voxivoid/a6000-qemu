# UART2 debug console after a HOT resume: the `amba2:1:TXRIS` deadlock

Scope: why CPU0 deadlocks in the Sony PL011 driver after an idle power save + lever HOT resume when the debug shell
console is on UART2 (`console=ttyAM1 amba2.console=1`, `shell-socket=`), whether QEMU's PL011 model is at fault, whether
the board should reset UART2 on a HOT power-on, and a spec for the code agent. RE only; no emulator code was changed.

Related: `docs/RE_WARM_RESUME.md` §7 (the issue as first seen), `docs/RE_GUEST_SHELL.md` (UART ownership, ttyAM1),
`docs/HARDWARE.md` (UART0/1/2 at 0xF2000000/1000/2000, IRQ 150-152). `[?]` = not proven.

Evidence:
* Static: `re/vmlinux.elf` (objdump): the Sony PL011 driver (`pl011_*` at 0x401B2C54..0x401B3EC8), printk
  (`vprintk` 0x40060C80, `console_trylock_for_printk` 0x400604F0, `__call_console_drivers` 0x40060218),
  `rt_spin_lock_slowlock` 0x402894E0, `hwtimer_on` 0x4005261C. QEMU `hw/char/pl011.c` (v10.1.0, unmodified).
* Dynamic: binary `18850ab` (HOT resume branch), pristine NAND clone per run, `A5K_SHELL`, gdb stub, no SD:
  * **r1**: hardware breakpoints on `pl011_suspend`, `pl011_resume` and the TXRIS `printk` call; APO + lever.
    The TXRIS printk ran during the resume itself; no deadlock (uptime 377 s answered).
  * **r2**: no breakpoints, `-d trace:pl011_write,trace:pl011_irq_state`; APO + lever; first shell input 40 s later.
    **Deadlock reproduced.** CPU0 backtrace, stack and the kernel log buffer read with gdb / `pmemsave`.
  * **r3**: as r2, but `busybox dmesg -n 1` (console_loglevel 1) before the APO. No deadlock (uptime 357 s
    answered, `[   40.067693] amba2:1:TXRIS` in dmesg).
  * **r4**: as r3 with `busybox dmesg -n 4` (the value proposed in §5). No deadlock (uptime 357 s answered).

---------------------------------------------------------------------------------------------------------------------

## 0. Summary

| Question | Answer |
|---|---|
| What deadlocks? | The Sony PL011 interrupt thread calls `printk("<7>amba2:%d:TXRIS")` (`pl011_int+0x94`) **while it holds the port lock**. If printk finds printable messages pending and gets the console semaphore, it writes them to the console in that same thread: `pl011_console_write+0x88` takes the same port lock again → `rt_spin_lock_slowlock` → `task_blocks_on_rt_mutex` returns -EDEADLK → `BUG()` (`__bug`, rtmutex.c line 752) → data abort → `die` → exception monitor → `em_dump_write` → `pl011_console_write` → `rt_spin_trylock_irqsave` spins forever on the rt_mutex `wait_lock` that the interrupted slowlock still holds. (r2, full chain on the stack, §2.) |
| Is QEMU's PL011 TXRIS behaviour the cause? | **No.** The driver reads RIS.TXRIS = 1 at that point, but the real UART shows the same: the last thing on the port before the IRQ is the console flush of the resume log (hundreds of bytes, ending with a BUSY poll), which drains the TX FIFO through its trigger level and sets TXRIS on real hardware too (§3). QEMU's TX interrupt model differs from the PrimeCell in several ways (§3.2), none of which changes this sequence. |
| What makes it happen in the emulator? | The pending messages. In r2 they are `<4>hwtimer_on: counter(N) has caught up expire(M)!` lines, printed by the Sony hwtimer driver from **hard-IRQ context**. The PREEMPT_RT printk does not write to the console from atomic context (`console_trylock_for_printk`: preempt_count ≠ 0 or IRQs off → store only), so they stay queued until the next printk from a sleepable context — the TXRIS printk in the IRQ thread, which then flushes them under the port lock. The emulator emits these warnings about once a second (109 in r2 up to 210 s); the real camera's kernel log sample (`re/real_kernel_log.txt`, 146 lines around a suspend, local only) has none [? short sample]. This is a timer/TCG-timing artefact, outside the UART. |
| When is the dangerous printk? | Once per port start: `pl011_startup` clears the "TXRIS seen" flag (`uart_amba_port+0xCC`); the first interrupt that sees RIS.TXRIS prints it and sets the flag. On a cold boot that is init's first write at ~0.9 s (nothing pending yet, safe). After a HOT resume `uart_resume_port` calls `pl011_startup` again; the next TX interrupt is the shell's first echo, e.g. 40 s later in r2, when ~40 hwtimer lines are queued. |
| Why is the stock console not affected? | With `console=ttynull` nobody opens ttyAM1, so `pl011_int` never runs for UART2 and the Sony console write is never registered. |
| Would a real camera deadlock? | With `console=ttyAM1 amba2.console=1` (a kemco.txt debug setting) it would do so whenever a printable message is queued from atomic context at the first TX interrupt after a port start. Without the hwtimer artefact that should be rare [?]. The bug is in the firmware (printk under the port lock; the same pattern exists for `amba2:%d:TXFF` at `pl011_int+0x564`). It cannot be removed without changing the firmware. |
| Should the board reset UART2 on a HOT power-on? | **Yes, as it does now.** The micro cuts SoC power for the warm off (RE_WARM_RESUME §0); the UART loses its registers like every SoC block. The kernel expects it: `pl011_suspend` → `uart_suspend_port` → `pl011_shutdown` (IMSC 0, ICR 0xFFF, FIFO off) and `pl011_resume` reprograms everything (set_termios, startup). r1: at `pl011_resume` entry UART2 reads FR 0x90, LCR_H 0, CR 0x300, IMSC 0, RIS 0 — the QEMU reset values, which match the PL011 TRM reset values. No change needed. |
| Fix | No PL011 change. For the debug console only: add `loglevel=4` next to `console=ttyAM1 amba2.console=1` in the board's console token (§5). The queued `<4>` hwtimer warnings are then not printable, so the TXRIS printk finds nothing to write and returns (r3/r4). The stock cmdline is unchanged. The hwtimer artefact itself is a separate RE topic (§6). |

---------------------------------------------------------------------------------------------------------------------

## 1. The Sony PL011 driver ("amba2")

Port array at `0x403A7034`, 0xD8 bytes per port (`uart_amba_port`); UART2 = line 1 = `0x403A710C`. Fields used here
(offsets from the port; the `uart_port` part is the 3.0 layout):

| Offset | Meaning | Evidence |
|---|---|---|
| +0x20 | `membase` (0xF2002000) | every register access |
| +0x34 | IRQ (152) | `pl011_softirq`, `request_threaded_irq` |
| +0x40 | `fifosize` (16) | TX loop counts |
| +0xA0 | line | printk argument |
| +0xBC | `im` (shadow of UARTIMSC) | `pl011_start_tx/stop_tx` |
| +0xC0 | old modem status (FR & 7) | `pl011_startup`, `pl011_int` MS path |
| +0xC8 | console mode (`amba2.console=`, `AMBAUART_IOC_SETMODE`) | `pl011_console_write`, `amba_ioctl` |
| +0xCC | **TXRIS seen** (TX interrupt known to work) | cleared in `pl011_probe` and `pl011_startup`, set in `pl011_int` |
| +0xD0 | **soft TX pending** | set in `pl011_softirq`, consumed in `pl011_int` |

TX interrupt bootstrap (Sony's replacement for mainline's "provoke TX FIFO interrupt" loopback write in startup):

* `pl011_start_tx` (`0x401B2CE0`): `im |= TXIM`, write IMSC, then `pl011_softirq`.
* `pl011_softirq` (`0x401B2C78`): if +0xCC == 0: +0xD0 = 1, `dsb`, and write `1 << (irq & 31)` to
  **GICD_ISPENDR** (`0xF8001200 + (irq >> 5) * 4`): a software-made UART interrupt.
* `pl011_int` (`0x401B3810`, run as a forced IRQ thread under PREEMPT_RT), right after `rt_spin_lock(port)`:
  ```
  mis = UARTMIS
  if (+0xCC == 0) {
      if (UARTRIS & TXRIS) { +0xCC = 1; printk("<7>amba2:%d:TXRIS\n", line); }   /* +0x84..+0x94 */
      else if (+0xD0 && (im & TXIM)) mis |= TXRIS;                              /* fake TX irq */
      +0xD0 = 0;
  }
  ... loop: UARTICR = mis & ~0x70 (TX is cleared only when it is in MIS); RX; modem status; TX ...
  ```
* TX part: while +0xCC == 0 it writes up to `fifosize + 2` bytes and polls FR.TXFF before each; TXFF set →
  +0xCC = 1, `printk("<7>amba2:%d:TXFF\n")` (`+0x564`, also under the lock). With +0xCC == 1 it writes
  `fifosize / 2` bytes blindly. Buffer empty → `pl011_stop_tx`; else (still bootstrapping) `pl011_softirq` again.
* `pl011_startup` (`0x401B3710`): CR = 0, LCR_H &= ~FEN, RSR = 0, **ICR = 0x7D0** (OE, BE, PE, FE, RT, RX — not TX,
  as in mainline), request IRQ, IFLS = 0x12, **+0xCC = 0, +0xD0 = 0**, CR = 0x1701, old status, ICR = 0x50,
  IMSC = im = 0x50.
* `pl011_shutdown` (`0x401B368C`): IMSC = 0, ICR = 0xFFF, free IRQ, CR = 0x1F01 (console keeps TX), LCR_H &=
  ~(BRK | FEN).
* `pl011_console_write` (`0x401B2F50`): returns if mode (+0xC8) is 0. `port->sysrq` or `oops_in_progress` →
  `rt_spin_trylock_irqsave`, else `rt_spin_lock` (**no trylock**). Then CR |= UARTEN | TXE (CTSEn off), IMSC = 0,
  `uart_console_write` (`pl011_console_putchar` polls TXFF), poll FR.BUSY, restore CR, IMSC = im, unlock.
* `pl011_suspend` / `pl011_resume` → `uart_suspend_port` / `uart_resume_port` (serial core). The resume path calls
  `ops->set_termios` for the console, then `pl011_startup`, `uart_change_speed`, and `ops->start_tx` under the lock.

## 2. The deadlock (r2)

Timeline (trace + gdb):

1. Idle power save at ~316 s; `pl011_shutdown` clears UART2 (RIS 0).
2. HOT power-on, board reset; `pl011_resume`: set_termios, startup (+0xCC = 0), `start_tx` → IMSC 0x70, soft
   interrupt. TX buffer empty: the handler fakes TX, writes nothing, `stop_tx` → IMSC 0x50. RIS.TX is 0 here, so
   no TXRIS print yet.
3. `resume_console()` flushes the resume log through `pl011_console_write` (IMSC 0 during the write, restored to
   0x50). The TX FIFO drains: **RIS.TX = 1**, but TXIM is off, so no interrupt and nobody clears it.
4. `Restarting tasks ... done.` From then on `hwtimer_on` warnings (level 4) are printed from hard-IRQ context at
   0.34, 0.60, 0.81 s, ... and stay queued (RT printk, §0).
5. 40 s later the host sends a command; the ash echo → `pl011_start_tx` → IMSC 0x70 → the UART interrupt is raised
   (RIS.TX already 1) and the soft interrupt as well. `pl011_int`: +0xCC == 0 and RIS.TXRIS → printk under the lock.
6. printk: thread context, preempt_count 0 → `console_trylock` succeeds → `console_unlock` → the queued
   `<4>hwtimer_on ...` lines pass the loglevel filter (4 < 7) → `pl011_console_write` → `rt_spin_lock(port)`,
   already owned by this thread → BUG → exception monitor → endless spin in `rt_spin_trylock_irqsave`.

CPU0 at the hang (gdb): `__raw_spin_lock` ← `rt_mutex_trylock` ← `rt_spin_trylock_irqsave` ←
`pl011_console_write+0x70` ← `console_write` ← `em_dump_write` ← `em_get_callstack` ← `em_exception_monitor` ←
`die` ← `__do_kernel_fault` ← `do_page_fault` ← `do_DataAbort`; further up the same stack: `__bug` ←
`rt_spin_lock_slowlock+0xD8` ← `rt_spin_lock` ← `pl011_console_write+0x88` ← `console_unlock` ← `vprintk` ← `printk`
← `pl011_int+0x98` (r0 = `0x4030D481` = `"<7>amba2:%d:TXRIS\n"`). The printk timestamp in the stack frame is
`[   40.048927]`. UART2 at the hang: IMSC 0x50, RIS 0x30, port +0xCC = 1.

Kernel log buffer (`log_buf` = `*(0x4037C764)`, 8 KiB, read with `pmemsave`): after `<4>[    0.279427] Restarting tasks
... done.` it holds `<4>[    0.341933] hwtimer_on: counter(333067) has caught up expire(333061)!`, `<4>[
0.601819] hwtimer_on: ...`, ... which never reached the console or the klog ring. The `__call_console_drivers`
arguments on the stack (0xA24F..0xA298) are exactly the first of them: that is the line the faulting console write was
emitting.

r1 did not deadlock because there the first TXRIS interrupt ran during step 2-3 (breakpoint timing [?]); printk was
then refused by `console_suspended` or found nothing queued, and +0xCC = 1 afterwards.

## 3. PL011 TX interrupt: QEMU vs the PrimeCell

### 3.1 The sequence above on real hardware

PrimeCell UART (PL011) TRM, UARTTXINTR: with the FIFOs enabled the TX interrupt is asserted when the TX FIFO level
reaches the programmed trigger level (here IFLS TXIFLSEL = 010, 1/2 = 8 bytes) and stays asserted until data is
written above the trigger level or it is cleared through UARTICR; with the FIFOs disabled it is asserted while the
single holding register is empty. Linux (and Sony, §1) treat it as asserted only on the transition, which is why
neither expects TXRIS straight after reset.

* After reset: RIS = 0 on both (TRM reset value; QEMU `pl011_reset` int_level = 0).
* Step 3: the console flush writes far more than 8 bytes and ends with FR.BUSY = 0, so the FIFO passed the trigger
  level on the way down: **TXRIS = 1 on the real UART**, held because nothing clears it (TXIM off, no ICR write).
  QEMU: TXRIS = 1 (every DR write sets it).
* Step 5: the real UART raises the interrupt as soon as TXIM is set; so does QEMU.

So the driver sees the same RIS value on both at the moment that matters; the difference is only how long the
flush takes (≈ 87 µs per byte at 115200 baud on the real UART, instant in QEMU).

### 3.2 Remaining model differences (not changed by this spec)

| Behaviour | Real PL011 | QEMU `pl011.c` |
|---|---|---|
| TXRIS on a DR write | cleared when the FIFO level goes above the trigger, set again when it drains to it | set on every write, never cleared by writes |
| TX timing | drains at the baud rate, FR.BUSY / TXFF / TXFE follow | instant; TXFF never set, TXFE always set |
| TXRIS after short writes (≤ trigger level) from empty | not asserted (no transition) [?] | asserted |

Consequences for this firmware: the Sony `amba2:%d:TXFF` probe is unreachable in QEMU, so the driver always leaves
bootstrap mode through the TXRIS path; and a write of 1-8 bytes to an idle port sets TXRIS in QEMU where the real
UART would not [?]. None of this is needed for the resume issue, and `pl011.c` is shared with every other machine;
leave it unchanged until a case needs it.

### 3.3 UART state across the HOT power-on

The warm off cuts SoC power while DDR stays in self-refresh (RE_WARM_RESUME §0, §1.2), so UART2 comes up with reset
values, and the board's HOT path (`qemu_system_reset_request` → `pl011_reset`) does exactly that. The kernel does
not rely on any UART state surviving: it shut the port down before the off and reprograms baud rate, line control,
FIFO level, CR and IMSC in `pl011_resume` (r2 trace: CR 0, FBRD 8, IBRD 0x4E, LCR_H 0x70, ... IMSC 0x50). The
bootloader is not known to touch UART2 [?]. **Keep the reset.**

## 4. Experiments

| Run | Before the APO | First shell input after the lever | Result |
|---|---|---|---|
| r1 | (breakpoints) | ~60 s | TXRIS printk during resume; no deadlock; `377.57` |
| r2 | — | 40 s | **deadlock** (§2) |
| r3 | `busybox dmesg -n 1` | 40 s | no deadlock; `357.32`; `[   40.067693] amba2:1:TXRIS` |
| r4 | `busybox dmesg -n 4` | 40 s | no deadlock; `357.37`; `[   40.080293] amba2:1:TXRIS`, preceded in dmesg by `hwtimer_on` warnings at 39.11, 39.14, 39.47 s (queued, filtered) |

The RE_WARM_RESUME §7 observation (deadlock with the shell) is a second independent reproduction of r2.

## 5. Spec for the code agent

Goal: the debug shell survives the APO + lever HOT resume. Firmware, `pl011.c` and the stock cmdline stay unchanged.

1. `hw/arm/sony_a5000.c`, `a5k_console_token()`: for `ttyAM1` return
   `"console=ttyAM1 amba2.console=1 loglevel=4"` instead of `"console=ttyAM1 amba2.console=1"`. Comment: the Sony
   PL011 interrupt thread printks `amba2:%d:TXRIS` under the port lock (`pl011_int+0x94`) at the first TX interrupt
   after every port start (cold boot ~0.9 s, every HOT resume); if printable messages are queued from atomic context
   at that moment (the emulator's `hwtimer_on ... caught up` warnings, level 4), the console write in the same thread
   re-takes the lock → `BUG` → CPU0 dead (docs/RE_UART_RESUME.md). `loglevel=4` keeps levels 0-3 (errors, BUG,
   panic) on the console. The kernel parses `loglevel=` (early_param at `0x403341BC`); stock userland does not change
   `/proc/sys/kernel/printk` (r1: `7 4 1 7` at 72 s).
2. Leave `ttyAM0` and `ttynull` as they are (ttynull = byte-identical stock cmdline).
3. Docs: one line in `docs/RE_GUEST_SHELL.md` §6 (console token now carries `loglevel=4`, why, and that `busybox dmesg
   -n 7` restores full kernel output on the shell at the user's own risk); `docs/RE_WARM_RESUME.md` §7 known issue
   → points here.

Residual risk (documented, not fixable without changing the firmware): a level 0-3 message queued from atomic
context at that moment still deadlocks, on a real camera with this kemco setting as well.

### Acceptance tests

Common: copied binary, fresh NAND clone, `A5K_SHELL=<port>`, monitor on TCP, no gdb breakpoints.

1. **Resume.** Boot to the shooting screen; `tools/a5kshell.py --port <p> run 'cat /proc/sys/kernel/printk'` →
   `4	4	1	7`. No input until the idle power save (~316 s, `warm off, sus_error 0: DDR kept`). HMP `sendkey o` twice
   (`power-on, HOT resume (DDR kept)`). Wait ≥ 40 s (lets hwtimer warnings queue up; the deadlock needs them).
   `a5kshell.py run 'cat /proc/uptime'` answers with uptime > 316 s. `a5kshell.py run 'busybox dmesg | grep -c
   "TXRIS\|__bug\|BUG"'` shows the TXRIS line from after the resume (timestamp ≥ 40 s) and no `BUG`. Repeat the APO /
   lever cycle once more in the same run.
2. **Shell output intact.** After 1, `a5kshell.py push` / `pull` of a 256 KiB file (RE_GUEST_SHELL §5.1) still
   matches its md5.
3. **Stock unchanged.** Without `A5K_SHELL`, `/proc/cmdline` (via klog `Kernel command line:`) is byte-identical to
   `a5k_default_cmdline` (`console=ttynull`, no `loglevel=`).
4. **Negative control** (optional, shows the test can fail): as 1, but run `busybox dmesg -n 7` before the APO →
   CPU0 deadlock as in §2 (no answer from the shell).

## 6. Open questions

1. `hwtimer_on: counter(N) has caught up expire(M)!` (`hwtimer_on` `0x4005261C`: writes the compare value, reads the
   counter at +0x0C, warns and retries with +10 ticks when the counter is already past it; deltas of 3-35 ticks at
   2 MHz in r2). About one per second in the emulator, none in the real kernel log sample (146 lines). Likely host/TCG timing
   between the caller's counter read and the compare write [?]. Separate RE topic (CXD4115 timer, `cxd4115-timer`);
   fixing it removes the trigger in practice but not the firmware's latent bug.
2. TXRIS after short writes from an idle FIFO on the real UART (§3.2) [?].
3. Whether the Sony bootloader programs UART2 on a HOT power-on [?] (irrelevant to the kernel, which reprograms it).

## 7. Reproduce

* gdb (no watchpoints): `hbreak *0x401b38a4` (TXRIS printk call in `pl011_int`); UART2 registers `x/12wx
  0xf2002018` (FR, ILPR, IBRD, FBRD, LCR_H, CR, IFLS, IMSC, RIS, MIS, ...); port fields `x/16wx 0x403a71ac` (+0xA0 ..).
* At the hang: `thread 1`, `bt`; raw stack from `sp` (the frames through `pl011_int` are above `die`).
* Log buffer: `x/2wx 0x4037c760` (`log_buf_len`, `log_buf`), then `pmemsave <log_buf - 0x40000000 + 0x80000000>
  0x2000 file`.
* `-d trace:pl011_write,trace:pl011_irq_state` shows the resume reprogramming of UART2 (interleaved with UART0's
  av-cam monitor writes; the trace has no instance id).

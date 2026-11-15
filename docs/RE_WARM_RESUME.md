# Warm power-off and power-on: suspend-to-RAM resume vs WBI snapshot boot

Scope: what really happens on the a5000 (firmware 1.10) when it powers off for the idle power save (factor 8) or the
power lever (factor 7), and how the next power-on gets the system back. Ends in a spec for the board
(`hw/arm/sony_a5000.c`, standing in for the Sony bootloader) and acceptance tests. RE only; no emulator code was
changed for this document.

Related: `docs/RE_REBOOT.md` (§1.4 IRAM code, §4.1 reset kinds, section B off kinds), `docs/RE_UPDATER_BOOT.md`,
`docs/HARDWARE.md` (boot contract), `docs/RE_RTC.md` (hand-off +0xE4). `[?]` = not proven.

Evidence:
* Static: `re/vmlinux.elf` (objdump; IDA 9.4 + Hex-Rays on windev, private copy of the kernel IDB under
  `C:\a5000\emu\warm\`), `/usr/bin/im.elf` (objdump, Thumb), `upm.ko` (`re/ida/kmod_i2cgrp/upm.c`), the factory WBI
  (`nflasha5`, header read directly; RAM content via `tools/wbi.py`), the unit's Backup.bin
  (`backup_props_all.tsv`).
* Dynamic (binary `910eae2`, pristine NAND clone per run, no SD, `A5K_SHELL`, gdb stub):
  * **w1**: boot, no input, idle power save at 316 s; gdb hardware breakpoint on the IRAM power-off store
    (`0xC00200C8`); memory read there; then `pc = 0x800523CC` (the resume vector) without a reset.
  * **w2**: as w1, plus an attempt to set SCTLR from gdb at the MMU-enable instruction (QEMU's gdbstub ignores the
    write; no result).
  * **w3**: as w1, but the XN bit of the DDR identity section is cleared in both L1 tables for the MMU-enable step only
    (restored at `resume_after_mmu`), diagnostic gdb memory writes. Devices are **not** reset (no board support yet).

---------------------------------------------------------------------------------------------------------------------

## 0. Summary

| Question | Answer |
|---|---|
| Is a warm off a suspend-to-RAM? | **Yes, and so is every other off.** Every off of firmware 1.10 ends in the same kernel suspend (`"mem"`, state 3) with the IRAM code putting DDR into **self-refresh** before it drops p0.10. There is no deep power-down: the kernel has no `ddr_ctrl` parameter (upm's `"dpd,dpd"` write goes nowhere, §1.3). |
| What makes it warm or cold? | Only two flags: the **slot-A TX +0x1B bit 0** to the sub-micro (warm = 1) and **`pm.sus_error`** (hand-off +0x60): upm writes 1 for a cold off (type 28), 0 stays for a warm one. The kernel writes the resume vector **hand-off +0x54 = 0x800523CC** (PA of `cxd90014_cpu_resume`) at every kernel init. |
| Power-on after a warm off | (static, `[?]` for the bootloader itself) The micro keeps DDR powered in self-refresh and cuts the SoC. On the lever/key it powers the SoC up, the Sony bootloader runs from reset, sees "warm" and `sus_error == 0`, and **jumps to `*(0x80021054)` with the MMU off**. The kernel resumes in place: same processes, same screen, uptime continues. The loader boot mode for this is **2 = HOT** ("ON:fast EE(Hot)" etc. in upm). The unit's own Backup.bin holds 0x51000F = 2: its last real power-on was a HOT resume. |
| Power-on after a cold off (or a lost DDR) | The bootloader loads the **WBI** (factory snapshot in `nflasha5`, header `WBI1`) into DDR and jumps to its header resume vector **0x800523BC** (`cxd90014_cpu_resume_ssboot`). Boot mode 11 = SS. If the WBI is invalid (dropped), it boots `vmlinux.bin` + initrd from nflasha3 (boot mode 1 = COLD), which is what the emulator always does today. |
| WBI vs suspend-to-RAM | Two different resumes through the same kernel code: WBI = snapshot of a booted system restored from NAND (DDR lost); HOT = the live DDR image. The kernel tells them apart by `pm_mem_alive` (0x403946BC): 1 in a live DDR image, 0 in the snapshot (it is set only after the image was taken). The `"warm"` state (4) that **creates** a WBI is not used on this unit (Backup 0x510023 = 2, §1.2). |
| Emulator blocker found | The resume path turns the MMU on and executes **two more instructions from the XN identity mapping** of DDR (`0x80011452`, domain client). Real Cortex-A5 runs them from its prefetch; QEMU applies the SCTLR write at once and takes a permission fault (w1). A CPU-model fix is needed (§3.3), not a firmware change. |
| Does the rest work? | With that step bypassed (w3) the unmodified kernel resumes in place at uptime 317 s: CPU1..3 come back, every UDIF driver resumes (`PM: resume of devices complete`, `[LIRO]: liro resume complete`), `Restarting tasks ...`. Then an av-cam task hits its fatal handler (end of §3.3); w3 did not reset the devices, so this is not yet a verdict on the real path. |

---------------------------------------------------------------------------------------------------------------------

## 1. What a warm off saves

### 1.1 From upm to `/sys/power/state`

1. upm, state 34 (`sus`), `k_upm_off_factor_handler_inactive` case 0x92014E (GI_CMP) logs `"EV OFF Warm boot"` when
   `sub_E0E4()` is true and sends **0x92014F** to im.elf with **msg[1] = `sub_E0E4()`**:
   `sub_E0E4` = Backup **0x510023** (`dword_14D98`, read in `k_upm_Backup_read_off_factor`) ≠ 0 **and** off type 27
   **and** factor ∈ {7, 8, 9, 10, 23, 31}. On this unit 0x510023 = 2, so APO and lever offs give msg[1] = 1.
2. im.elf case 0x92014F (`0xA258`): `a = 2` if the factor is 39; else `a = (Backup 0x510023 == 1) && msg[1]`
   (`0xA26E`: `ldr r3,[0x154D0]; cmp r3,#1`; 0x154D0 is read from 0x510023 in `main` at `0x9C6E`). Then `sub_9FB4(a)`:
   * `a == 2` → write `"warm"`;
   * otherwise `sub_9F68` (if Backup 0x940005 ≠ 0: warm count 0x940004 += 1), then, if im.elf flag bit 2 is set, write
     nothing; `a == 1` → `"WARM(warm_count=N)"` + `"warm"` while count ≤ limit (0x940005; 0 = no limit), else
     `"SUS(..)"` + `"mem"`; `a == 0` → **`"mem"`**.
   * On this unit (0x510023 = 2, 0x940005 = 0) every off writes **`"mem"`**, warm or cold.
3. Kernel `state_store` (`0x40091EF8`), `pm_states` at `0x4028C688`: **1 `standby`, 3 `mem`, 4 `warm`**.

### 1.2 Kernel suspend, state 3 (`"mem"`)

`suspend_devices_and_enter` → `platform_suspend_ops` at `0x4037BE60` {valid, `cxd90014_pm_begin`, `cxd90014_pm_prepare`,
`cxd90014_pm_enter`, …, `cxd90014_pm_finish`}:

* `cxd90014_pm_begin` → `warm_boot_pm_begin(state)`: stores the state (`0x4037947C`, read by `pm_get_state`) and returns
  at once unless state == 4. **No WBI work for `"mem"`.**
* `cxd90014_pm_prepare` (`0x400520E0`): `udif_driver_suspend`; on failure `wbi_drop`, **sus_error word := 1**
  (`*(pm.sus_error - 0x40000000) = 1`), and the sleep entry is called with r0 = 1 (skip WBI, power off anyway). Then
  `warm_boot_pm_prepare` (no-op unless state 4).
* `disable_nonboot_cpus`: CPU1..3 park in `platform_cpu_die` (`0x40283CF4`, `wfi` until `pen_release == cpu`). w1: all
  three at `0x40283CF8`.
* `cxd90014_pm_enter` (`0x40052220`) calls the sleep entry `0x4037BEA0` with **r0 = 0**:
  1. push r4-lr; save SCTLR, ACTLR, sp; then DACR, CPACR, TTBCR, TTBR0, TTBR1, CONTEXTIDR, TPIDR×3, PRRR, NMRR;
     **`sleep_save_sp` (`0x4005249C`) := PA of that block** (`sleep_phys_sp`: VA + 0x40000000). w1: 0x8AB83E28.
  2. SVC, flush caches; `r0 == 0` → `cxd90014_create_warmbootimage()`. With `cxd90014_ssboot_stat` (`0x400523A0`) = 0, 1,
     2 or other it calls `cxd_create_warmbootimage` / `cxd_rewrite_wbheader` with the entry `cpu_resume_profile` /
     `_optimize` / `_ssboot_pre` (+ `em_reboot`) / `cxd90014_cpu_resume`; `cxd_create_warmbootimage` returns 0 at once
     unless state == 4. **So for `"mem"` nothing is written to NAND.**
  3. **`pm_mem_alive` (`0x403946BC`) := 1**, copy `0x4037BFA0..0x4037C174` to IRAM 0xC0020000, flush, SCTLR C/I off,
     `pl310_shutdown`, DACR = ~0, jump to 0xC0020000.
* IRAM code (offsets from 0xC0020000; RE_REBOOT §1.4, corrected here):
  * +0x00 MMU off.
  * Bytes at +0xF4 / +0xF8 are **`pm.pasr0` / `pm.pasr1`** (kernel params, ulongH at `0x4037C094/98`; partial-array
    self-refresh masks per channel), not `ddr_ctrl`. Non-zero → write it to DDRC +0xC4 / +0xD8, +0x12C = 0x1000,
    +0xAC = **0x00800011 then 0x02800011** (ch1 0x00800111 / 0x02800111; RE_REBOOT's 0x11008000 is a byte-order
    misreading of literals `0x4037C164..70`), poll +0x128 bit 12. Not in the cmdline → **0, skipped** (w1: 0, 0).
  * +0x80 DDRC +0x78 bits[31:24] := 0x0A, poll (+0x7C & 0xFF) == 0x25: **self-refresh entry, done on every off** [? name;
    the only DDR state change in the code].
  * +0xA4 byte +0xFC = **`pm.sus_msg`** → `"E\r\n"` on UART0 (0, skipped).
  * +0xC8 `*(0xF2910048) = 0x400` (p0.10 low, `cfg.poff=0,10`; w1: words +0xEC/+0xF0 = F2910048, 00000400).
  * +0xDC `wfi; b .` — **no way out but a reset**: the resume can only come from a power-on.

State at the p0.10 store, idle power save (w1, gdb, CPU0 at `0xC00200C8`, CPSR 0x600001D3, SP 0xC0020000):

| PA | meaning | value |
|---|---|---|
| 0x80021054 (hand-off +0x54, `pm.resume=`) | resume vector | **0x800523CC** (`cxd90014_cpu_resume`) |
| 0x80021060 (hand-off +0x60, `pm.sus_error=`) | suspend error / do not resume | **0** |
| 0x8002104C / 0x80021050 | boot-factor format / ? | 7 / 1 (ROM blob values) |
| 0x803946BC | `pm_mem_alive` | **1** |
| 0x8037947C | PM state | **3** (`mem`) |
| 0x800523A0 | `cxd90014_ssboot_stat` | 0 (the emulator cold-boots vmlinux) |
| 0x8005249C | `sleep_save_sp` (PA of the context) | 0x8AB83E28 |
| 0xC0005010.. | SMP mailbox | 0x8004E6E0 ×3 (stale; CPU1..3 are in `platform_cpu_die`) |

### 1.3 Warm vs cold off: what differs

| | warm (type 27: APO 8, lever 7, …) | cold (type 28: SWITCH_MODE 13, reset 20, battery, heat) |
|---|---|---|
| im.elf → `/sys/power/state` | `"mem"` (this unit) | `"mem"` |
| upm `ddr_ctrl` write | — | `"dpd,dpd"` to `/sys/module/pm/parameters/ddr_ctrl` (`k_upm_snd_hcrn_ura`). **The kernel has no such parameter**: `param_ops_ddrctrl` (`param_set_ddrctrl` = print `Usage: %s={no\|sref\|dpd}`, return -EINVAL) is exported but no built-in param and no module uses it, and the string `ddr_ctrl` is not in the kernel. The write fails; the IRAM code is the same. |
| `pm.sus_error` (+0x60) | stays 0 (cleared by `cxd90014_pm_setup` at kernel init) | **1** (`k_upm_set_sus_error`, when the type-28 factor becomes the strongest) |
| sub-micro slot-A TX +0x1B bit 0 | 1 | 0 |
| DDR at p0.10 | self-refresh | self-refresh |

So the decision "keep the RAM image" is not made by the SoC. It is made by the micro (does it keep DDR powered?) and
the bootloader (does it jump to the resume vector?), from +0x1B bit 0 and `sus_error` [? the bootloader is not in the
update package; this follows from what the kernel leaves for it].

## 2. How the real camera powers on

### 2.1 Three boot kinds

| loader boot mode (Backup 0x51000F, written by the bootloader) | upm text (`k_upm_setup_boot_factor`) | what the bootloader does [?] | kernel entry |
|---|---|---|---|
| **2 HOT** | `ON:fast EE(Hot)` / `Play(Hot)` / `Other(Hot)` | DDR kept (self-refresh), `sus_error == 0`: exit self-refresh, jump to `*(hand-off +0x54)` | `cxd90014_cpu_resume` 0x800523CC, MMU off |
| **11 SS** | `ON:fast EE(SS)` … | load the WBI (nflasha5) into DDR, jump to the header resume vector | `cxd90014_cpu_resume_ssboot` 0x800523BC (header +0x0C) |
| **1 COLD** | `ON:fast EE(Cold)` … | WBI dropped/invalid: load `vmlinux.bin` + initrd, kemco cmdline | 0x80038000 (head.S) |
| 10 WARM | `ON:fast EE(Warm)` … | after a `"warm"` (state 4) off, unused here [?] | — |
| 4 CIPA, 5 ADJUST, 6 USBJ, 12 BIS | | special modes | |

Evidence for HOT = RAM resume: (a) the kernel stores a PA resume vector into the hand-off page and the code at it
restores the CPU context saved in DDR (`sleep_save_sp`), which only exists in the live image; (b) the IRAM code
enters self-refresh and then waits for a reset, never for an interrupt; (c) `pm_mem_alive` is set to 1 right before
the IRAM jump, after any snapshot could have been taken, and `cxd90014_pm_enter` prints `WARMBOOT` and calls
`ssboot_setmode` only when it is 0 (a snapshot resume); (d) `sus_error` exists to veto it; (e) the unit's Backup.bin has
0x51000F = 2 (HOT) [? loader-owned prop; written to the RAM copy at each power-on and flushed later].

### 2.2 The resume path (`cxd90014_cpu_resume`, `0x400523CC`, entered at its PA)

```
msr cpsr_c, #0xD3                 SVC, IRQ/FIQ masked (any entry mode works)
v7_flush_kern_cache_all; dsb; isb; TLBIALL
SCTLR &= ~(M|A|C|I|..)            MMU/caches off (they already are)
pl310_init                        L2 at 0xF8002000 (from reset state)
cxd90014_smp_init                 mailbox 0xC0005010/14/18 := PA of cxd90014_secondary_entry, clear 0xC000501D..1F, SEV
r0 = sleep_save_sp (PA)           restore DACR, CPACR, TTBCR, TTBR0, TTBR1, CONTEXTIDR, TPIDR*, PRRR, NMRR, SCTLR, ACTLR, sp
SCU: +0x0C = 0xFFFF, +0x00 |= 1   (0xF8000000)
ACTLR |= SMP
resume_turn_on_mmu (0x40052480):  mcr SCTLR (MMU on); nop; mov pc, r1 (= resume_after_mmu VA 0x400524A0)
resume_after_mmu:                 r0 = 0; pop {r4-pc} -> back in cxd90014_pm_enter after the sleep-entry call
```

Then `cxd90014_pm_enter`: `cpu_init`; if state == 4 and `pm_mem_alive == 0` → `WARMBOOT` + `ssboot_setmode` (snapshot
case only); `cxd90014_early_resume` (`pl310_enable`, `xrstreq`, WDT, FIQ), `boot_time_takeover`,
`cxd90014_timer_early_init`; then the normal `suspend_devices_and_enter` unwinding: `syscore_resume`,
`enable_nonboot_cpus` (CPU1..3 are expected to be in the boot-ROM wait loop and are released through the mailbox),
`cxd90014_pm_finish` (`warm_boot_pm_finish`, `udif_driver_resume`, `wbi_resume_drop`), device resume, thaw tasks.
upm's `k_upm_resume` re-reads the boot factor (hand-off +0x4C via Backup 0xFA0029, 0x510013, 0x51000F, ...), so the
bootloader's per-power-on values are taken again on a HOT resume.

What the kernel needs from the bootloader for a HOT resume:

| Item | Requirement | Evidence |
|---|---|---|
| DDR | contents unchanged since the p0.10 store (self-refresh exit done by the bootloader) | context at `sleep_save_sp`, page tables, everything |
| entry | PA from hand-off **+0x54**, ARM state, **MMU off**, caches off, any privileged mode; r0..r3 not used | code above |
| L2 / SCU / GIC / timers | in power-on reset state (re-initialised by the resume code and the drivers' resume) | `pl310_init`, SCU writes, `gic_cxd90014_resume` (restores ISENABLER0..7 + PMR only) |
| GIC routing | **the same pre-configuration as a cold boot** (ITARGETSR, ICFGR): the kernel never writes them, also not on resume | HARDWARE.md boot contract, `gic_cxd90014_resume` |
| CPU1..3 | in the boot-ROM wait loop on the mailbox | `cxd90014_smp_init`, `enable_nonboot_cpus` |
| hand-off page | not rebuilt from scratch: +0x54 must survive; the bootloader updates the boot-reason fields (+0x4C, and the RAM Backup props 0x51000F = 2, 0x510013 = reason bit) [? exact field set] | `cxd90014_pm_setup` writes +0x54 only at kernel init |
| SRAM/IRAM bootloader data | NAND dev-info (SRAM +0x3F00/+0x3F40), IBT 0xC0000C00, init_param 0xC0001500 rebuilt as on any power-on [? whether nand.ko re-reads them on resume] | RE_UPDATER_BOOT, HARDWARE.md |
| `sus_error` | 0, else the bootloader must not resume | §1.3 |

### 2.3 The WBI (snapshot boot), for comparison

* `nflasha5` header (`fwtool` `WbiHeader`): magic `WBI1`, 0x431 = 1073 sections, flag 9 (compressed), **resume vector
  0x800523BC** (`cxd90014_cpu_resume_ssboot`), version 0x20060224.
* The snapshot is a fully booted system (Android, `zygote`, `im.elf` present); its RAM has state 4 (`warm`),
  `cxd90014_ssboot_stat` = 2, `pm_mem_alive` = 0, hand-off +0x54 = 0x800523CC, +0x60 = 0.
* Factory sequence from the code: fresh boot (stat 0) → `"warm"` off creates an image with entry `_profile` → stat 1
  profiling → `_optimize` → stat 2 → image with `_ssboot_pre` + `em_reboot` → stat 3 → `cxd90014_pm_finish` rewrites the
  header entry to `_ssboot` and prints `SSBI Create Completed!` → stat 4 in the field.
* The emulator never does a snapshot boot: the board's stage 1/2 boots `vmlinux.bin` + initrd (= boot mode COLD,
  stat 0). That is acceptable as the "DDR lost" path; snapshot boot is out of scope here.

## 3. Spec for the board

### 3.1 Off: keep the RAM image when the off is warm

In `a5k_poff_line` → `a5k_power_off` (RE_REBOOT B.7): the latched off data already carries `warm = off_info[+0x1B] & 1`.
Add `s->dram_retained = warm && sus_error == 0`, where `sus_error` is read from PA 0x80021060 at the p0.10 edge (the
micro cannot see it; it stands for the bootloader's later check, so reading it at power-on is equally valid). Cold or
`sus_error == 1` → unchanged (power-on clears DDR, cold boot). The `poff=restart` path (factor 13/20, cold) is
unchanged.

Save at the p0.10 edge (only when `dram_retained`): nothing else. DDR is the state; IRAM is lost with the SoC.

### 3.2 Power-on after a warm off ("HOT" resume)

Replace the cold power-on in `a5k_power_wakeup` when `dram_retained`:

1. `reset_kind = A5K_RESET_HOT`; `qemu_system_reset_request(...)`: every device resets (SoC power-on), CPUs reset, ROM
   blobs come back.
2. In `a5k_boot_reset`, kind HOT:
   * **do not clear DDR**; clear **IRAM** (SoC power loss) as for a power cycle; ROM blobs then restore the IRAM/SRAM
     bootloader data (IBT, init_param, NAND dev-info, ARM7 status) as on every power-on;
   * **do not write** kernel, initrd, kemco, the initrd-tail clear, or process the loader descriptor (the live kernel
     owns that memory: the initrd window was freed and reused);
   * clear the SMP mailbox (as today; the secondaries start in the ROM wait loop and are released by
     `cxd90014_smp_init`);
   * read `entry = *(u32 *)0x80021054`; if it is not inside DDR (or `sus_error != 0`) fall back to a cold power-on and
     log it; otherwise write it into the stub's entry literal (`A5K_STUB_ENTRY_LIT`). The stub still programs the GIC
     (ITARGETSR, ICFGR) before `ldr pc`; r0/r1/r2 are harmless.
   * log `sony-a5000: power-on, HOT resume (DDR kept) -> 0x800523cc`.
3. **Hand-off page vs ROM blob.** The `preload=` page at 0x80021000 is a ROM blob and `rom_reset` runs after
   `a5k_boot_reset`, so a reset would overwrite the live page. Before requesting the HOT reset, copy the live 4 KiB
   page into the ROM blob (`rom_ptr(0x80021000, 0x1000)`), keeping a pristine copy of the factory page; restore the
   pristine copy into the blob before any cold power-on. Optionally update the boot reason the bootloader would
   present (+0x4C stays 7; RAM Backup 0x51000F := 2 HOT, 0x510013 := POWER_SLIDE / key bit, RE_REBOOT B.5) [?: needs
   the RAM Backup image address; not required for the resume to work].
   Other ROM blobs inside DDR (ATAGs 0x80024000, stubs 0x80025000/0x80025200) lie in `memrsv 0x28000@0x80000000`, which
   the kernel does not use: restoring them is harmless.
4. DDR controller model (`cxd90014-ddrc`): after reset it must be in the normal (not self-refresh) state; the
   bootloader's self-refresh exit is implicit.
5. Persist across the HOT power-on, as for every power cycle: NAND, SD, USB cable, RTC/sub-micro clock, lever state.
   `off_info_valid` is consumed.

### 3.3 Required CPU fix: MMU enable from the XN identity map (blocks 3.2)

`resume_turn_on_mmu` (PA 0x80052480):

```
80052480  mcr p15,0,r2,c1,c0,0   SCTLR.M = 1      (no isb)
80052484  nop
80052488  mov pc, r1             -> 0x400524A0 (kernel VA)
```

After the `mcr` the CPU fetches 0x80052484 through the restored page tables. The identity section for DDR is
`0x80011452` in both `swapper_pg_dir` (L1 index 0x800 at 0x80036000) and the task's pgd (w1: 0x8AB52000): AP = 01,
domain 2 (client, DACR = 0x15), **XN = 1**. QEMU raises a prefetch permission fault there (w1: IFSR 0x2D, IFAR
0x80052484, PC 0x80052484) → the kernel's exception monitor ("vector exception (0x000) at 0x00000000", task im.elf).
The factory snapshot has the same entry (walk of the WBI page table: VA 0x80052484 → 0x80011452), and real cameras do
resume HOT, so the real Cortex-A5 executes the `nop` and the `mov pc` from instructions fetched before the SCTLR write
took effect (no ISB follows; architecturally the MMU state is only guaranteed after a context synchronisation).

Options for the code agent (a CPU-model change, not a firmware patch):
1. *Preferred:* model the A5 behaviour narrowly: a write to SCTLR that sets M without a following ISB takes effect for
   instruction fetch at the next context-synchronising event or taken branch (here `mov pc, r1`). In QEMU this means
   letting the TB that contains the `mcr` (and the next two instructions) run with the old `mmu_idx` for fetch, e.g. by
   not ending the TB on an SCTLR.M 0→1 write when it is followed by straight-line code, or by a CPU property
   `sctlr-m-delay=2` on the a5000 CPU. Data accesses are not affected in this sequence.
2. *Fallback (HLE, document per AI_WORKFLOW §1.5):* the board recognises a prefetch abort at exactly PA 0x80052484 with
   SCTLR.M just set and completes `mov pc, r1` itself. Fragile; only if option 1 is not feasible.

Check the normal boot path is unaffected: Sony head.S `__turn_mmu_on` already runs from an executable mapping (the
emulator boots today).

w3 confirms that this is the only CPU-side obstacle on the way into the kernel: with XN cleared for that one step the
CPU reaches `resume_after_mmu` (0x400524A0) and the kernel continues (below). The saved context at the off (w3,
`sleep_save_sp` = 0x8AB93E28): DACR 0x15, CPACR 0xC0F00000, TTBCR 0, TTBR0 0x8AA0006A (the task's pgd), TTBR1
0x8003406A (`swapper_pg_dir`), CONTEXTIDR 0x70, …, SCTLR 0x10C5387D.

#### Result of the in-place resume (w3) and what to check after the board work

Kernel log after `pc = 0x800523CC` (klog ring, w3):

```
[ 317.325419] Enabling non-boot CPUs ...
[ 317.332907] CPU1: Booted secondary processor      (x3; "Unknown IPI message 0x1": they left platform_cpu_die,
[ 317.343921] PM: early resume of devices complete    not the boot ROM, because w3 did not reset the CPUs)
[ 317.344220] UDIF: resume start: unified drivers      gpio, hwtimer, fusb_otg, dmac, ldec, lld, mumindrv, b-252,
                                                       ..., UPM, nfc_rsrc, grm_ma, hdmi, ..., liro
[ 317.420479] [LIRO]: liro resume complete           (av-cam entry_resume 0x43541078)
[ 317.423030] PM: resume of devices complete after 2.441 msecs
[ 317.454735] Restarting tasks ...
[ 317.467310] Unable to handle kernel NULL pointer dereference at virtual address 00000000
               PC 0x439B193E (av-cam, `movs r3,#0; strb r3,[r3]` = its fatal-error stub), task liro-kliro_99,
               from osal_uipc __default_cb <- __osal_snd_sync_msg <- av-cam 0x4355DA0B
```

`__default_cb` is the osal_uipc callback for a message to a queue without a registered handler: an av-cam task sent a
sync message right after the thaw to a receiver that is not (yet / any more) registered [?]. Candidate causes, to be
checked once the board does the real HOT power-on (devices and CPUs reset, sub-micro link restarted):
1. w3 artefact: the sub-micro model was left in its "powered off" state (power-off request acknowledged, off data
   latched) and the SIO/timer devices were not reset, so the micro never sends its power-on frames;
2. a real resume handshake the board's sub-micro model does not yet do (the micro reporting the wake source / "ON"
   in the first frames after a power-on) [?];
3. av-cam's own resume order. The receiver queue behind `0x4355DA0B` must be identified first (av-cam IDB) if the
   fatal persists after 1 and 2.

### 3.4 Interaction with the existing paths

| Off | latched off data | `sus_error` | power-on | boot |
|---|---|---|---|---|
| APO factor 8 (warm) | +0x1B bit0 = 1, +0x15 = 0x21 | 0 | lever ON / APO wake key | **HOT resume** |
| lever factor 7 (warm) | 1, 0x21 | 0 | lever ON | **HOT resume** (blocked earlier by the av-cam stall, RE_REBOOT B.5) |
| SWITCH_MODE 13, UI reset 20 (cold) | 0, 0x20 | 1 | micro restart | cold (stage 1 → updater / stage 2), DDR cleared |
| battery / heat (cold) | 0, 0x21 | 1 | lever | cold |
| suspend error (`cxd90014_pm_prepare`) | any | 1 | any | cold |
| HMP `system_reset`, SYSCTL 0x11, WDT | — | — | — | SoC reset as today (§4.1 of RE_REBOOT) |
| `-M poff=restart` override | | | | cold power cycle |

A machine option `warm-resume=on|off` (default `on`, stock behaviour) keeps the old always-cold behaviour available.

## 4. Acceptance tests

Common: fresh NAND clone, copied binary, `A5K_SHELL=<port>`, monitor on TCP.

1. **APO → lever → HOT resume.** Boot to the shooting screen (~3 min), `a5kshell run cat /proc/uptime` = U0. No input:
   at ~316 s uptime `sub-micro off data: factor 8, next 0x21 (off), warm`, p0.10 low, `sub-micro power off`; HMP
   `info status` = suspended. Wait 60 s. `sendkey o` (lever OFF), `sendkey o` (lever ON) → `power-on, HOT resume (DDR
   kept) -> 0x800523cc`, **no** `stage 1 (loader)` line. Within ~10 s emulated: `screendump` shows the shooting screen
   again; `cat /proc/uptime` ≥ 316 (it continues; a cold boot would show < 60) and `dmesg` shows the resume
   (`PM: resume of devices complete`, `Restarting tasks ... done`) with no `Linux version` banner. Repeat the APO /
   resume cycle twice in the same run.
2. **APO wake key.** As 1, but `sendkey s` instead of the lever → same HOT resume.
3. **Cold offs stay cold.** `pmca-console updatershell` (factor 13): `sus_error` = 1 at the p0.10 edge, power cycle with
   `DDR and IRAM cleared`, stage 1 → updater (RE_REBOOT §5 test 2). A cold power-on after a HOT resume uses the factory
   hand-off page again (+0x54 = 0x800523CC, +0x60 = 0, +0xE4 per `rtc-lost`).
4. **Fallback.** Poison +0x54 with gdb before power-on (e.g. 0xFFFFFFFF) → `HOT resume refused, cold boot` and a normal
   cold boot.
5. **No regression.** Cold boot to the shooting screen; `-M warm-resume=off` reproduces today's cold power-on after APO.

## 5. Open questions

1. The bootloader itself is not in the update package: the HOT decision rule (warm flag from the micro vs
   `sus_error` vs a DDR check) and the exact hand-off fields it rewrites on a HOT power-on are inferred [?].
2. Backup 0x510023 semantics: 0 = no warm handling, 1 = WBI re-creation on warm offs (`"warm"` state 4), 2 = this unit
   (`"mem"` only) [? from the two consumers' tests only].
3. Whether nand.ko / lld re-read the bootloader's IRAM/SRAM data (IBT, init_param, dev-info) on resume, i.e. whether the
   board must restore them before a HOT resume (it does when it restores all ROM blobs).
4. Whether the micro also cuts DDR after a long time off (battery drain, battery removal): then a HOT power-on must
   fall back to SS/COLD. The emulator never loses power while the VM runs.
5. Hand-off +0x50 (= 1) meaning.
6. The av-cam fatal after `Restarting tasks` in w3 (end of §3.3): artefact of the no-reset diagnostic or a missing
   sub-micro power-on handshake. Re-test after §3.2/§3.3 are implemented.

## 6. Reproduce

* gdb (no watchpoints): `hbreak *0xc00200c8` (IRAM p0.10 store; MMU off, VA = PA), then `x/1wx 0x80021054`,
  `0x80021060`, `0x803946bc`, `0x8037947c`, `0x800523a0`, `0x8005249c`. Manual resume test: `set $pc = 0x800523cc`,
  detach (devices not reset; shows the §3.3 fault). w3: additionally clear bit 4 (XN) of the L1 word at
  `(TTBR0 & ~0x3FFF) + 0x2000` (TTBR0 = word 3 of the context at `*(0x8005249C)`) and of `0x80036000`, `hbreak
  *0x400524a0`, restore bit 4 there; then `tools/klog.py <monitor port>`. Fault registers: `maint print raw-registers` (IFSR_S, IFAR_S,
  DACR_S, TTBR0_EL1_S, SCTLR_S).
* WBI header: first 0x30 bytes of `nflasha5`; identity entry: `tools/wbi.py` `walk(0x80052484)`.
* im.elf: objdump `-M force-thumb`, `0x9F44..0xA06C` (`sub_9F44`, `sub_9F68`, `sub_9FB4`), `0xA258..0xA286`.

## 7. Implementation notes (board + CPU, 2026-11-15)

* **Board** (`hw/arm/sony_a5000.c`): at the p0.10 drop with "stay off", `a5k_power_off` sets `dram_retained` when the
  latched off data is warm (+0x1B bit 0) and `sus_error` (PA 0x80021060) is 0 (`warm off, sus_error 0: DDR kept`). On
  power-on `a5k_power_wakeup` re-checks the hand-off page (resume vector inside DDR and word-aligned, `sus_error` 0;
  else `HOT resume refused (...), cold boot`), copies the live 4 KiB hand-off page into the `preload=` ROM blob
  (keeping the factory copy) and requests reset kind `A5K_RESET_HOT`. `a5k_boot_reset` then clears IRAM only, writes
  the resume vector into the stub's entry literal and skips kernel, initrd, kemco and the loader descriptor; the SMP
  mailbox is cleared as on every reset. Every other reset puts the factory page back into the blob first. The machine
  option `warm-resume=on|off` (default `on`) selects this; `off` keeps the old cold power-on.
* **Boot stack (H)**: `cxd90014_cpu_resume` calls `pl310_init` and `cxd90014_smp_init` (which push) before it restores
  the saved `sp`, so it needs a stack from the bootloader. With `sp = 0` after the CPU reset the first push faulted
  (data abort, DFAR 0xFFFFFFE4) and CPU0 ended in the zero-filled low vectors. The board sets CPU0's `sp` to 0xC0020000
  for the HOT entry (the IRAM stack the sleep code itself used at the off, §1.2) [? real bootloader value unknown].
* **CPU** (`target/arm`, upstream files): ARM CPU property `sctlr-fetch-delay` (default off, set by the board). With it,
  an AArch32 SCTLR write that turns M on (0 → 1 at run time) does not end the TB, so the instructions after the `mcr`
  up to the next branch are fetched as translated with the MMU off; hflags are rebuilt and data accesses see the MMU at
  once (TLB flush in `sctlr_write`). Every other SCTLR write still ends the TB. A first attempt that reused the XScale
  mechanism (`ARM_CP_SUPPRESS_TB_END` on every SCTLR write) made the normal cold boot fail at ~3.5 s (`kernel BUG at
  .../grm_ma/dmm_if.c:281`) in two of two runs, so the runtime M 0 → 1 check is required, not just tidier.
* **Result** (stock console, binary built from branch `warm`): idle power save at ~317 s → `warm off, sus_error 0` →
  lever OFF/ON → `power-on, HOT resume (DDR kept) -> 0x800523cc`, no `stage 1 (loader)`; klog: `Enabling non-boot
  CPUs` … `CPU1..3 is up`, `[LIRO]: liro resume complete`, `PM: resume of devices complete`, `Restarting tasks ...
  done.`, no `Linux version`. The shooting screen is back within seconds and MENU opens (screendump identical to the
  menu of a cold-booted run). A second APO in the same run resumed the same way with the APO wake key `S`. **No av-cam
  fatal** (the w3 fatal of §3.3 was an artefact of resuming without a device/CPU reset). `jiffies_64` (PA 0x80360240)
  continues across both resumes (0xFFFFEB23 at the off → 0xFFFFF0F3 15 s after the resume → 0x1_00006769 at the second
  off). printk timestamps restart at 0 (sched_clock follows the reset timer) [? same on the real camera].
* **Known issue, debug shell only**: with `A5K_SHELL` (`console=ttyAM1 amba2.console=1`) the resume deadlocks CPU0 in
  the UART2 driver: the Sony PL011 interrupt thread prints `amba2:%d:TXRIS` (`pl011_int+0x98`) while it holds the port
  lock, the console write on the same port takes that lock again (`rt_spin_lock_slowlock` → `__bug`), the exception
  monitor then spins in `em_dump_write` → `pl011_console_write`. The UI keeps running on CPU1..3, but IRQs routed to
  CPU0 (UART2 152, so the shell) are dead. The stock console (`ttynull`) is not affected. Not investigated further
  (QEMU PL011 TX raw interrupt state after reset vs the real UART [?]).

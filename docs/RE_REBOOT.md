# Reboot paths: USB SWITCH_MODE restart and warm reset after stage 2

Scope: (1) how the unmodified firmware 1.10 restarts the camera after `pmca-console firmware|updatershell` sends
SWITCH_MODE, and why the emulator never resets; (2) why an HMP `system_reset` from stage 2 makes the stage-1 loader
kernel panic; (3) what a real reboot or power cycle does, and what the board must rebuild on a reset. The output is a
spec for the board code (`hw/arm/sony_a5000.c` and friends). No emulator code was changed for this document.

Related: `docs/BOOT.md`, `docs/RE_UPDATER_BOOT.md`, `docs/RE_USB_HOST_PATH.md` (open question 6),
`docs/RE_SUBMICRO_LINK.md`, `docs/RE_INPUT_I2C_POWER.md` §11 (upm.ko), `docs/HARDWARE.md`.
`[?]` = not proven.

Evidence:
* Static: `re/vmlinux.elf` (objdump), `re/ida/kmod_i2cgrp/upm.c`, `re/ida/upd/libupdaterufp.so.c`, `re/ida/im.elf.c`,
  `re/ida/libObj.so.c`, `re/ida/libIMDB.imdb_raw.txt`, `/usr/bin/up.sh`, `/bin/ud_send_lsi.elf` (objdump), av-cam.bin
  (IDA 9.4 + Hex-Rays on windev, `C:\a5000\emu\reboot\av.i64`, a copy of `lv2\pristine.i64`; raw objdump, base
  0x43541000).
* Dynamic (HEAD binary, pristine NAND clone per run, no SD):
  * r1: `A5K_DEBUG_CONSOLE=1`, stage 2 up, HMP `system_reset` (panic captured on serial), then a second
    `system_reset` with the initrd window tail zeroed through gdb (boots).
  * r2-r5: stock console, `-M usb-socket`, `pmca-console updatershell -d socket`, OK pressed with `sendkey ret 600`,
    `a5ktrace` probes on kernel restart/suspend functions, the IRAM sleep code, im.elf and libObj (CameraBackground),
    plus gdb reads of upm.ko and av-cam data.
  * r6: stock console; HMP `system_reset` at the shooting screen with the initrd tail zeroed: UI comes back.

---------------------------------------------------------------------------------------------------------------------

## 0. Summary

| Problem | Root cause |
|---|---|
| 1. No reset after SWITCH_MODE | The stock firmware does **not** use `reboot()`/`machine_restart`/SYSCTL +0x44 here. It runs the normal **power-off sequence with a "restart" request to the sub-micro**: upm off factor {13 = OFF_BY_UPDATE, 28 = cold, next_boot 32} → im.elf OFF → suspend-to-RAM → IRAM sleep code puts DDR in deep power-down and drops the power-off line **GPIO p0.10** → the sub-micro cuts power and powers the camera back on. In the emulator the chain stops at the **first** of three gaps: (a) im.elf waits forever for `CameraBackground_inact` (libObj sends av-cam command 0x300E and av-cam never replies); after that, (b) the IRAM sleep code would spin on the unmodelled DDR controller at 0xF0104000, and (c) nothing models the sub-micro power cycle on the p0.10 request. |
| 2. Panic after a warm `system_reset` | The stage-1 kernel uses `initrd=0x80428000,8M` (kemco cmdline) and unpacks the **whole 8 MiB** as initramfs. The board writes only the 3.97 MB `loader.cpio`; the rest of the window still holds stage-2 data (the stock ext2 initrd tail, freed pages, e.g. `f7f7f7f7`). `unpack_to_rootfs` → "junk in compressed archive" → treated as an initrd → `RAMDISK: Couldn't find valid RAM disk image` → `mount_block_root` panic. Zeroing `[0x80428000 + loader_len, 0x80C28000)` before entry fixes it (verified in r1 and r6). |
| 3. Real semantics | Power cycle by the sub-micro, not a SoC soft reset. `/setting/updater/mode` is on NAND (written by `up.sh user` + `sync`), so it survives. DDR is put in deep power-down (`ddr_ctrl=dpd,dpd`), so RAM contents are lost and the bootloader does a full cold boot. `ud_send_lsi.elf` does nothing on this unit (Backup 0x004B0000 = 0), so the updater decision is the FAT flag alone. |

---------------------------------------------------------------------------------------------------------------------

## 1. How the stock firmware restarts after SWITCH_MODE

### 1.1 The request (`libupdaterufp.so` `sub_17C0`)

On SWITCH_MODE (`*arg == 3`) it runs `system("up.sh user")` (or `up.sh ver`), then sends one osal message
`{0x920141, 13, 28, 32}` (`re/ida/upd/libupdaterufp.so.c` line 993). 0x920141 is upm.ko's **ADD_OFF_FACTOR**
(`k_upm_off_factor_handler_*` case 9568577 → `add_off_factor`). The three words are:

| word | value | meaning | evidence |
|---|---|---|---|
| factor | 13 | `OFF_BY_UPDATE` | libObj `testSetCautionOffFactor` name table (`re/ida/libObj.so.c` ~1213060) |
| type | 28 | cold off. 27 = warm/WBI off. `add_off_factor` forces anything else to 27 | upm `add_off_factor`; libObj `InfraInput::RebootCold` sends (7, 28, next_boot), `RebootHot` sends (26, 27, next_boot), `ExeColdOff` sends (f, 28, 33) |
| next_boot | 32 | "restart after off". 33 = stay off | `k_upm_snd_hcrn_ura`, below |

So the "reset" is `RebootCold` with factor OFF_BY_UPDATE. Nothing on this path calls `reboot(2)`: in r2-r5 the probes
on `sys_reboot`, `kernel_restart`, `machine_restart`, `cxd90014_reset` (the SYSCTL +0x44 = 0x11 writer),
`emergency_restart`, `kernel_power_off` and `wdt_intr` fired **only** for stage 1's own `reboot -f`.

### 1.2 upm.ko off sequence

* Type 28 → `k_upm_set_sus_error()` writes 1 to the `pm.sus_error` word of the hand-off page (cmdline
  `pm.sus_error=0x80021060`; the module writes `*(dword_14D94 - 0x40000000)`) [the module variable holding that
  address is inferred from the cmdline, not traced].
* Off states (`g_upm`): 81 active → 17 inacting_app → 19 inacting_app2 → **18 inacting_im** → 33 inactive → 34 sus →
  65 res. Read live in r2 with gdb (upm.ko core at 0x3F31A000, `.data` = `g_upm` at 0x3F32E5CC): **`g_upm = 0x12` (18)**,
  i.e. upm sent OFF (0x920143) to im.elf (queue 0x920169) and waits for OFF_CMP (0x920144, arg 1).
* On I_INV_CMP (`k_upm_snd_hcrn_ura`) upm already told the sub-micro side what to do after the off:
  * type 28 and factor ≠ 39 → writes `"dpd,dpd"` to `/sys/module/pm/parameters/ddr_ctrl` (DDR deep power-down on both
    channels = RAM contents lost);
  * `k_upm_snd_front_enddata(apo, warm, restart, kind, factor)` to av-cam queue **0x28023B**. next_boot 32..38 →
    `restart = 1`, kind 0..5; anything else (33 = plain off) → `restart = 0`.
* av-cam handler `sub_43AC7002` (registered by `sub_43910B28`) copies the end data into the task-48 state block
  `dword_43ED44B4` and marks slot-A item 22 dirty (`sub_4391037C(22)`). Read live in r5 after the OFF (gdb, VA
  0x43ED44B4+128): **+135 = 0x01** (bit0 restart = 1, bits1-3 kind = 0), **+136 = 0x01** (cold), +137 = 0x01,
  +139 = 0 (not APO), +140 = 0xC0, **+162 = 0x0D** (factor 13). The serializer at `0x4391238C` copies these into the
  task-48 TX packet: packet +203 bit0 = restart, bits1-3 = kind; +204 = cold/warm; +205..206 = +137..138;
  +210 = +169; +277 = factor. This is how the **sub-micro learns "power back on after this off"**.

### 1.3 im.elf OFF: where the emulator stops (gap a)

im.elf case 0x920143 (`re/ida/im.elf.c` ~1674): `dword_153DC` = 1 (cold), then `sub_AFBC` ("inact_all") calls the
`inact=` symbol of every IMDB entry of type 0x20000003 with that argument and counts them (`dword_154D8`). Each
library must answer 0x940122 (inact_cmp, u16 id) before im.elf sends OFF_CMP to upm. r3/r5 (probes on im.elf
`0xB10A` = the `dlsym` call, `0xADAC` = an inact_cmp arriving):

| IMDB id | library / symbol | inact_cmp |
|---|---|---|
| 0x0F8 | libInfraMediaPwrMgr.so `InfraMediaPwrMgr_inact` | yes |
| 0x155 | libSyncAndroid.so `SyncAndroid_inact` (answers at once) | yes |
| **0x102** | libObj.so `CameraBackground_inact` | **never** |

`CameraBackground_inact` (libObj `0x17AA94`) posts code 64 to its own task queue (0x0102021F). Its state machine
(`sub_17A760`, state 4 at that time) goes to state 2 and calls `sub_17A0E8(0x300E)`: a message to av-cam queue
**0x030001** with command 0x300E, then `osal_rcv_msg_tmo(..., -1)` for the reply; only a reply with +8 = 0xF000 and
+2 = 0x300E produces the inact_cmp. r5 trace (libObj base 0x05938000):

```
cb_inact  (CameraBackground_inact)       -> cb_post 0x40 -> cb_sm {18,..,0x40}
cb_send   r1=0x300e                      (sub_17A0E8)
av_post   r0=0x300e [liro-kliro_69]      (av-cam 0x4375A0D4: av-cam received it)
(no cb_rcvret afterwards; boot-time 0x3007 and 0x3006 did get replies)
```

In av-cam the 0x030001 task (`sub_4375CFB8`) passes type-0 messages to `sub_437D9E68` ("CapconExec"), which maps
0x300E → capture-control request **8** on queue 0x0302D9; the reply comes when capcon finishes that request. The LR
bridge branch is off on this unit (object +1348 = Backup 0x000300A4 = 0). **Why capcon never completes request 8 in
the emulator is not resolved** (open question 1). It is an engine-side gap, not a board-reset gap. The screen goes
black because the display was already switched off by the earlier inact steps.

### 1.4 After im.elf OFF_CMP (static only; not reached in the emulator)

1. upm → 33 → 34; im.elf case 0x92014F (from upm in state 34) → `sub_9FB4(0)` writes **`"mem"` to `/sys/power/state`** (cold off: not
   `"warm"`).
2. Kernel: `enter_state` → `cxd90014_pm_begin` → `cxd90014_pm_prepare` (`udif_driver_suspend`; on failure
   `wbi_drop` + sus_error = 1) → `cxd90014_pm_enter` (0x40052220) → sleep entry 0x4037BEA0: saves the CPU context,
   `cxd90014_create_warmbootimage`, copies 0x4037BFA0..0x4037C174 to **IRAM 0xC0020000**, disables caches,
   `pl310_shutdown`, jumps there.
3. IRAM code (offsets from 0xC0020000; source 0x4037BFA0):
   * +0x00 MMU off.
   * DDR controller **0xF0104000** (literal at 0x4037C160). For each channel whose `ddr_ctrl` byte (0xC00200F4 /
     0xC00200F8) is non-zero: write it to +0xC4 (ch0) / +0xD8 (ch1), +0x12C = 0x1000, +0xAC = 0x11008000 then
     0x11008002 (ch1: 0x11018000 / 0x11018002), then **poll +0x128 bit12** (+0x3C / +0x74).
   * +0x78: bits[31:24] := 0x0A, then **poll (+0x7C & 0xFF) == 0x25** (+0x94).
   * If the byte at 0xC00200FC is set, print `"E\r\n"` on UART0 0xF2000000.
   * **+0xC8: `*(0xF2910048 + 0x100*port) = 1 << pin`** from `cfg.poff=0,10` (`param_set_port`: byte0 = port,
     byte1 = pin; `cxd90014_pm_setup` builds the address/value pair at 0x4037C08C/90) → **GPIO p0 output-clear,
     bit 10**.
   * +0xDC: `wfi` loop forever.
4. The sub-micro removes power. Because restart = 1 it powers the SoC up again; the Sony bootloader runs from reset
   with DDR contents lost (dpd), finds `/setting/updater/mode`, and boots the updater core (RE_UPDATER_BOOT).

Gaps (b) and (c): `0xF0104000` falls into the `io-f0` register file (reads return the last write, so +0x128 and +0x7C
read 0 → both polls spin forever), and no board code reacts to p0.10. In the emulator p0 is `dir = 0x40, out = 0x01`
at the shooting screen (HMP `xp`, r5), so p0.10 is not even configured as an output by Linux: on the real camera
it is presumably left driven high by the bootloader [?].

---------------------------------------------------------------------------------------------------------------------

## 2. Warm reset from stage 2: the stage-1 panic

Observed (r1, debug console):

```
[    0.658357] Trying to unpack rootfs image as initramfs...
[    0.675659] rootfs image is not initramfs (junk in compressed archive); looks like an initrd
[    0.831352] RAMDISK: Couldn't find valid RAM disk image starting at 0.
[    0.862493] Kernel panic - not syncing: VFS: Unable to mount root fs on unknown-block(1,0)
  ... (mount_block_root+0x23c) from mount_root from prepare_namespace
```

Cause: the stage-1 kernel gets the stock kemco cmdline, `initrd=0x80428000,8M`. Sony head.S ignores ATAGs, so the
`ATAG_INITRD2` length the board writes is never seen. `populate_rootfs` runs `unpack_to_rootfs` over all 8 MiB. The
`newc` cpio (`070701`, 3 966 976 B) ends at 0x807F0800. After the TRAILER the parser accepts only zero padding or a
compressed-stream magic. On a cold start the rest of DDR is zero. After stage 2 the window holds the tail of the
stock ext2 `initrd.img` (4 076 544 B) and pages the stage-2 kernel freed ("Freeing initrd memory: 8192K");
gdb read `0x807F0800 = 0xF7F7F7F7` (r1) and `0x000A0022 ...` (r6).

Verification: `stop`, `system_reset`, gdb `restore zeros.bin binary 0x807F0800` (4 421 632 zero bytes, up to
0x80C28000), `cont`:
* r1 (debug console): stage 1 → "a5k-loader: entering stage 2" → stage 2 → `input: D keyboard`.
* r6 (stock console, from the shooting screen): stage 1 → stage 2 → shooting screen again (screendump).

Nothing else had to be cleared for the warm reset. ROM blobs (boot stubs, NAND dev-info/status at SRAM+0x3F00/0x3F40,
bad-block list 0xC0000C00, init_param 0xC0001500, the hand-off page 0x80021000 from `preload=`) are restored by
`rom_reset`. `a5k_boot_reset` clears the SMP mailbox and rewrites kernel, initrd and kemco. Every board device has a
reset handler (nand ARM7 HLE, usbhdc, timers, sysctl, gpio, regfile, ...). The loader descriptor at 0x80027000 is
zeroed after use. The MUMIN area at 0x80000000 is rebuilt by stage 1 (`nand.ko boot_mode=1`).

Real camera comparison: the real bootloader loads the stock **ext2** initrd into the same window. An ext2 initrd is
parsed by the superblock size, so trailing junk is harmless and a warm reboot works without clearing. The emulator's
stage 1 uses a cpio initramfs, so clearing the tail is part of the "board stands in for the bootloader" duty.

---------------------------------------------------------------------------------------------------------------------

## 3. Real-camera semantics for this path

* **Kind of restart:** a power cycle by the sub-micro (§1.2, §1.4), not a SoC soft reset. The SYSCTL +0x44 = 0x11
  reset exists (`cxd90014_reset(1)` from `machine_restart`) but this path does not use it. The only user in the
  emulator is stage 1's `reboot -f`.
* **The updater flag survives:** `up.sh user` deletes `mode*`, `touch`es `/setting/updater/mode`, then `sync`. The flag
  is on NAND (nflasha2 FAT), and a power cycle does not touch NAND. Already shown by RE_USB_HOST_PATH: a cold boot
  from the same NAND clone enters the updater.
* **`ud_send_lsi.elf <mode_num>`** (called by `up.sh`) reads Backup **0x004B0000** (1 byte) and only messages the
  other LSI (queue 0x804B0376, reply 0x004B0379) if the value is 1 or 2. This unit has **0** (`backup_props_all.tsv`),
  so on the a5000 it exits at once. This closes RE_UPDATER_BOOT open issue 1 for this unit: the FAT file is the
  only updater-mode signal.
* **RAM:** `ddr_ctrl=dpd,dpd` → DDR deep power-down → contents lost. The `pm.sus_error` = 1 written into the hand-off
  page therefore matters only if the DDR kept its contents (the warm/WBI path) [?].
* **Hand-off page / boot reason:** the bootloader rebuilds 0x80021000 on every boot. +0x4C (u16 boot factor, read by
  upm via Backup 0xFA0029; snapshot value 7) is presumably derived from the sub-micro's power-on reason [?]. The
  updater core does not need it (built-in cmdline; RE_UPDATER_BOOT). The emulator presents the snapshot page on every
  reset (ROM blob). This is acceptable until a consumer that cares about the restart reason is found.
* **USB:** the gadget detaches during the off (observed). After the power cycle the updater's `sauu` attaches again as
  054C:03E2. The cable (VBUS) is external and stays.
* **Updater kernel caveat:** its built-in cmdline has `cfg.poff=1,1` (GPIO p1.1), the normal kernel `cfg.poff=0,10`.
  The physical power-off line cannot be both [?]. Check this when the updater's own restart is modelled (after
  WRITE_FIRM).

---------------------------------------------------------------------------------------------------------------------

## 4. Spec for the board

### 4.1 Reset kinds

Two board-level events, both ending in `qemu_system_reset_request()` (so all devices and ROM blobs reset as today):

| Event | Trigger | Extra work in the reset handler |
|---|---|---|
| **SoC reset** | SYSCTL +0x44 bits 0x11 (existing), WDT bit31 (existing) | DDR keeps its contents (no clearing beyond §4.2) |
| **Power cycle** | sub-micro model after the power-off line drops with restart = 1 (§4.4); HMP `system_reset` may also be mapped here (option) | additionally zero **all DDR (512 MiB) and IRAM (16 MiB)** before the ROM blobs are restored, to model the power loss / `dpd` [the real content is undefined, zero is the safe stand-in]. Then cold boot |

Keep a `reset_kind` field set by the trigger (default SoC reset), consumed and cleared by `a5k_boot_reset`. The zeroing
must happen **before** `rom_reset` restores the blobs: zero the memory in the trigger path, just before calling
`qemu_system_reset_request`, or make `a5k_boot_reset` zero and then re-apply the hand-off/devinfo blobs. The reset
handler order (registered before `rom_reset`, RE_UPDATER_BOOT §3) means the first option is simpler.

### 4.2 `a5k_boot_reset`, stage-1 branch (fixes problem 2)

When the stage-1 loader (or any `-initrd`) is written to `A5K_INITRD_ADDR`:

```
write s->initrd (initrd_len bytes) at 0x80428000
zero  [0x80428000 + initrd_len, 0x80428000 + 8 MiB)      /* cmdline initrd=0x80428000,8M */
```

Comment: the stage-1 kernel unpacks the whole cmdline window as initramfs. The real bootloader loads an ext2 initrd,
which tolerates junk, so this clearing is part of the stage-1 stand-in. Cost: one 4.2 MB memset per reset.
Do not clear in the stage-2 branch: the stock ext2 initrd behaves as on the real camera.

Optional hardening (same handler, cheap): zero the staging area header `0x90000000..+16` and keep the existing
descriptor wipe, so that a reset during stage 1 cannot replay a half-written descriptor. Not needed by any observed
failure.

### 4.3 DDR controller at 0xF0104000 (minimum the IRAM sleep code needs) — gap (b)

A small device (or `autoclear`/`init` entries in a dedicated regfile, priority above `io-f0`) with:

| Offset | Behaviour |
|---|---|
| +0xAC (W) | command. A write with bit1 set (0x11008002 / 0x11018002) completes at once: set **+0x128 bit12** |
| +0x12C (W) | 0x1000 clears +0x128 bit12 (W1C) [?: written before the command, so clearing on write is consistent] |
| +0x128 (R) | status, bit12 = command done |
| +0x78 (RW) | bits[31:24] = requested low-power state |
| +0x7C (R) | low byte = 0x25 when +0x78[31:24] == 0x0A (self-refresh / power-down entered), else the current state [?] |
| +0xC4, +0xD8 | storage (`ddr_ctrl` mode per channel) |

All values come from the IRAM code (§1.4 step 3); their names are guesses [?]. With this model the IRAM code reaches
the p0.10 write and `wfi`. DDR contents are not changed here (the power cycle in §4.1 handles loss).

### 4.4 Power-off line and sub-micro power cycle — gap (c)

* **Line:** GPIO **p0.10**, driven low by the write `0xF2910048 = 0x400` (p0 output-clear) from the IRAM code. The
  current GPIO model raises its `out` qdev line only on a latch change, and p0.10 is 0 in the latch. Two options:
  1. *Preferred, bootloader stand-in:* at reset the board sets p0 dir bit10 = 1 and out bit10 = 1 ("power hold"
     left by the bootloader [?]), then listens on GPIO out line `0*32+10` for a 1 → 0 edge. Check first that no
     firmware writes p0 +0x40 (whole latch) or clears bit 10 in normal operation: log it once under `-d unimp`.
  2. *Fallback:* the GPIO model exports a "poff" event for any write to p0 +0x48 with bit10 set.
* **Restart decision:** the sub-micro model keeps the last restart/kind/factor it received from the CPU and decides on
  the poff edge:
  * restart = 1 → schedule a **power cycle** (§4.1) after a short delay (e.g. 100 ms virtual: lets the `wfi` settle).
    Log `sony-a5000: sub-micro power cycle (factor N)`;
  * restart = 0 → **power off**: stop the VM (`qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN)`) or idle
    until a power-key model exists. Report it the same way.
* **Where the restart bit comes from:** the task-48 slot-A TX packet that `a5k_submicro_xfer` already receives. Known
  so far: in av-cam's packet layout restart = **+203 bit0**, kind = +203 bits1-3, cold/warm = +204, factor = +277
  (§1.2). The SIO frame offsets are **not yet mapped** [?]. Diffing the slot-A TX frame at VA 0x4338201C
  (`*(0x43E0C9A4)` = PA 0x8338201C) before (r6) and after the OFF (r5) gives these changed bytes:

  | frame byte | before | after OFF_BY_UPDATE |
  |---|---|---|
  | 0x14 | 00 | **0D** |
  | 0x15 | 1F | 20 |
  | 0x1F | 00 | 80 |
  | 0x21 | 00 | 80 |
  | 0x22 | 00 | **0D** |
  | 0x24 | 00 | **01** |
  | 0x25 | 00 | 81 |
  | 0x31 | 02 | 80 |

  Best guess [?]: 0x22 = factor, 0x24 bit0 = restart, 0x25 = cold flag | 0x80. The code agent should confirm by
  logging slot-A TX in `a5k_submicro_xfer` for one `RebootCold`-type off (factor 13) and one plain off (`ExeColdOff`,
  next_boot 33 → restart 0), and fix the offsets in the model's comment.
* **Interim** until the mapping is confirmed: a machine option `poff=restart|off`, default `restart`, with a
  `LOG_UNIMP` note. No other software-initiated off is reachable in the emulator today, because every off passes
  through the same stalled im.elf inact phase.

### 4.5 What persists across both reset kinds

| Kept | Why |
|---|---|
| NAND backing file (incl. `/setting/updater/mode`) | flash |
| SD card image | removable media |
| USB cable state (A5KU client connected → VBUS) | external; already kept by `cxd90014-usbhdc` |
| RTC / sub-micro clock state, `rtc-lost` option | lives in the sub-micro (battery-backed) |
| board options (`provision=`, `preload=`, `loader=`, …) | host configuration |

Reset/rebuilt: all device state (qdev reset), ROM blobs (hand-off page, NAND dev-info, IBT, init_param, stubs), SMP
mailbox, kernel/initrd/kemco, initrd window tail (§4.2), the sub-micro link state machine and timers (already in
`a5k_boot_reset`), and on a power cycle also all DDR/IRAM.

### 4.6 Not a board task, but blocking problem 1

Gap (a): av-cam must answer command 0x300E (capcon request 8). Until it does, the board work in §4.3/§4.4 is never
exercised by the SWITCH_MODE flow. Investigate capcon request 8 on queue 0x0302D9 in av-cam (what it waits for:
mechanical shutter, sensor power-down, lens, CAM-side sub-micro handshake) [?]. A temporary workaround (HLE reply)
would violate AI_WORKFLOW §1.2/§1.5 and is not proposed.

---------------------------------------------------------------------------------------------------------------------

## 5. Acceptance tests

Common: fresh NAND clone, binary copied, `-M usb-socket=127.0.0.1:<p>` where USB is used, monitor on TCP.

1. **Warm reset from the shooting screen.** Boot to the shooting screen (about 3 min) → HMP `system_reset` →
   expect serial/qemu `stage 1 (loader)` then `stage 2 — kernel ...`, and after about 3 min a `screendump` showing the
   shooting screen. With `A5K_DEBUG_CONSOLE=1`: no `Kernel panic`, `a5k-loader: entering stage 2`, then
   `input: D keyboard`. Repeat twice in a row (reset → reset) to cover a reset from a freshly reset system.
2. **Updater shell without a manual cold boot** (needs §4.6 resolved):
   `pmca-console updatershell -d socket` (patched PMCA, `A5K_USB=...`) → camera shows "The device will be reset" →
   `sendkey ret 600` → within a few minutes emulated, with no HMP command:
   * `a5ktrace` probes `pc=0x40052220` (cxd90014_pm_enter), `pc=0xC00200C8` (poff write), `pc=0xC00200DC` (wfi
     loop) each hit once on CPU0;
   * the board logs `sub-micro power cycle (factor 13)`, then `stage 1 (loader)` and the updater descriptor branch
     (`stage 2 — kernel ... @0x81528000`);
   * PMCA finds "Sony DSC ... in updater mode" (054C:03E2) in the same run, or in the documented second run after its
     30 s window, and the updater shell prompt answers `help`.
3. **Intermediate check for gap (a)** (regression test for the engine fix): after OK, im.elf probe `0xADAC` must log
   the ids 0x155, 0xF8 **and 0x102**, and upm `g_upm` (upm core base + 0x145CC, read with gdb) leaves 18.
4. **Plain power-off stays off** (after the restart-bit mapping): an off with next_boot 33 (e.g. trigger
   `ExeColdOff` or APO when reachable) → board logs power off, no reboot loop.
5. **No regression:** cold boot to the shooting screen; `pmca-console info -d socket` prints the model; stage-1's own
   `reboot -f` still goes through SYSCTL 0x11 → stage 2.

---------------------------------------------------------------------------------------------------------------------

## 6. Open questions

1. **Why av-cam's capcon never completes request 8 (0x300E)** in the emulator. This is the first blocker for the
   SWITCH_MODE restart. Check what capcon request 8 drives in hardware or the sub-micro.
2. **SIO frame offsets** of restart/kind/factor in the slot-A TX packet (candidates in §4.4).
3. **p0.10 at power-on**: is it really left high (power hold) by the bootloader, and is it an active-low "power off
   request" or a "power hold" that the micro watches? Is the sub-micro's power cut immediate or after a timeout?
4. **DDR controller register meanings** at 0xF0104000 (+0x78/+0x7C/+0xAC/+0x128/+0x12C/+0xC4/+0xD8). Only the values
   used by the sleep code are known.
5. **Hand-off +0x4C boot factor** after a sub-micro restart (restart-after-update vs power key). Not needed by the
   updater core.
6. **`cfg.poff=1,1` in the updater kernel** vs `0,10` in the normal kernel (§3). Relevant for the updater's own reboot
   after WRITE_FIRM / COMPLETE (the busy loop in RE_USB_HOST_PATH §9 is a separate issue).
7. Whether the real bootloader also clears or ignores `pm.sus_error` on a cold power-on (no effect on the emulator
   while the hand-off page is a ROM blob).

---------------------------------------------------------------------------------------------------------------------

## Capture-control stop (0x300E) gate

Closes open question 1 (gap (a) of §0/§1.3). Static RE: Hex-Rays + disassembly on a private IDB copy
(`C:\a5000\emu\poff\po.i64`, from `reboot\pristine.i64`). Hex-Rays drops most globals in the link module
(`MEMORY[x]`), so every claim about it was checked in the disassembly. Dynamic RE: HEAD binary (null SA, Sgc1 latch,
SYSV master V), pristine NAND clone per run, no SD, `pmca-console updatershell -d socket` + `sendkey ret 400`,
`a5ktrace` `fn=` probes reloaded at the OK press, HMP `xp`/`pmemsave`, and one gdb run that only **wrote three bytes**.
Runs (scratch `re-poff/r1..r3`): **p1, p2** stock (both stall); **p3** with `A5K_SMFRAMES` = `data/submicro_frames.bin`
with TLV 3 data[1] bit 6 set again (the snapshot value), then the gdb write. `[?]` = not proven.

### A.0 Result

| Question | Answer |
|---|---|
| What capcon request 8 waits for | Nothing in av-cam's own hardware. 0x300E drives the **mode sequencer** (path 1 → 0x20). Its last module request is **module 0xD = 0x0D20** ("cold power-off" mode of the sub-micro link). Module 0xD answers only when the **link driver commits mode 0x0D20**, and it commits only after a **handshake with the sub-micro**. |
| The missing event | The **sub-micro**: (1) must acknowledge the CPU's power-off request by setting **slot-B RX TLV 0x03 data[1] bit 6** while the CPU's **slot-B TX TLV 0x0A data[1] bit 6** is set, and (2) must answer two **slot-A memory reads** (TX packet type **0x41** → RX packet type **0x40**, 256 bytes each from micro addresses **0x20000500** and **0x00030100**). The emulator's sub-micro replays a fixed frame, with the bit 6 that the snapshot had **cleared by `mkdata.py`**, and never sends a 0x40 packet. |
| Proof | p1/p2: sequencer stuck at index 0xF, module 0xD request 0x0D20, response 0x0D00. p3 (bit 6 restored): task 48 starts the read of 0x20000500 and loops on "restart" (no 0x40 reply). p3 + gdb `byte 0x43E62C13 = 1, 0x43E62C16 = 1, 0x43E62C73 = 1` (= "both reads done"): link commits 0x0D20 → `CapconReply(0x300E)` → CameraBackground reply → im.elf inact_cmp **0x102** → `"mem"` → `cxd90014_pm_enter` → IRAM sleep code, which then spins on the DDR controller (gap (b)). |
| Same request on a plain power-off? | Yes (static): CameraBackground sends 0x300E on every im.elf OFF, and every av-cam path into state 0x20 requests module 0xD = 0x0D20 (§A.6). |
| Class | Missing **sub-micro behaviour** (board HLE of the micro, `hw/arm/sony_a5000.c` `a5k_submicro_*`), not an av-cam IRQ, DMA or register. |

### A.1 0x300E → capcon request 8 → mode sequencer (av-cam)

* `CapconExec` `437D9E68` (task 69, from queue 0x030001): 0x300E → `{0, 8, seq, replyq}` to queue **0x302D9**.
  `tsk_capcon_sequence` `437DA408` (task 97) dispatches it to `CAM_SyncRequest_Wrapper` `4375C920(…, 0x300E)` →
  path selector `437D8560`. The reply is `CapconReply` `4375A338` (0x300E → 776), sent by task 68 (`43A5C48A`) when
  HS_Main has finished the path (`dword_43DC70E0 = 0x300E`). This is the same mechanism as 0x3007 (RE_PLAYBACK_SWITCH
  §2.1).
* Path table `0x43EC6420` (`{state, event, fn, event}`): 0x300E from every state ends in state **0x20**
  (handlers `437D9201` state 1 → `{0x20}`, `437D9099` state 2 → `{0x20}` or `{0x43,2,0x20}`, …).
* Observed (p1–p3, trace `PATH_setup`): when 0x300E arrives after the USB flow, the selector is in **state 1**
  (USB connect had run 0x3007 2 → 6 and 0x3006 6 → 2 before). Transition 1 → 0x20 = step array **`0x43B7111C`**:
  ```
  101 201 301 401 521 601 701 960 A00 B01 C06 D00   wait 0x0810 (modules 5, C)   522 D20   wait 0x1010 (modules 5, D)   end
  ```
* Stuck state (p1, p2; HMP): sequencer `0x43DC7104` = `{table 0x43DC71A0, 250, 13, busy 1, steps 0x43B7111C, 17,
  index 0x0F}`; requests `0x43DC7120[12]` = **0x0D20**, responses `0x43DC7160[12]` = **0x0D00**; every other module
  matched. Camera-state copy `0x43D86D2E` (= `word_43D86D18 + 22`) = 0x0D20, so VD_Pre did hand the request out.

### A.2 Module 0xD = the sub-micro link "power mode"

* Response of module 0xD = `word_43D887AA` (HS_Main `4376B28E..4376B294`, `437D86F4(12, …)`).
* It is written by VD_Pre `4376E172` → **`4376A54C(camstate, &word_43D887AA)`**. With the gate pointer
  `off_43D84BB4 = &byte_43D68AC7` (set by the link at its first slot-B frame) and `byte_43D68AC7 == 0` (p2: both
  true), it posts `{camstate, &resp}` to the link mailbox **0x1504CF**, handler **`4369D8BC`** (runs in task 66 every
  frame, p2: `M12_task` 20/20).
* `4369D8BC` (disassembly `4369D8BC..4369D952`): the request `camstate+22` is written into the link's mode double
  buffer `0x43D68B3C` **only while `byte_43EDA39D != 0`**. That byte is 1 in the image (`av-cam.bin` VA 0x43EDA39D) and
  the handler clears it: it latches once, at boot. Afterwards the handler only copies `camstate` into the request
  record `dword_43D68B4C` (14 bytes, `{req, cam[0], cam[2], …}`) and answers with the **current committed mode**
  (`43A3E0E6` = latest slot of `0x43D68B3C`). p2: `0x43D68B3C` = `{1, 0x0D00, 0x0D00}`.
* The mode is committed by the **slot-B RX parser** `4369B88C` (link task 99, after every micro → CPU TLV frame), which
  ends with `4369D880(R6)` (`4369BD54`). `R6` (disassembly `4369BB8E..4369BD80`):

  | requested (`dword_43D68B4C`) | committed when | otherwise |
  |---|---|---|
  | 0x0D09 | `(cfg byte_43D68AC6 & 2)` or **RX TLV 0x03 data[1] bit 0** | current mode kept |
  | **0x0D20** | (`(byte_43D68AC6 & 1)` or **RX TLV 0x03 data[1] bit 6**) **and** `byte_43E62C13` **and** `byte_43E62C16` (log 0x166) | current mode kept |
  | anything else | — (current mode kept) | |

  With bit 6 set the same function also starts the two reads (below): first time `byte_43E62C5B == 0` → log 0x161,
  `{…, 0x20000500, 0x100}` to queue **0x2804C1**, `5B = 1`; then, once `C13 != 0` and `byte_43E62C73 == 0` → log 0x162,
  `{…, 0x00030100, 0x100}` to 0x2804C1, `C73 = 1`.
* `C13` / `C16` are set by link events **0x30 / 0x31** (dispatcher `4369CF88`, `4369D004..4369D010`). Those are posted
  by `4369C42C` → `4369AFA0(0x30|0x31)` after the read result has been delivered (A.4). (`4369B6E0`, the 0x2804C0
  handler, also sets them directly for a reply whose byte 0 ≠ 1.)
* `byte_43D68AC6` (config block `0x43D68AB0`) = 0 at run time (p2), so only the micro bit can start it.
* Live (p2): RX TLV 3 = `03 04 00 00 | 00 00 00 02` (bit 6 clear), `C13 = C16 = 5B = C73 = 0`.

### A.3 What the CPU sends: TX TLV 0x0A data[1]

`4369C580` (slot-B TX builder, before every exchange; disassembly `4369C584..4369C5E2`), TLV pointer
`dword_43D68920[0x0A]` (`off_43D68948`), byte `+5` = data[1]:

| requested mode | TX TLV 0x0A data[1] |
|---|---|
| 0x0D09 | bit 1 set |
| 0x0D20 | **bit 6 set** (log 0x160 when it was clear) |
| other | bits 6 and 1 cleared |

This is the same TLV the model already parses for the lens power request (data[1] bit 0, RE_LENS_EMOUNT 2.3). So the
handshake has the same shape as the lens one: request in TX TLV 0x0A, answer in RX TLV 0x03.

### A.4 The slot-A memory read ("SaveUsr" POW/CAM)

Task 48 (`439110E4` → `43913A8C`, queue 0x2804C1) turns `{addr, len}` into a chunked read through the slot-A packet.
State: `dword_43E0C950` addr, `dword_43E0C94C` len (≤ 0x200), chunks = ceil(len / 120) → `B2/B3/B6`
(`byte_43E749B2` total, `B3` TX chunks left, `B6` RX chunks left), `B9` RX chunk index, `BB` TX chunk index, `B8`
"first request just sent".

**CPU → micro, TX slot-A packet type 0x41** (`43915604` + `4391383C`; built instead of the normal packet while
`B3 != 0`, one per tick, lock-step with RX):

| byte | content |
|---|---|
| 0 | **0x41** |
| 1..4 | micro address, LE: `addr + 120·k` |
| 5 | chunk length n = min(len − 120·k, 0x78); **bit 7 set = write** (then data follows at 6..) |
| 6.. | write data only; zero for a read |
| 0x7E–0x7F | CRC-16/CCITT-FALSE, LE (as every frame) |

**Micro → CPU, RX slot-A packet type 0x40** (`439149F8` → `43913AC0`): byte 0 = **0x40**, `n = byte5 & 0x7F`
(1..0x78, and `120·B9 + n ≤ 0x200`, else the read is aborted with log 0xC0), **data at bytes 6..6+n**, valid CRC.
The data are appended at `word_43E747B2 + 120·B9`. The address bytes 1..4 are **not checked** [?: echo them]. The
normal field parsers do not run for a 0x40 packet.

**Timing rule (important):** the RX of an exchange is what the micro loaded **before** that exchange, so the answer
to the 0x41 sent in exchange k must come in the RX of **exchange k+1**:
* the RX of the exchange that carried the first request is ignored (`B8`, return 18), whatever its type;
* if a **non-0x40** RX arrives while chunks are outstanding and `B6 != B3`, the whole read restarts (`43913788`, log
  0xC3, `B6 = B3 = B2`, `byte_43EDAA86 = 3`). This is the loop seen in p3 (`T48_restart` every second packet).

Sequence for 256 bytes (3 chunks: 0x78, 0x78, 0x10): TX 0x41(k0) | RX any → TX 0x41(k1) | RX 0x40(k0) → TX 0x41(k2) |
RX 0x40(k1) → TX normal | RX 0x40(k2) → done (log 0xC2) → `43913980` posts `{byte0 = 1, addr, len, data[≤0x200]}` to
**0x2804C0** → `4369B6E0`: byte0 == 1 and addr 0x20000500 → `4369D64C(…, cb 4369C42C, 0x30)` (0x00030100 → 0x31).

Delivery: `4369D64C` finds the region in the table at `0x43EA797C` (36-byte entries `{u16 0x185, flags, u32 mailbox,
handler 4369D35D, addr, len, …, u8 mark}`), sets the mark and sends the data to the mailbox with `4355DC44`; the
callback posts event 0x30/0x31. Table (static, `av-cam.bin`):

| entry | micro addr | len | flags | mailbox |
|---|---|---|---|---|
| 0 | 0x00020280 | 0x180 | 0x0F | 0x01850001 |
| 1 | 0x00010100 | 0x80 | 0x0F | 0x01850000 |
| 2 | **0x20000500** | **0x100** | 0x1F | 0x01850004 |
| 3 | **0x00030100** | **0x100** | 0x1F | 0x01850005 |

The mailbox handler `4369D35C` sees the mark, logs 364 and drops the data; without the mark it would **write**
mailbox data back into the micro (255-byte pieces through `4369D2F0`). No other receiver of 0x0185000x was found
[?]. So the content of the read is not checked by anything that gates the off; its real content is unknown [?].

### A.5 Spec for the code agent (sub-micro model, `hw/arm/sony_a5000.c`)

Boundary: the micro side of the existing frame HLE (RE_SUBMICRO_LINK §6). Firmware is not touched; nothing else in
the machine changes.

1. **Power-off acknowledge (slot B).** In `a5k_submicro_slot_b()`, parse TX TLV 0x0A (`a5k_submicro_tlv(tx, 0x0a, 2)`,
   already done for the lens) and keep `s->poff_req = t[1] & 0x42`. When building the reply:
   * `poff_req & 0x40` → RX TLV 0x03 data[1] **|= 0x40**;
   * `poff_req & 0x02` → RX TLV 0x03 data[1] **|= 0x01** (ack of mode 0x0D09; static only, not exercised) [?];
   * recompute the CRC.
   The ack follows the request (set while requested, cleared when the CPU clears it). Keep `mkdata.py` clearing
   the snapshot bit: the initial state is "no request". Answering in the same exchange is fine: `4369B88C` re-checks
   every frame [?: real micro latency unknown]. Log once: `sony-a5000: sub-micro power-off request acknowledged`.
2. **Memory read (slot A).** In the slot-A path (`a5k_submicro_xfer`, both the 128-byte and the 256-byte case):
   * If the **previous** slot-A TX was type 0x41 with byte5 bit 7 = 0 (read), the reply to **this** exchange is a
     type-0x40 frame: `out[0] = 0x40`, `out[1..4]` = the previous TX bytes 1..4, `out[5] = n` (= previous TX byte 5),
     `out[6..6+n)` = micro memory at that address, rest 0, CRC. Otherwise reply as today (snapshot slot A + RTC).
   * Then remember the current TX (type, bytes 1..5, and for writes bytes 6..) for the next exchange.
   * Micro memory: four zero-filled regions from the table in A.4 (0x00010100/0x80, 0x00020280/0x180,
     0x20000500/0x100, 0x00030100/0x100) [? real content; zeros pass]. A read outside them returns zeros and logs
     `LOG_UNIMP`. A write (TX byte5 bit 7) stores bytes 6..6+n into the region and is answered with a 0x40 frame
     `n = 0` [? write ack format not decoded; no write was seen on this path].
   * Reset all of this in `a5k_boot_reset` (power cycle).
3. **Do not** set TLV 3 bit 6 permanently (p3 shows the read starts then): it must follow TX TLV 0x0A bit 6.

Expected (USB updatershell, OK pressed), in this order:
* av-cam log ids 0x160 (TX bit 6), 0x161, task-48 `43913A8C` with `{0x20000500, 0x100}`, three `43915604` (0x41) and
  three `43913AC0` (0x40) without `43913788`, `4369B6E0` byte0 = 1, event 0x30 (`C13 = 1`), 0x162, the same for
  0x00030100 (`C16 = 1`), log 0x166, `4369D880(r0 = 0x0D20)`;
* `xp /16xh 0x83DC7160` → index 12 = 0x0D20, then the path finishes: `CapconReply` (`4375A338`, `*r0 = 0x300E`),
  libObj `+17A156` (`cb_rcvret`), im.elf `0xADAC` with **r0 = 0x102**;
* then A.7 (stops at gap (b) until §4.3 is implemented).

### A.6 Plain power-off and other triggers

* **0x300E is sent on every OFF that reaches im.elf.** `CameraBackground_inact` (libObj `0x17A9F8`) posts code 64
  unconditionally; `sub_17A760` answers it with `sub_17A0E8(0x300E)` from states 1, 3 and 4 (state := 2). im.elf runs
  the inact round for every upm OFF (0x920143), whatever the factor.
* **Every path into av-cam state 0x20 needs 0x0D20.** Sequencer table (`pmemsave 0x83DC71A0`, 250 × `{from, to,
  steps, len}`): transitions 0→20, 1→20, 2→20, 6→20, 7→20, 8→20, 0x21→20, 0x60→20 all contain `D00 … D20`; only
  0x43→20 and 0x58→20 keep 0x0D00. **0x0D09** appears only in 0x38→9 and 8→9 (`0x43B70124`, `0x43B6FAAE`) [? which
  request leads there; probably the WBI/warm off], 0x0D50 in 1→0x50.
* So a cold power-off by any cause stops at the same gate, and the A.5 model fixes all of them. Warm (type 27) offs
  were not traced [?].
* **Non-USB triggers [?, not tested]:**
  * Power lever = front **CAkey1 bit 0**, active low (input.ko key table, RE_INPUT_I2C_POWER §7: keycode 0x0002
    POWER) = slot A **byte 28 bit 0** (snapshot 1 = on). The model binds only bits 1/2 (PLAY/MENU,
    `a5k_submicro_front_key`). Binding a host key to bit 0 (held at 0 = lever off) would give a power-off
    without USB. Which off type it produces (libObj `M_ROOT ActionReqPowerOff` defaults to type 26 → upm 27,
    warm) is not traced.
  * libObj `sub_319B14(20, 28, 32)` (`"Reset:1"` in `ActionRecoveryComplete`) is a cold restart from the UI, path not
    identified.
  * USB `updatershell` remains the proven reproducer (3/3 runs reach the gate in about 25 s after OK).

### A.7 After the reply: the rest of the power-off (p3, after the byte write)

Observed order (probes, ~20 s wall clock in total):

1. `4369D880(0x0D20)` (task 99) → module 0xD = 0x0D20 → path end → task 68 `CapconReply(0x300E)`.
2. libObj CameraBackground gets the reply (`+17A156`), sends inact_cmp → im.elf `0xADAC` r0 = **0x102** (the third
   and last library after 0x155 and 0xF8).
3. im.elf → OFF_CMP to upm → upm 33 → 34 → im.elf case **0x92014F** → `sub_9FB4(0)` (probe r2 = 0x92014F) writes
   `"mem"` to `/sys/power/state`.
4. Kernel suspend: all drivers suspended (no stall seen: `cxd90014_pm_prepare` / `udif_driver_suspend` returned) →
   **`cxd90014_pm_enter` `0x40052220`** (CPU0, task im.elf) → sleep entry **`0x4037BEA0`** → copies the IRAM code →
   **`0xC0020000`**.
5. IRAM: MMU off. The per-channel DDR steps (+0xC4/+0xD8, +0xAC, poll +0x128) were **skipped**: `ddr_ctrl` bytes at
   `0xC00200F4` / `0xC00200F8` = 0 in this run [?: §1.2 expects "dpd,dpd" for type 28; not traced why they are 0].
   `+0x78` written `0x0A000000`, then **CPU0 spins at `0xC0020094`** reading `0xF0104000 + 0x7C` and comparing the low
   byte with **0x25** (registers: R0 = 0xF0104000, R2 = 0x25, PSR SVC, IRQ off). → **gap (b)**, spec §4.3.
6. Not reached yet: `0xC00200C8` writes `*(0xF2910048) = 0x400` (IRAM parameters seen: `0xC00200EC = 0xF2910048`,
   `0xC00200F0 = 0x400`) = **p0.10 low** → `wfi` loop at `0xC00200DC` → **gap (c)**, sub-micro power cycle (§4.4;
   restart bit in task-48 TX, §1.2).

### A.8 Open questions

1. Real micro latency for the TLV 3 bit 6 ack and real content of the four micro regions (A.4). Zeros are accepted by
   the firmware; whether a later boot reads them back (the write-back path of `4369D35C`) is not known.
2. Format of the micro's answer to a slot-A **write** (TX byte5 bit 7); not used on this path.
3. Why `ddr_ctrl` is 0 in IRAM at suspend (A.7 step 5) although upm should write `"dpd,dpd"` for type 28.
4. Which request moves av-cam to state 9 (mode 0x0D09, TLV 3 data[1] bit 0 ack), and whether warm offs (type 27) use
   it.
5. Off type produced by the power lever (CAkey1 bit 0) and by APO; a host-key binding for bit 0 would make a non-USB
   regression test.

### A.9 Reproduce

* Probes (`fn=`; reload the file at the OK press to reset the caps):
  ```
  437d9e68 CapconExec  4375c920 CAM_SyncReq  43a70f76 PATH_setup  4375a338 CapconReply
  4376a54c M12_hdl  4376a568 M12_async  4369d8bc M12_task  4369d880 SM_setmode  4369b88c SM_slotA
  4369b6e0 SM_C0rcv  43913a8c T48_req  43915604 T48_tx41  43913ac0 T48_rx40  43913788 T48_restart
  43913980 T48_reply  4369d64c SM_D64C  4369c42c SM_cb
  +17a0e8 cb_send  +17a156 cb_rcvret  +17a9f8 cb_inact
  9fb4 im_mem  40052220 pm_enter  4037bea0 sleep_entry  c0020000 iram0  c0020094 iram_7c  c00200c8 iram_poff
  ```
  plus `pc=0xadac,pc=0xb10a,s=0` on the plugin line (im.elf inact_cmp / inact dlsym).
* State (HMP): `xp /16xh 0x83DC7120` / `0x83DC7160` (index 12 = module 0xD), `xp /4wx 0x83D68B38` (committed mode),
  `xp /8xb 0x83E62C10` (`C13` = +3, `C16` = +6), `xp /8xb 0x83E62C58` (`5B` = +3), `xp /8xb 0x83E62C70` (`C73` = +3),
  RX TLV 3: `xp /1wx 0x83D689A0` gives the TLV pointer P, then `x /8xb P`.
* Hand check of the rest of the sequence (diagnostic only, not a fix): with TLV 3 bit 6 set
  (`A5K_SMFRAMES` = a copy of `data/submicro_frames.bin` with byte 0xA1 |= 0x40 and the slot-B CRC recomputed), after
  `T48_restart` appears: `arm-none-eabi-gdb -batch -ex 'target remote :PORT' -ex 'set *(unsigned char*)0x43E62C13=1'
  -ex 'set *(unsigned char*)0x43E62C16=1' -ex 'set *(unsigned char*)0x43E62C73=1' -ex detach`.
* Pitfall: a gdb **watchpoint** session on this machine (two watchpoints, plugin loaded) hung QEMU's main loop
  (monitor and gdb stub dead, SIGKILL needed). Use plugin probes and plain memory reads/writes instead.

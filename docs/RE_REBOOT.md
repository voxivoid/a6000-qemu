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

# Guest root shell and file transfer while the stock UI runs

Scope: how a developer gets a **root shell inside the running guest** (stock firmware 1.10, stock UI up) and how files
move **in and out** (e.g. a native ARM binary dropped into a tmpfs and run next to `im.elf`). It explains why the
existing `debug-console=on` (UART0) blacks out the UI and ignores input, and it gives a spec for a code agent. No
emulator code was changed for this document.

Related: `docs/BOOT.md`, `docs/RE_UPDATER_BOOT.md` §5 (updater console), `docs/RE_USERLAND_SUBCPU.md` §1.1 (init),
`docs/RE_AVCAM_IRQ_BOOT.md` §5.3 (UART owners), `docs/HARDWARE.md` (UART rows). `[?]` = not proven.

Evidence:
* Static: `/sbin/init` of the stage-2 initrd (not stripped, has symbols; objdump), the rootfs `nflasha7` (`debugfs`),
  `/usr/bin/s_start_android.sh`, `re/vmlinux.elf` (objdump), `re/ida/liro.ko.c`, `re/ida/av-cam.irq.c`, av-cam.bin
  (capstone, base 0x43541000), `/setting/kemco.txt` from the update package (`0800_appli/setting/kemco.txt`).
* Dynamic (HEAD binary `2587a4d`, pristine NAND clone per run, no SD, `-display none`):
  * r1, r6: `A5K_DEBUG_CONSOLE=1` (`console=ttyAM0 amba2.console=1`), serial log; r6 with `a5ktrace` probes on
    `pl011_startup`, liro `def_inh` and its return, and the av-cam monitor setup.
  * r2, r5: stock cmdline (`console=ttynull`), klog via `tools/klog.py`, screendumps.
  * r3, r4, r8: stock cmdline with **only** `console=ttynull` replaced by `console=ttyAM1 amba2.console=1` (`-append`),
    `-serial null -serial null -serial tcp:...` (UART2 on a socket); shell commands, file push/pull, screendumps.
    r4 and r5 were left idle for about 7 min to check the idle power-off (§9).

---------------------------------------------------------------------------------------------------------------------

## 0. Summary

| Question | Answer |
|---|---|
| Who owns UART0 / ttyAM0? | **av-cam** (the in-kernel ITRON image run by `liro.ko`). Its 9600-baud "monitor console" claims IRQ 150 and programs UART0 on every boot. Linux registers ttyAM0 but nothing in the stock system opens it. |
| Why is the UI black with `debug-console=on`? | `console=ttyAM0` makes init open `/dev/ttycons` (→ ttyAM0) at 0.7 s, so Linux's PL011 driver takes IRQ 150 **first**. av-cam's `def_inh(150)` then fails (**returns -33**, r6) while av-cam still reprograms the same UART. Userland output on ttyAM0 stops right after `input: D keyboard` (3.4 s). At ~70 s an av-cam task (task 113) oopses on a NULL source pointer and Sony's exception monitor takes over the console (`Exception>` prompt). The system halts there, so the screen stays black. |
| Why is serial input ignored? | Before the crash, UART0 has two drivers: av-cam's RX callback reads the FIFO and drops everything that is not a `#`/`&`/`_` monitor command. After the crash only the exception monitor reads the port. It answers `echo: Command not found.`, so the RX path itself works. |
| A stock setting where the UI **and** a serial shell work? | **Yes: `console=ttyAM1 amba2.console=1`** (UART2, 0xF2002000, IRQ 152). Linux registers ttyAM1 on every stock boot (`uart2: ttyAM1 at MMIO 0xf2002000 (irq = 152)`) and nothing else uses it. Stock init then runs its own `shell_loop` (`/bin/ash --login`, root) on `/dev/console` = ttyAM1. The shooting screen comes up normally (r3, r8). No firmware, NAND or loader change is needed. |
| Recommended shell | Machine option that swaps only the `console=` token for `console=ttyAM1 amba2.console=1` and wires UART2 to a host chardev (§6). Default off: the cmdline stays byte-identical to the stock one. |
| Recommended file transfer | Over the same serial shell, using stock tools only: `busybox stty raw` + `cat` (push) and `cat` with `-opost` (pull), checked with `busybox md5sum` (§5). Verified: a 4460 B static ARM ELF and a 256 KiB random file pushed, md5 equal, the ELF runs from `/android/a5k/` (`HELLO_FROM_TMPFS`, rc=42). The 256 KiB file was pulled back byte-identical. About 50 KB/s. |
| Where Track B binaries run | `/android` is a **tmpfs** that the stock `s_start_android.sh` mounts itself (`mount -t tmpfs tmpfs /android`). `mkdir /android/a5k` works and the files there are executable. The emulator needs nothing extra. Not `/tmp`: that is NAND vfat (`nflasha10`). Not the SD: that is FuFsys (fuse) at `/android/mnt/sdcard`. |

---------------------------------------------------------------------------------------------------------------------

## 1. UART ownership on the stock firmware

| UART | PA / IRQ | Linux | av-cam | Stock user |
|---|---|---|---|---|
| UART0 | 0xF2000000 / 150 | `ttyAM0` (204:16), `/dev/ttycons` → `ttyAM0` symlink | monitor console, 9600 baud, RX+TX callbacks | **av-cam** |
| UART1 | 0xF2001000 / 151 | not registered | lens / high-speed port (`43993798`, RE_AVCAM_IRQ_BOOT §5.3) | av-cam |
| UART2 | 0xF2002000 / 152 | `ttyAM1` (204:17) | the monitor can be put here (`port==2` → IRQ 152), not observed | **nobody** |

Observed `/proc/interrupts` (r3, shell on ttyAM1, UI up):
```
150:          0          0          0          0       GIC  liro-150
151:          0          0          0          0       GIC  liro-151
152:       1096          0          0          0       GIC  cxd90014-uart
```

### 1.1 av-cam's monitor console
* `sub_4355F7D0(ctx, port)` is called from `4355F150` (task 56, `lr=4355f187`) with `port = ldrb [ctx+0x30]`. The
  observed value is 0 in every run (r6 probe: `r1=00000000`). It runs only if `sub_4355FE30()` returns 0, and in 1.10
  that function is a plain `return 0`, so the monitor is **always** installed.
* port 0/1/2 → IRQ 150/151/152 (`dword_43D296E0`). The interrupt is registered through the liro service table
  (`sub_4355D8C4` → liro `def_inh`, module +0x12A0). Then the UART is opened and configured (`sub_43919D70`,
  `sub_43919E24`: 9600 baud, 8 bit, RX callback `sub_4355F6BC`), RX is enabled (`sub_4391A08C(0,0,1)`) and
  `ena_int(150)` is called.
* QEMU trace (`pl011_baudrate_change`): `ibrd 312, fbrd 32` (= 9600 baud at 48 MHz) appears in the stock run (r2) as
  well as in the ttyAM0 run.
* RX callback `sub_4355F6BC`: reads up to 15 bytes (`sub_4391A004`) and only reacts to frames that start with `#`, `&`
  (e.g. `&S...`) or `_`. Everything else is dropped. With `-serial file:` on UART0 (r3) the monitor printed nothing
  during boot.
* liro `def_inh` → `os_request_irq` → `subsys_request_irq(irq, isr_wrapper, 0x10020, "liro-N")`. This interrupt is
  **not shared**. On failure `def_inh` returns -33.

### 1.2 Linux PL011 (Sony `amba2` driver)
* `pl011_startup@0x401b3710` → `request_threaded_irq(irq, pl011_int@0x401b3810, NULL, 0, ...)`. This interrupt is
  **not shared** either. It runs when the tty is first opened, not at probe time.
* `pl011_console_write@0x401b2f50` returns early when the port's mode word (`uart_amba_port+0xC8`) is 0. The mode is
  set by `amba2.console=` (and later by `AMBAUART_IOC_SETMODE 0x40047502` in `amba_ioctl@0x401b3348`; this is what
  the updater's `ud_cons.elf` uses). Without `amba2.console=1` the console prints nothing (HARDWARE.md note).
* `pl011_int` drops received characters while the mode word is 0 (`401b391c`).

## 2. `console=ttyAM0` (`debug-console=on`): why the UI goes black

Sequence (r1/r6):
1. `/sbin/init` `parse_cmdline@0x934c` finds the literal `console=ttyAM0` in `/proc/cmdline` and sets
   `is_enable_ctrl_c` (`0x144dc`). After `pivot_root`, init reopens fds 0-2 on **`/dev/ttycons`** instead of
   `/dev/console` (`main+0x10e..0x178`) and clears O_NONBLOCK. That open calls `pl011_startup` → Linux owns IRQ 150.
   The r6 probe shows `pl011_startup` hit from `[init]`, before liro is loaded.
2. liro loads av-cam (2.8 s). The av-cam monitor setup calls `def_inh(150)`. r6:
   ```
   A5KT cpu0 pc=4355f7d0 r0=43e6191b r1=00000000 ...            [liro-kliro_56]   monitor setup, port 0
   A5KT cpu0 pc=3f4012a0 r0=00000096 ...                        [liro-kliro_56]   def_inh(150)
   A5KT cpu0 pc=4355d8d6 r0=ffffffdf ...                        [liro-kliro_56]   returns -33 (all other def_inh: 0)
   ```
   av-cam ignores the error. It reprograms UART0 (9600 baud, its own interrupt mask) under the Linux driver.
3. All userland stdio is on ttyAM0, because init's children inherit `/dev/ttycons`. Userland output stops after
   `input: D keyboard` (3.4 s). There are no `zygote`/`bootanim` lines, which the ttyAM1 run prints at 8-12 s.
   Writers presumably block on the tty once Linux's TX interrupt is masked/never serviced **[?]** (not traced at
   register level).
4. At 70.7 s (r1):
   ```
   Unable to handle kernel NULL pointer dereference at virtual address 00000000
   PC is at 0x4394aa4c          (memcpy)
   LR is at 0x437dee4d          (437dee48: blx memcpy, src = [obj+0xC38] = NULL, len 4)
   Process liro-kliro_113
   ...
   Entering exception monitor.
   Exception>
   ```
   Kernel `em_exception_monitor@0x400a67ac` then polls `console_read` for commands and the machine is halted. In r1
   the host sent `\r` and `echo HELLO_SHELL` at 150 s, and the monitor replied `echo: Command not found.`. How the
   stalled userland/UART fight leads to the NULL pointer in task 113 is **[?]**. The crash happens without any serial
   input.
5. The stock run (r2, `console=ttynull`) has no Oops and reaches the shooting screen.

Conclusion: on the 1.10 main firmware UART0 is av-cam's port. A Linux console on ttyAM0 is a configuration the
firmware does not support while av-cam runs. It does work in the updater core, which has no av-cam
(RE_UPDATER_BOOT §5). The same conflict presumably exists on a real camera **[?]**. `debug-console=on` should
therefore stop using UART0 (§6.2).

## 3. Where the cmdline comes from (bootloader contract)

`/setting/kemco.txt` (written by the updater from `0800_appli/setting/kemco.txt`) is **byte-identical** to the board's
`a5k_default_cmdline`: a `kemco` line, one parameter per line, `*` terminator. `setup_arch` looks for exactly this
`"kemco"` block at PA 0x80033000 (BOOT.md). So the Sony bootloader very likely builds the cmdline from this file
**[?]** (the bootloader binary is not available). The stock way to get a console on a real unit is therefore to edit
`kemco.txt` (e.g. `console=ttyAM1 amba2.console=1`). In the emulator, a machine option that rewrites the board's
copy of the cmdline does the same thing without touching NAND. An optional fidelity follow-up is listed in §8.

## 4. The stock shell on ttyAM1

`console=ttyAM1 amba2.console=1` (and nothing else) changes the boot as follows:
* `parse_cmdline` does not match `console=ttyAM0`, so `is_enable_ctrl_c = 0`. init reopens fds 0-2 on `/dev/console`
  (5:1). That device is now ttyAM1, so `pl011_startup` takes IRQ 152, which nobody else wants.
* av-cam gets IRQ 150 as in the stock boot (`150: liro-150`).
* At the end of `main`, `shell_loop_pvt@0x96a4` (init itself, PID 1) execs
  `/bin/ash -c "sleep 1; umount /initrd;freeramdisk /dev/ram0;while true;do /bin/ash --login;echo shell_loop_pvt;done"`.
  The result is a root `ash` on UART2. It respawns when it exits.
* All other init children (`bootin.elf`, `im.elf`'s `D/stella` lines, `s_start_android.sh`, Android init) also write
  to the console, so boot output is chatty. After boot the console is quiet apart from occasional kernel lines
  (`hwtimer_on: ...`); `busybox dmesg -n 1` silences those.

Observed (r3, r8):
```
[    0.433430] uart2: ttyAM1 at MMIO 0xf2002000 (irq = 152) is a AMBA/PL011
[    0.450642] console [ttyAM1] enabled
...
BusyBox v1.13.4 (2012-07-17 15:11:04 JST) built-in shell (ash)
/ # echo HELLO_SHELL; id; ls /
HELLO_SHELL
/bin/ash: id: not found
android      etc          log          sbin         tmp          var
bin          initrd       mnt          setting      tmp_network  version.txt
...
```
The shooting screen is complete (screendump: NO CARD, 30", F--, ±0.0, ISO AUTO), and no Oops occurs.

Limitations of the init shell, with their stock-tool workarounds:
* There is no controlling tty (`/bin/ash: can't access tty; job control turned off`), so Ctrl-C does not interrupt a
  command. Workaround (verified r8): `exec busybox getty -n -l /bin/ash 115200 ttyAM1`. getty does `setsid` and
  makes ttyAM1 the controlling tty, so the new ash has job control. `sleep 30` was interrupted by `\x03`. When that
  ash exits, init's loop starts a plain one again.
* Busybox 1.13.4 has applets without `/bin` links: `stty`, `md5sum`, `tail`, `getty`, `telnetd`, `dmesg -n` must be
  called as `busybox <applet>`. There is no `id`, `free`, `head`, `base64`, `uudecode` or `printf`.
* The cmdline change applies to stage 1 as well (same `s->cmdline`), so the loader's `fail()` shell also appears on
  UART2. That is harmless and useful.

## 5. File transfer

### 5.1 Recommended: serial, stock tools only (verified)
Push (host → guest):
```
busybox stty raw -echo min 0 time 30; cat > /android/a5k/FILE; busybox stty sane; echo @@PD
```
After about 0.8 s the host sends the bytes in 1 KiB chunks, 10 ms apart. `cat` ends 3 s after the last byte
(`VMIN=0, VTIME=30`). Then the host checks `busybox md5sum FILE` against the local md5.
Pull (guest → host):
```
busybox dmesg -n 1                     # keep kernel messages out of the byte stream
ls -l FILE; busybox md5sum FILE        # size + md5
busybox stty raw -echo -opost; echo @@BG; cat FILE; busybox stty sane
```
The host reads exactly `size` bytes after the `@@BG\n` marker and compares the md5.

Results (r8, prototype `xfer2.py` in the RE scratch dir):
```
push 4460 B   in 4.7s (incl. 3 s idle timeout); remote md5 7be1219d... local 7be1219d...
/ # chmod 755 /android/a5k/hello; /android/a5k/hello; echo rc=$?
HELLO_FROM_TMPFS
rc=42
push 262144 B in 8.1s (incl. 3 s idle timeout); remote md5 9d1da22d... local 9d1da22d...
pull 262144 B; remote md5 9d1da22d... local 9d1da22d...     (cmp: identical)
```
Pitfalls that were hit:
* `stty` without `busybox`: "not found". The tty then stays canonical with echo, and the data is echoed and eaten by
  ash.
* `dd bs=1 count=N` with unpaced host writes (r3): a 256 KiB push lost bytes and `dd` never finished. Use `cat` with
  VTIME and pace the host side.
* Use `busybox dmesg -n 1` before pulling. Kernel console lines are interleaved with tty output otherwise.

### 5.2 Alternatives considered

| Path | Status | Verdict |
|---|---|---|
| SD image (`A5K_SD`) | The card is FuFsys (fuse) at `/android/mnt/sdcard` (`rw,nosuid,nodev`; noexec on the real camera per project notes). Files must be copied to tmpfs before they can run. The host needs a FAT32 writer: macOS refused to mount a `tools/mksd.py` image (`Volume on disk7s1 failed to mount`). Results come back only by reading the image after shutdown. | Good for large, pre-boot inputs. Needs a host FAT tool **[?]**. Not the primary path. |
| USB (A5KU cable) | Stock: no adbd (`init: cannot find '/sbin/adbd', disabling 'adbd'`). `usbg_adb.ko` does not load (`init ko_insmod: error: open file: /usr/kmod/usbg_adb.ko` and `insmod: ... unknown symbol` in `s_start_android.sh`). MTP works (README) but only exposes the SD card and needs a host MTP client over A5KU. | No stock adb. MTP is possible later; same SD/noexec limitation. |
| Network (telnetd, `ulogio telnet up`, OpenMemories) | busybox has `telnetd`, and ulogio has a telnet socket (`[ulogio] socket ignore because telnet flag is down`). The board has no Wi-Fi/Ethernet model. The kernel has no virtio, 9p or NFS. | Not available. |
| Second UART for data | UART0 belongs to av-cam and UART1 to av-cam's lens port. | Only UART2 is free; shell and data share it. |

## 6. Spec for the code agent

### 6.1 Machine option (board, `hw/arm/sony_a5000.c`)
Add a string property **`console=`** with the values `ttynull` (default), `ttyAM1`, `ttyAM0`:
* `ttynull` (default): nothing changes, and the cmdline stays exactly `a5k_default_cmdline`.
* `ttyAM1`: in the kemco/ATAG cmdline, replace the single token `console=ttynull` with
  `console=ttyAM1 amba2.console=1`. This is the same `g_string_replace` as the current `debug_console` path, only the
  text differs. No other parameter changes.
* `ttyAM0`: the current `debug-console=on` behaviour. Keep it for early-boot debugging only, and say in its
  description that the UI does not come up with it (§2).
* Keep `debug-console=on` as an alias. Recommendation: map it to `ttyAM1`, because UART0 conflicts with av-cam.
  If compatibility matters, map it to `ttyAM0` and warn with `warn_report` that the UI will not start.
* `-append` keeps overriding everything (unchanged).
* Optional **`shell-socket=HOST:PORT`**, following the `usb-socket` pattern (`qemu_chr_new` of
  `socket,host=..,port=..,server=on,wait=off`). It is used for UART2 when `serial_hd(2)` is NULL and implies
  `console=ttyAM1`. Then `pl011_create(A5K_UART_BASE + 0x2000, ..., chr)`. Without it the QEMU convention applies:
  `-serial` #3 = UART2 (`-serial null -serial null -serial tcp:...`).
* Comment evidence: init `parse_cmdline@0x934c` / `shell_loop_pvt@0x96a4`; `/proc/interrupts` 150 = `liro-150`;
  r6 `def_inh(150) = -33` under `console=ttyAM0`.

### 6.2 run.sh
* `A5K_SHELL=<port>` → `-M ...,console=ttyAM1` plus `-serial null -serial null -serial
  tcp:127.0.0.1:<port>,server=on,wait=off`, or `shell-socket=127.0.0.1:<port>` if implemented. UART0 is set to `null`
  explicitly so that QEMU's default serial backend never attaches to av-cam's monitor port. Default unset = no change.
* `A5K_DEBUG_CONSOLE=1`: follow §6.1. If it stays on ttyAM0, fix the comment ("the UI does not start").

### 6.3 Host tool `tools/a5kshell.py` (new, outside QEMU)
Generalise the prototype. Use plain sockets and no third-party modules, and run it with `python3 -I`.
* `a5kshell.py [--port N]`: interactive passthrough. Optional `--job-control` first sends
  `exec busybox getty -n -l /bin/ash 115200 ttyAM1`.
* `a5kshell.py push LOCAL REMOTE`: `mkdir -p` the dir, then raw `cat` with VTIME as in §5.1, paced 1 KiB / 10 ms,
  then md5 check. Fail loudly on mismatch.
* `a5kshell.py pull REMOTE LOCAL`: `busybox dmesg -n 1`, size + md5, `-opost` cat, md5 check.
* `a5kshell.py run CMD`: send a command, wait for an end marker such as `echo @@O""K` (the empty quotes keep the
  echoed command line from matching), and print the output.
* Default remote directory `/android/a5k`, a tmpfs created by stock `s_start_android.sh`. Never write to `/tmp`,
  `/setting` or other NAND partitions by default.

### 6.4 Track B runtime notes (no emulator work needed)
* Run from `/android/a5k/` (tmpfs, exec ok, lost on power cycle). For Android/bionic binaries use
  `busybox chroot /android /a5k/BIN` with the usual `LD_LIBRARY_PATH=/system/lib` (as on the real camera).
* glibc binaries built with the camera toolchain and static binaries run directly in the native root. A
  `-nostdlib` static ELF built with `arm-none-eabi-as/ld` (raw `svc 0`) works as a smoke test.
* RAM: the guest has the camera's 512 MiB (mem=200M + 312M). tmpfs counts against it, so keep pushed files small.

### 6.5 Acceptance tests (pristine NAND clone, no SD, `-display none`)
Run them within about 5 minutes of boot, or keep the camera awake. Otherwise the idle power save (§9) reboots the
guest in the middle of a test.
1. **Default unchanged**: no new option → `/proc/cmdline` (klog line `Kernel command line:`) contains
   `console=ttynull` and is identical to before; the shooting screen appears (screendump) at ≈2 min.
2. **Shell + UI**: `A5K_SHELL=<port>` (or `-M console=ttyAM1` + `-serial` triple) → within ≈3 min the screendump shows
   the shooting screen (NO CARD / 30" / F-- / ISO AUTO, allowing for blinking items), **and** over the socket
   `ls /` lists `android etc log sbin ...`, **and** `cat /proc/interrupts` shows `150: ... liro-150` and
   `152: ... cxd90014-uart`, and no `Unable to handle kernel` appears in the serial log.
3. **Push + run**: `a5kshell.py push hello /android/a5k/hello` (static ARM ELF printing a marker and exiting 42) →
   md5 match, `/android/a5k/hello; echo rc=$?` prints the marker and `rc=42`. The UI is still up afterwards
   (screendump).
4. **Pull**: push 256 KiB of random data, pull it back, `cmp` identical.
5. **Job control**: after `--job-control`, `sleep 30` followed by `\x03` returns to the prompt in < 2 s.
6. **Regression for the old option**: `debug-console=on` behaves as documented. If it now maps to ttyAM1, test 2
   passes with it. If it stays on ttyAM0, the warning is printed.

## 7. Candidate shell paths, ranked

| # | Path | Firmware changes | Board/loader work | Default-off safe | Verdict |
|---|---|---|---|---|---|
| 1 | **Stock init shell on UART2** (`console=ttyAM1 amba2.console=1`) | none | cmdline token swap + UART2 chardev | yes (default cmdline unchanged) | **Recommended. Verified.** |
| 2 | Stock init shell on UART0 (`console=ttyAM0`) | none | exists (`debug-console=on`) | yes | Breaks av-cam (§2). Only for early-boot logs. |
| 3 | `kemco.txt`-driven cmdline (loader reads `/setting/kemco.txt` like the bootloader) | none (data file in `/setting`) | loader passes the file to the board | yes if the shipped file is used | Fidelity improvement, but it is no simpler for users than #1. Optional (§8). |
| 4 | Loader-injected init hook (modified initrd/rootfs copy in RAM) | **patches stock initrd** | large | yes | Rejected: violates "firmware is never modified", and #1 needs no patch. |
| 5 | adb over the USB gadget | stock has no adbd/usbg_adb | — | — | Not possible with stock firmware. |
| 6 | Network telnet (busybox `telnetd`, ulogio telnet) | none | needs a NIC/Wi-Fi model | — | Out of scope (no network model). |
| 7 | virtio / 9p | kernel has no drivers | — | — | Impossible. |

## 8. Open questions

1. **[?]** The exact mechanism by which the UART0 fight under `console=ttyAM0` stalls userland: it could be av-cam's
   interrupt mask removing Linux's TX interrupt, or blocking tty writes. Also the link to the task-113 NULL
   dereference (`[obj+0xC38]`, `437dee32`). This does not matter for the recommended path; a register-level trace of
   UART0 IMSC/RIS around 2.8-4 s would settle it.
2. **[?]** Whether the real bootloader reads `/setting/kemco.txt`. The file is identical to the recovered cmdline,
   which is strong but indirect evidence. If confirmed, the stage-1 loader could pass the file's contents to stage 2
   (e.g. a new `a5kcopy -kemco <file>` that writes the block where the board expects it). That would also let a
   `console=` edit in the NAND clone work exactly like on hardware.
3. **[?]** Whether UART2's pins are wired to anything on a real a5000 (test pads). This does not matter for the
   emulator: the kernel registers ttyAM1 on every stock boot (`cfg.uart=5`; that bit 0 = UART0 and bit 2 = UART2 is
   inferred from the result, not traced).
4. **[?]** Which setting controls the idle power-off in §9: the Power Save Start Time menu item and its Backup.bin
   property. The shell cannot hold it off, because serial activity does not count as user activity.
5. **[?]** Whether binaries can run from the FuFsys SD mount in the emulator (the real camera reports noexec). Not
   tested, and not needed with the tmpfs path.

## 9. Idle power-off: an operational caveat, not caused by the shell

All three long runs power-cycled after about 5 minutes **without key input**. That includes the stock-cmdline
control r5, so the shell is not the cause:
```
r4 serial (guest uptime):
[  316.659598] PM: Syncing filesystems ... done.
[  316.687125] Freezing user space processes ... (elapsed 0.02 seconds) done.
[  316.739900] Suspending console(s) (use no_console_suspend to debug)
board (r4 and r5):
sony-a5000: sub-micro power-off request acknowledged (TLV 0x0a data[1] = 0x40)
sony-a5000: power-off line p0.10 low (pc ~0xc00200d8, off request 0x40)
sony-a5000: sub-micro power cycle (factor 8)
sony-a5000: power-on (DDR and IRAM cleared)
```
This is the firmware's idle power save. The board's `poff=restart` (default) turns it into a full cold boot, so the
shell session and everything in `/android/a5k` are lost (observed in r3: `/proc/uptime` 91 s and `/android/a5k`
gone after the cycle). Workarounds for developers, to document in README and in the tool:
* send a harmless key now and then (HMP `sendkey`; which key does not change UI state is **[?]**), or
* lengthen Power Save Start Time in the camera menu once (it is stored in the NAND clone) **[?]**, or
* use `-M poff=off` to see the shutdown instead of a silent reboot.
Fidelity note: on the real camera the idle power save turns the camera **off** (it stays off until the power lever
is used), it does not reboot. The board logged `slot-A TX factor byte 0x1e = 0x08, restart byte 0x20 = 0xff [?]` for
this off, so +0x20 bit 0 alone does not tell restart from off (the SWITCH_MODE restart had factor 13, +0x20 = 0x01).
Decoding the off kind (and a power-lever key to turn the camera back on) is a separate RE task (`RE_REBOOT.md` §6.2).

The tool (§6.3) should detect a reboot (a new `Linux version` / `BusyBox ... built-in shell` banner, or
`/proc/uptime` going back) and report it instead of hanging.

# Updater boot (nflasha1 "updater core") in the emulator

Scope: what the Sony bootloader does differently when the updater flag is set, how the emulator's stage-1 loader
reproduces it, and how far the unmodified updater core of firmware 1.10 runs. Everything was read from the emulated
NAND (made from the 1.10 image) and checked by booting it. Claims that are not proven are marked **[?]**.

Related: `docs/BOOT.md` (normal two-stage boot), `docs/NAND_PROTOCOL.md`, parent `NOTES.md` §9.

---------------------------------------------------------------------------------------------------------------------

## 0. Summary

| Item | Result |
|---|---|
| nflasha1 | 6 MiB **plain FAT12** (`mkdosfs`, 32 sectors/cluster, 12288 sectors). Raw `/dev/nflasha1` (252:1) holds it. The LZPT device is not used. |
| Boot files | `/boot/vmlinux.bin` (zImage, 977 820 B), `/boot/initrd.img` (cramfs, 3 809 280 B), `/boot/loadaddr.txt` = `0x81528000,0x80628000` |
| Hand-over | zImage **loaded and entered at 0x81528000**. Initrd at **0x80628000**. The command line is **built into the kernel**. It has no `kemco` parser, and ATAGs are not used. |
| Decision | `/setting/updater/mode` exists → updater core. Otherwise the normal boot runs. This is the file `up.sh` creates. |
| Result | Updater kernel boots on 4 CPUs. Then: `/sbin/init` → `UdtrMain.sh` → **UFP mode: `sauu` waits for a USB host** (`onEvent(CONNECTION)` val=0). A **root `ash` runs on ttyAM0**. Production mode (`mode1`) reaches `mf` + `sen.elf` waiting. |
| Board change | `hw/arm/sony_a5000.c` (qemu branch `updater-boot`): an extended loader descriptor, a per-reset kernel-entry literal, and clearing of the SMP release mailbox. |

---------------------------------------------------------------------------------------------------------------------

## 1. nflasha1 layout

How it was read: a stage-1-style initramfs loads Sony's NAND stack (`nand.ko boot_mode=1`), runs
`a5kcopy -f2p /dev/nflasha1 0x90000000` and then `pmemsave` (QEMU monitor). The FAT image is then mounted read-only on
the host (`hdiutil attach -readonly -imagekey diskimage-class=CRawDiskImage`).

```
/dat2              4 B   12 00 03 41   model code (ud_datcnv.elf-encoded, edited by "up.sh modmod")
/dat3              4 B   00 00 00 00   region     (edited by "up.sh modreg")
/p1chksum        105 B   md5sum list of the two boot images below
/boot/loadaddr.txt 22 B  "0x81528000,0x80628000"   (kernel PA, initrd PA)
/boot/vmlinux.bin 977820 B  ARM zImage (magic 016f2818 @0x24), gzip payload @0x3f8f
/boot/initrd.img 3809280 B  cramfs "Compressed ROMFS", 712 files, /version.txt = 41.01.28
```

The updater kernel (zImage payload, 2 059 904 B decompressed):
- `Linux version 3.0.27_nl-rt106 ... #1 SMP PREEMPT RT Tue Oct 1 17:20:18 JST 2013`. This is a different build from
  the normal kernel on nflasha3, which is a raw Image at 0x80038000.
- The decompressor's `zreladdr` literal (file offset 0x88c) is **0x80038000**. This is the same text base as the normal
  kernel: PAGE_OFFSET 0x40000000, `.text 0x40038000`.
- Built-in `CONFIG_CMDLINE` (raw image offset 0x1d6fe0):
  ```
  initrd=0x80628000,0x00F00000 root=/dev/ram0 init=/sbin/init stack_usage=y heap_profile.records=0
  cxd90014.bam=N console=ttyAM0,115200n8 rw noatime noalign amba2.console=0 cfg.uart=5 cfg.poff=1,1
  pm.ddr_ctrl=dpd warm.boot_drop=0 warm.ignore_drop=1 mem=1024M@0x80000000@0 memrsv=0x28000@0x80000000
  memrsv=0x3D270000@0x82D90000
  ```
  The kernel image does not contain the string `kemco`, so `setup_arch` cannot take the normal boot's cmdline block at
  PA 0x80033000. The board still passes an ATAG_CMDLINE (the normal cmdline) at 0x80024000. The kernel logged the
  built-in line above, so **ATAGs are not used** (as in the normal kernel, whose head.S clobbers r2).
- The initrd window `0x80628000 + 0xF00000` ends exactly at the zImage load address 0x81528000.

## 2. Boot contract: normal vs updater

| | Normal (nflasha3 = `/system`, LZPT `dnflasha3`) | Updater (nflasha1, plain FAT) |
|---|---|---|
| kernel | `vmlinux.bin` raw Image, copied to and entered at 0x80038000 | `boot/vmlinux.bin` zImage, copied to and entered at **0x81528000**. It decompresses itself to 0x80038000 |
| initrd | `initrd.img` ext2 at 0x80428000 (8 MiB, cmdline `initrd=0x80428000,8M`) | `boot/initrd.img` cramfs at **0x80628000** (cmdline `initrd=0x80628000,0x00F00000`) |
| cmdline | `kemco` block at PA 0x80033000 (from the bootloader) | **built-in**. The bootloader passes nothing |
| MUMIN state @0x80000000 | needed (`nand.ko` `boot_mode=0`) | **needed**. The updater's `insmod.sh` loads its own `nand.ko` without `boot_mode`. The hand-off written by the normal stage-1 `nand.ko` is accepted (`nflasha0: p1 ... p23` comes up) |
| Backup.bin @0x82EC0000 | needed (`backup.ko Backup_load=0`) | loaded the same way. The updater's `backup.ko Backup_save=0` logs `** NO SAVE MODE **`. Whether it reads the RAM copy is not checked **[?]** |
| av-cam.bin @0x83541000 | needed (liro.ko) | **not loaded**: the updater has no liro/av-cam. Whether the real bootloader skips it is unknown **[?]** |
| memory | `mem=200M@0x80000000 mem=312M@0x8C800000` + reserves | `mem=1024M@0x80000000` with everything above 0x82D90000 reserved: "Memory: 19840k/19840k available". It works on the 512 MiB board because the reserved range is never touched |

## 3. Updater-mode decision (what the bootloader does)

The flag protocol, from `/usr/bin/up.sh` (normal rootfs) and the updater's `UdtrMain.sh`:

| `/setting/updater/` | Written by | Updater mode (`getCurrentMode`) |
|---|---|---|
| `mode` only | `up.sh user` (PMCA `CMD_SWITCH_MODE 0x30`) | `MODE_USER` 0 → `execute_ufp.sh` → `sauu` (USB firmware update, UFP) |
| `mode` + `mode1` | `up.sh` with no arguments ("Updater mode") | `MODE_PROD` 1 → `execute_prod.sh` → `mf` + `sen.elf` |
| `mode` + `mode3` / `mode2` / `mode6` / `mode5` | `up.sh serv` / ... / `up.sh ver` / `up.sh ms` | minor / service / version-skip / ms |
| `debug` (any mode) | nobody: a manual switch | `ud_cons.elf on` (enables console output, see §5) |
| `comp` (present in the normal state) | | UFP mode deletes `mode*` at start, so the **next boot is normal again** |

`up.sh` also runs `ud_send_lsi.elf <mode_num>`, which tells the other LSI. It is not proven that the real bootloader
decides on the FAT file rather than on that state **[?]**. The emulator follows the file, which is the documented
contract (NOTES §9).

Stage-1 loader (`loader.sh`, full replacement attached to the task; diff in §6):
1. Load the NAND stack and write the MUMIN hand-off (unchanged).
2. Mount `/setting` (nflasha2) read-only and stage `Backup.bin` (unchanged).
3. **If `/setting/updater/mode` exists:**
   - mount `/dev/nflasha1` (252:1) as vfat, read-only;
   - parse `boot/loadaddr.txt` into `KDST,IDST`;
   - stage `boot/vmlinux.bin` at 0x90000000 and `boot/initrd.img` at 0x90400000 (`a5kcopy -f2p`; the lengths are taken
     from its output);
   - write the descriptor `{"A5KB", 0x90000000, klen, 0x90400000, ilen, "A5KX", KDST, IDST}` at PA 0x80027000
     (`a5kcopy -w32`, magic last).
4. Otherwise: av-cam + `-stage2` as before.
5. `reboot -f` → SoC reset → stage 2.

Board (`a5k_boot_reset`, branch `updater-boot`):
- If words 5..7 carry `"A5KX"` and both destinations lie in DDR without overlapping, copy the kernel to `kdst` and the
  initrd to `idst`, and set the boot stub's entry literal to `kdst`. **No `kemco` block** is written (this kernel
  cannot parse one). The plain 5-word descriptor behaves exactly as before.
- The entry literal (stub word 20) was taken out of the `a5k.stub` ROM blob. The stub is now split into
  `a5k.stub` (words 0..19) and `a5k.stub-gicd`. `a5k_boot_reset` writes the literal on every reset. Reason: the
  handler is registered from a machine-init-done notifier, which runs **before** `rom_check_and_register_reset()`.
  `rom_reset` therefore runs after it and restores ROM blobs (the old comment "runs after the ROM reset" was wrong).
  The kernel, initrd and kemco writes were never in ROM blobs, so the normal boot was not affected.
- The secondary-CPU release mailbox at 0xC0005010..0xC000501B is cleared on every reset. Otherwise CPU1-3 leave the ROM
  wait loop immediately with the previous kernel's `cxd90014_secondary_entry` (0x8004e6e0, normal kernel), which
  points into the freshly loaded updater image. The normal stage 2 only worked because it reloads the identical kernel.
  On power-on the IRAM is zero, which is the state reproduced here. That the real boot ROM/bootloader clears the
  mailbox is a **hypothesis**.

## 4. How to run

```sh
S=<scratch>
cp -c <pristine.nand> $S/t.nand              # never the shared image / state/a5000.nand
# 1. set the flag as "up.sh user" does: rm -f /setting/updater/mode*; touch mode; sync
#    (plus "debug" to get console output, see §5); done from a stage-1-style initramfs that mounts
#    /dev/nflasha2 rw with the updater's own options (posix_attr,rw,nodiratime,noatime,shortname=mixed,sync)
# 2. build the loader from the new loader.sh (mkinitramfs.py <out> loader.sh)
A5K_LOADER=$S/loader.cpio A5K_NAND=$S/t.nand A5K_QEMU=<qemu built from branch updater-boot> \
  emulator/run.sh -display none -serial tcp:127.0.0.1:46082,server=on,wait=off -monitor tcp:127.0.0.1:46080,server,nowait
```

`A5K_DEBUG_CONSOLE` has no effect on the updater kernel (it only edits the normal kemco cmdline). Kernel messages are
not printed (see §5). They can be read from the kernel `log_buf` in RAM instead: `pmemsave 0x80000000 0x3000000` and
search for `<5>[    0.000000] Linux version`. The updater has no klog ring (`klog: klog.addr is not valid.`), so
`tools/klog.py` does not apply.

## 5. Results (firmware 1.10, updater core 41.01.28)

Serial log, UFP mode (`mode` + `debug`), about 36 s of emulated time to the USB wait:

```
Uncompressing Linux.... done, booting the kernel.
-------------------------
[VERSION] 41.01.28
-------------------------
[DETAIL] MODE=0
[UD_IM] init
[DETAIL] : exec sauu
 FixedAreaTop  : 82DF9000 ...
UdrAPI,0,UpdaterManager.cpp:560,"IN GetUpdater(1)"
...
UdrMAIN,0,USBHandler.cpp:201,"DONE usbif_initialize() - (return = 0)"
UdrMAIN,0,USBHandler.cpp:210,"DONE usbif_hndl::open() - (return = 0)"
UdrMAIN,0,USBHandler.cpp:402,"CALLBACK onEvent(CONNECTION)(0, 0x4131cf8, 0)"
UdrMAIN,0,USBHandler.cpp:403,"usbif_event_param_t.connection.val = 0"
UdrMAIN,0,USBHandler.cpp:422,"DONE onEvent(CONNECTION)"
/ # cat /version.txt
41.01.28
```

- Kernel (from `log_buf`): 4 CPUs come up. The cramfs initrd is loaded from 0x80628000. Then `nflasha0: p1 ... p23`
  (MUMIN hand-off accepted), `backup.ko: ** NO SAVE MODE **`, and DMM `unit_type=0x157`. All 15 `insmod.sh` modules
  load.
- Processes while waiting: `UdtrMain.sh`, `ud_im.elf`, `execute_ufp.sh`, `UsbPortMonitor`, `ud_timeout_poweroff.elf`,
  `sauu`, and the console `ash`.
- **Shell**: `/sbin/init` (a script) loops `/bin/ash --login` on the console, because `/root/IAMLDUDWRT` is absent.
  It is fully interactive over the TCP serial port. `/setting` is unmounted at that point (UdtrMain unmounts it).
- **Console gating**: the cmdline has `amba2.console=0`, so the Sony PL011 driver prints nothing (kernel or user)
  until `ud_cons.elf on` (`AMBAUART_IOC_SETMODE` ioctl on `/dev/console`). UdtrMain runs it in production mode or when
  `/setting/updater/debug` exists. That firmware switch is what is used here. Without it the boot is identical but
  the serial port stays silent after the decompressor message.
- Production mode (`mode` + `mode1` + `debug`): `[DETAIL] MODE=1` → `execute_prod.sh` → `mf` (pid 124) and `sen.elf`
  are running, and `/ramdsk/fifo1`, `fifo2`, `MIS.bin`, `DGU.bin` exist. It waits for the factory data source.
  The flags are kept, so every boot re-enters the updater.
- Normal boot with the same board and loader (no flag): unchanged. Stage 2 brings up 4 CPUs, and userland reaches
  im.elf (`input: D keyboard`).

NAND changes made by the runs (all in the emulated clone):
- UFP mode: `execute_ufp.sh` deletes `/setting/updater/mode*` because `comp` exists, so the next boot is normal.
  `debug` stays. Nothing else was written before the USB wait. No erase or format happened: `loader_writer.sh` and
  `pformat_*` belong to the update package (`updater.img`), which is only received over USB.
- Production mode: the flags remain. `comp` and `dat4` showed a new mtime (`Jan 1 00:12`) while `/setting` was
  mounted rw. Whether this is a write or only the `posix_attr` view is not checked **[?]**.

## 6. Files

- qemu branch `updater-boot` (worktree, one commit on top of `sony-a5000`): `hw/arm/sony_a5000.c`.
- `provision/loader.sh`: a full replacement is supplied with the task, together with a unified diff against the current
  file. It adds `/dev/nflasha1`, the flag test, and the nflasha1 staging + extended descriptor. The normal path is
  unchanged.
- `provision/mkinitramfs.py` needs no change: `a5kcopy -f2p` / `-w32` cover the new duties.

## 7. Open issues

1. **Decision source [?]**: the FAT file `/setting/updater/mode` (used here) vs the sub-LSI state set by
   `ud_send_lsi.elf`. A real-camera cold boot with the flag only (no `ud_send_lsi`) would settle it.
2. **Bootloader duties in updater mode [?]**: whether Backup.bin is copied (done here) and whether av-cam is skipped
   (skipped here). Whether the kemco block / ATAGs are written is irrelevant to this kernel.
3. **SMP mailbox clear** is a hypothesis (§3). It is harmless for the normal boot (that path was verified).
4. **USB host side**: `sauu` (UFP) and `sen.elf` (production) wait for a host on the F_USB20HDC UDC. Getting further
   (receiving `FirmwareData` and running `startupdate.sh` / `loader_writer.sh` from `updater.img`) needs a USB device
   model bridged to a host, which is not modelled. `ud_timeout_poweroff.elf` is running and presumably powers off after
   its timeout **[?]**.
5. **No kernel console**: there is no switch for `printk` output on the serial port short of the firmware's own
   `ud_cons.elf`. Kernel messages before that are only visible through `log_buf` in RAM.
6. `mem=1024M` on the 512 MiB board: harmless as long as nothing touches PA >= 0xA0000000 (everything above
   0x82D90000 is memrsv). Not exhaustively checked **[?]**.

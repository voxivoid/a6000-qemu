# Boot flow — reproducing the real bootloader's role

On every power-on, the real a5000's Sony bootloader (resident in internal RAM at 0xC0000000) mounts NAND, loads several images into RAM, then hands off to the kernel. The kernel and modules **assume** that result (verified), so the emulator must produce the same state. The bootloader itself lives in reserved flash blocks and is not in the backup, so **what the bootloader does that depends on NAND is reproduced with Sony's own NAND stack** (stage-1 loader); the rest is done by board code.

## What the bootloader leaves behind (what the firmware requires)
| Item | Location | Consumer | Evidence | How reproduced |
|---|---|---|---|---|
| cmdline | PA 0x80033000, `"kemco"+string` | setup_arch | kernel code, snapshot original | board |
| SPI target = CPU0 | GICD_ITARGETSR | Sony gic_dist_init (not set) | kernel code | board stub |
| secondary core release | 0xC0005010+ / SEV | cxd90014_smp_init | kernel code, real-device IRAM dump | board stub |
| ARM7 NAND firmware running | SRAM+0x3F40=2 | lld.ko | lld analysis | board (+ HLE) |
| NAND dev-info | SRAM+0x3F00 (13 words) | lld_probe | snapshot, real-device SRAM | board |
| bad block table / init_param | 0xC0000C00 / 0xC0001500 | nand.ko | real-device IRAM dump | board |
| **MUMIN mount state** | PA 0x80000000, 128 KiB | nand.ko `boot_mode=0` → `muminblk_init(0x40000000,0x20000,1)` | IDA | **stage-1 loader** |
| **Backup.bin copy** | header word0 (0x82EC0000) | backup.ko `Backup_load=0` | IDA, run-confirmed | **stage-1 loader** |
| **av-cam.bin** | PA 0x83541000 | liro.ko `adr=` | IDA, run-confirmed | **stage-1 loader** |
| **kernel / initrd** | 0x80038000 / 0x80428000 | — | — | **stage-1 loader → board** |
| hand-off page | 0x80021000 (boot reason +0x4C etc.) | upm.ko etc. | snapshot | board preload (`data/handoff_80021000.bin`, snapshot values — may differ from cold-boot values [?]) |

## Stages
1. **Power ON / reset (stage 1)**: board boots with host `vmlinux.bin` + `provision/loader.cpio`.
   `loader.sh` mounts the emulated NAND with unmodified Sony modules (devno/dmac/ldec/lld/nand `boot_mode=1`), then:
   - `a5kcopy -mumin 0x3f03ad20`: copies the MUMIN buffer of the nand.ko device struct (+0x34/+0x38) to PA 0x80000000, relocating self-referencing pointers to base VA 0x40000000
   - `/setting/Backup.bin` → PA from header, `/system/av-cam.bin` → 0x83541000
   - `/system/vmlinux.bin`, `initrd.img` → staged at 0x90000000 / 0x90400000 + descriptor `{"A5KB", kpa, klen, ipa, ilen}` @ 0x80027000
   - `reboot -f` → kernel writes 0x11 to SYSCTL +0x44 → SoC reset
2. **Stage 2**: board reset handler (runs after ROM reset) sees the descriptor, moves the staged kernel/initrd into place, and enters. From here **everything is stock**: kernel read from NAND → stock initrd → `/sbin/init` → …
3. If the guest reboots, there is no descriptor, so it starts again from stage 1 (like the real device, every boot goes through the bootloader).

## Caveats
- `boot_mode=1` erases the whole NAND on mount failure (same behavior as the updater). Back up the NAND file with `cp -c` (APFS clone) before experimenting.
- `0x3f03ad20` is the nand.ko address when module load order matches the stock initrd (matches real-device snapshot). Changing load order changes it.

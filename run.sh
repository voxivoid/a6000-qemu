#!/bin/bash
# Boot the official a5000 1.10 firmware in the sony-a5000 QEMU machine.
#
#   emulator/run.sh [extra qemu args...]
#
# Default ("camera" boot, like the real device):
#   power-on -> stage-1 loader (Sony NAND stack mounts the emulated NAND and
#   does the bootloader's NAND duties) -> stage 2 = stock kernel + initrd read
#   from NAND /system -> stock /sbin/init ...
# Needs emulator/state/a5000.nand (create with emulator/provision.sh).
#
# Env: A5K_NAND=<file>       NAND backing file (default state/a5000.nand)
#      A5K_DEBUG_CONSOLE=1   serial kernel console on UART0 (changes the stock
#                            cmdline: console=ttyAM0; for boot debugging only,
#                            the stock UI is not tested with it)
#      A5K_DIRECT=1          old direct boot: host kernel + stock initrd
#      A5K_SD=<img>          SD card image (FAT32, e.g. state/sd.img)
#      A5K_HANDOFF=<file>    bootloader hand-off page (default data/handoff_80021000.bin)
E="$(cd "$(dirname "$0")" && pwd)"
R="$(dirname "$E")"   # optional parent checkout (firmware/, backup/ next to this repo)
# unpacked official updater: $A5K_FW, else ./firmware, else ../firmware
FW="${A5K_FW:-$E/firmware}"; [ -d "$FW" ] || FW="$R/firmware"
F="$FW/unpacked/firmware.tar_unpacked/0700_part_image/dev/nflasha3_unpacked_unpacked"
NAND="${A5K_NAND:-$E/state/a5000.nand}"
M="sony-a5000"
# stock console=ttynull by default (A5K_STOCK_CONSOLE=1 is the old spelling)
[ "${A5K_DEBUG_CONSOLE:-0}" = 1 ] && [ "${A5K_STOCK_CONSOLE:-0}" != 1 ] && M="$M,debug-console=on"
if [ "${A5K_DIRECT:-0}" = 1 ]; then
  exec "$E/qemu/build/qemu-system-arm" -M "$M" -m 512M \
    -kernel "$F/vmlinux.bin" -initrd "$F/initrd.img" "$@"
fi
[ -f "$NAND" ] || { echo "run.sh: $NAND missing — run emulator/provision.sh first" >&2; exit 1; }
[ -f "$E/provision/loader.cpio" ] || python3 -I "$E/provision/mkinitramfs.py" "$E/provision/loader.cpio" loader.sh >/dev/null
exec "$E/qemu/build/qemu-system-arm" \
  -M "$M,submicro=on,submicro-frames=${A5K_SMFRAMES:-$E/data/submicro_frames.bin},nand=$NAND,loader=$E/provision/loader.cpio,preload=${A5K_HANDOFF:-$E/data/handoff_80021000.bin}@0x80021000" \
  -m 512M -kernel "$F/vmlinux.bin" \
  ${A5K_SD:+-drive if=sd,file=$A5K_SD,format=raw} "$@"

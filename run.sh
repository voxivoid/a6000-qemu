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
#      A5K_STOCK_CONSOLE=1   keep the stock console=ttynull (no serial log)
#      A5K_DIRECT=1          old direct boot: host kernel + stock initrd
#      A5K_SD=<img>          SD card image (FAT32, e.g. state/sd.img)
R="$(cd "$(dirname "$0")/.." && pwd)"
E="$R/emulator"
F="$R/firmware/unpacked/firmware.tar_unpacked/0700_part_image/dev/nflasha3_unpacked_unpacked"
NAND="${A5K_NAND:-$E/state/a5000.nand}"
M="sony-a5000"
[ "${A5K_STOCK_CONSOLE:-0}" = 1 ] || M="$M,debug-console=on"
if [ "${A5K_DIRECT:-0}" = 1 ]; then
  exec "$E/qemu/build/qemu-system-arm" -M "$M" -m 512M \
    -kernel "$F/vmlinux.bin" -initrd "$F/initrd.img" "$@"
fi
[ -f "$NAND" ] || { echo "run.sh: $NAND missing — run emulator/provision.sh first" >&2; exit 1; }
[ -f "$E/provision/loader.cpio" ] || python3 -I "$E/provision/mkinitramfs.py" "$E/provision/loader.cpio" loader.sh >/dev/null
exec "$E/qemu/build/qemu-system-arm" \
  -M "$M,nand=$NAND,loader=$E/provision/loader.cpio,preload=$E/data/handoff_80021000.bin@0x80021000" \
  -m 512M -kernel "$F/vmlinux.bin" \
  ${A5K_SD:+-drive if=sd,file=$A5K_SD,format=raw} "$@"

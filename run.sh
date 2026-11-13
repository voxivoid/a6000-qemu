#!/bin/bash
# Boot the official a5000 1.10 firmware in the sony-a5000 QEMU machine.
# usage: emulator/run.sh [extra qemu args...]
R="$(cd "$(dirname "$0")/.." && pwd)"
F="$R/firmware/unpacked/firmware.tar_unpacked/0700_part_image/dev/nflasha3_unpacked_unpacked"
M="sony-a5000"; [ "${A5K_STOCK_CONSOLE:-0}" = 1 ] || M="$M,debug-console=on"
exec "$R/emulator/qemu/build/qemu-system-arm" -M "$M" -m 512M \
  -kernel "$F/vmlinux.bin" -initrd "$F/initrd.img" "$@"

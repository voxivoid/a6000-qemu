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
#      A5K_SHELL=<port>      root shell on UART2 as a TCP server on
#                            127.0.0.1:<port> (-M console=ttyAM1 via
#                            shell-socket=; only the console= token of the
#                            stock cmdline changes, the UI still runs).
#                            Use tools/a5kshell.py --port <port>.
#                            docs/RE_GUEST_SHELL.md
#      A5K_DEBUG_CONSOLE=1   same console (ttyAM1, UART2) on this terminal
#                            (stdio) instead of a socket
#      A5K_DEBUG_CONSOLE=ttyAM0  old UART0 console (console=ttyAM0): early-boot
#                            logs only, av-cam owns UART0 and the UI does not
#                            start
#      A5K_DIRECT=1          old direct boot: host kernel + stock initrd
#      A5K_SD=<img>          SD card image (FAT32, e.g. state/sd.img)
#      A5K_QEMU=<binary>     qemu-system-arm to run (default qemu/build/qemu-system-arm)
#      A5K_LOADER=<cpio>     stage-1 loader initramfs (default provision/loader.cpio)
#      A5K_HANDOFF=<file>    bootloader hand-off page (default data/handoff_80021000.bin)
E="$(cd "$(dirname "$0")" && pwd)"
R="$(dirname "$E")"   # optional parent checkout (firmware/, backup/ next to this repo)
# unpacked official updater: $A5K_FW, else ./firmware, else ../firmware
FW="${A5K_FW:-$E/firmware}"; [ -d "$FW" ] || FW="$R/firmware"
F="$FW/unpacked/firmware.tar_unpacked/0700_part_image/dev/nflasha3_unpacked_unpacked"
NAND="${A5K_NAND:-$E/state/a5000.nand}"
M="sony-a5000"
QEMU="${A5K_QEMU:-$E/qemu/build/qemu-system-arm}"
LOADER="${A5K_LOADER:-$E/provision/loader.cpio}"
# stock console=ttynull by default (A5K_STOCK_CONSOLE=1 is the old spelling).
# UART0 is set to null explicitly for the UART2 shell so that QEMU's default
# serial backend never attaches to av-cam's monitor port.
SER=()
DBG="${A5K_DEBUG_CONSOLE:-0}"; [ "${A5K_STOCK_CONSOLE:-0}" = 1 ] && DBG=0
if [ -n "${A5K_SHELL:-}" ]; then
  [ "$DBG" = 0 ] || { echo "run.sh: A5K_SHELL and A5K_DEBUG_CONSOLE are exclusive" >&2; exit 1; }
  M="$M,shell-socket=127.0.0.1:$A5K_SHELL"
  SER=(-serial null)
elif [ "$DBG" = 1 ]; then
  M="$M,console=ttyAM1"
  SER=(-serial null -serial null -serial stdio)
elif [ "$DBG" = ttyAM0 ]; then
  M="$M,console=ttyAM0"
fi
if [ "${A5K_DIRECT:-0}" = 1 ]; then
  exec "$QEMU" -M "$M" -m 512M \
    -kernel "$F/vmlinux.bin" -initrd "$F/initrd.img" ${SER[@]+"${SER[@]}"} "$@"
fi
[ -f "$NAND" ] || { echo "run.sh: $NAND missing — run emulator/provision.sh first" >&2; exit 1; }
[ -n "${A5K_LOADER:-}" ] || [ -f "$LOADER" ] || python3 -I "$E/provision/mkinitramfs.py" "$E/provision/loader.cpio" loader.sh >/dev/null
exec "$QEMU" \
  -M "$M,submicro=on,submicro-frames=${A5K_SMFRAMES:-$E/data/submicro_frames.bin},nand=$NAND,loader=$LOADER,preload=${A5K_HANDOFF:-$E/data/handoff_80021000.bin}@0x80021000" \
  -m 512M -kernel "$F/vmlinux.bin" \
  ${A5K_SD:+-drive if=sd,file=$A5K_SD,format=raw} ${SER[@]+"${SER[@]}"} "$@"

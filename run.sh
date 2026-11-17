#!/bin/bash
# Boot the official ILCE-6000 3.21 firmware in the sony-a6000 QEMU machine.
#
#   ./run.sh [extra qemu args...]
#
# Default ("camera" boot, like the real device):
#   power-on -> stage-1 loader (Sony NAND stack mounts the emulated NAND and
#   does the bootloader's NAND duties) -> stage 2 = stock kernel + initrd read
#   from NAND /system -> stock /sbin/init ...
# Needs state/a6000.nand (create with ./provision.sh <your NAND dump>).
#
# Env: A6K_NAND=<file>       NAND backing file (default state/a6000.nand)
#      A6K_SHELL=<port>      root shell on UART2 as a TCP server on
#                            127.0.0.1:<port> (-M console=ttyAM1 via
#                            shell-socket=; only the console= token of the
#                            stock cmdline changes, the UI still runs).
#                            Use tools/a6kshell.py --port <port>.
#                            docs/reference/RE_GUEST_SHELL.md
#      A6K_DEBUG_CONSOLE=1   same console (ttyAM1, UART2) on this terminal
#                            (stdio) instead of a socket
#      A6K_DEBUG_CONSOLE=ttyAM0  old UART0 console (console=ttyAM0): early-boot
#                            logs only, av-cam owns UART0 and the UI does not
#                            start
#      A6K_DIRECT=1          old direct boot: host kernel + stock initrd
#      A6K_SD=<img>          SD card image (FAT32, e.g. state/sd.img)
#      A6K_QEMU=<binary>     qemu-system-arm to run (default qemu/build/qemu-system-arm)
#      A6K_LOADER=<cpio>     stage-1 loader initramfs (default provision/loader.cpio)
#      A6K_HANDOFF=<file>    bootloader hand-off page (default data/handoff_80021000.bin)
#      A6K_FW=<dir>          unpacked official 3.21 updater (default ./firmware)
E="$(cd "$(dirname "$0")" && pwd)"
R="$(dirname "$E")"   # optional parent checkout (firmware/, backup/ next to this repo)
# unpacked official updater: $A6K_FW, else ./firmware, else ../firmware
M=sony-a6000 RAM=1G DATA="$E/data"
FW="${A6K_FW:-$E/firmware}"; [ -d "$FW" ] || FW="$R/firmware"
F="$FW/unpacked/firmware.tar_unpacked/0700_part_image/dev/nflasha3_unpacked_unpacked"
NAND="${A6K_NAND:-$E/state/a6000.nand}"
QEMU="${A6K_QEMU:-$E/qemu/build/qemu-system-arm}"
LOADER="${A6K_LOADER:-$E/provision/loader.cpio}"
# stock console=ttynull by default (A6K_STOCK_CONSOLE=1 is the old spelling).
# UART0 is set to null explicitly for the UART2 shell so that QEMU's default
# serial backend never attaches to av-cam's monitor port.
SER=()
DBG="${A6K_DEBUG_CONSOLE:-0}"; [ "${A6K_STOCK_CONSOLE:-0}" = 1 ] && DBG=0
if [ -n "${A6K_SHELL:-}" ]; then
  [ "$DBG" = 0 ] || { echo "run.sh: A6K_SHELL and A6K_DEBUG_CONSOLE are exclusive" >&2; exit 1; }
  M="$M,shell-socket=127.0.0.1:$A6K_SHELL"
  SER=(-serial null)
elif [ "$DBG" = 1 ]; then
  M="$M,console=ttyAM1"
  SER=(-serial null -serial null -serial stdio)
elif [ "$DBG" = ttyAM0 ]; then
  M="$M,console=ttyAM0"
fi
if [ "${A6K_DIRECT:-0}" = 1 ]; then
  exec "$QEMU" -M "$M" -m $RAM \
    -kernel "$F/vmlinux.bin" -initrd "$F/initrd.img" ${SER[@]+"${SER[@]}"} "$@"
fi
[ -f "$NAND" ] || { echo "run.sh: $NAND missing — run ./provision.sh <your NAND dump> first" >&2; exit 1; }
# (re)build the loader from this model's firmware when missing or older than its sources
if [ -z "${A6K_LOADER:-}" ] && { [ ! -f "$LOADER" ] || [ "$E/provision/loader.sh" -nt "$LOADER" ] ||
     [ "$E/provision/mkinitramfs.py" -nt "$LOADER" ] || [ "$E/provision/a6kcopy" -nt "$LOADER" ]; }; then
  A6K_FW="$FW" python3 -I "$E/provision/mkinitramfs.py" "$LOADER" loader.sh >/dev/null || exit 1
fi
exec "$QEMU" \
  -M "$M,submicro=on,submicro-frames=${A6K_SMFRAMES:-$DATA/submicro_frames.bin},nand=$NAND,loader=$LOADER,preload=${A6K_HANDOFF:-$DATA/handoff_80021000.bin}@0x80021000" \
  -m $RAM -kernel "$F/vmlinux.bin" \
  ${A6K_SD:+-drive if=sd,file=$A6K_SD,format=raw} ${SER[@]+"${SER[@]}"} "$@"

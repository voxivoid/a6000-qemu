#!/bin/sh
# pp_inject.sh - push pphook + pploader to the guest RAM disk and inject into
# im.elf, RAM-only (no flash write).  Emulator by default (A6K_SHELL port), or
# a real camera over its telnet shell with A6K_SHELL_HOST/PORT.
#
#   A6K_SHELL=46310 tools/pp_inject.sh            # emulator
#   tools/pp_inject.sh --port 46310               # same, explicit
#
# After a power cycle the tmpfs is wiped, so re-run this once per boot.  To
# undo without rebooting there is nothing to delete from flash; a battery pull
# (or killing im.elf, which init respawns) returns to stock.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
SH="python3 $HERE/a6kshell.py $*"
SO="$ROOT/tools/pphook/pphook.so"
LD="$ROOT/provision/pploader"
DLOPEN_OFF=0xc37cd            # __libc_dlopen_mode in libc-2.11.2.so (Thumb)

[ -f "$SO" ] || { echo "missing $SO (build: see tools/pphook/pphook.c)"; exit 1; }
[ -f "$LD" ] || { echo "missing $LD (build: see provision/pploader.c)"; exit 1; }

$SH --force push "$SO" /android/pp.so  | tail -1
$SH --force push "$LD" /android/pploader | tail -1
$SH run 'mkdir -p /android/pp; chmod 777 /android/pp; chmod 755 /android/pploader /android/pp.so;
         P=$(pidof im.elf); [ -n "$P" ] || { echo "im.elf not running"; exit 1; };
         echo "inject into pid $P"; /android/pploader $P '"$DLOPEN_OFF"' /android/pp.so'
echo "---"
echo "command file: /android/pp/cmd.txt   output: /android/pp/out.txt   (both tmpfs)"

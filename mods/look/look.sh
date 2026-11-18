#!/bin/bash
# look.sh - apply / revert a custom ISP gamma on a real A6000 (FW 3.21), RAM-only, no polling.
#
#   look.sh apply TABLE.bin     write TABLE (96 x u32) to the ISP gamma SRAM and patch the camera's own
#                               table writer to `bx lr` so nothing overwrites it (still + video)
#   look.sh revert              restore the writer; the camera re-programs its own curve at its next event
#   look.sh status
#
# Env: CAM=<camera ip> (OpenMemories-Tweak: Wifi + Telnet + ADB on).  Needs: adb, python3, mods/look/gammamod built.
# Why this works / what each address is: docs/A6000_LOOK_MOD_ISP.md.  RAM-only: battery pull = stock.
set -euo pipefail
CAM=${CAM:?set CAM=<camera ip>}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SHELLPY="python3 $ROOT/tools/a6kshell.py --host $CAM --port 23"
WRITER_PA=0x83b67414                 # av-cam VA 0x43b67414: copies 96 words to 0xF3215300 (3.21 only!)
ORIG8=00f5a8304ff4c072               # expected original bytes there (add.w r0,r0,#0x15000; mov.w r2,#0x180)
PATCH4=7047a830                      # bx lr + untouched 2nd halfword (one aligned 32-bit store)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT

cam() { $SHELLPY run "$1" 2>&1; }
hex2bin() { python3 -I -c "import sys;sys.stdout.buffer.write(bytes.fromhex('$1'))" > "$2"; }
connect() { adb connect "$CAM:5555" >/dev/null 2>&1 || true; }
push() { adb push "$1" "/$2" >/dev/null 2>&1; }          # adb's / is the camera shell's /android

writer_state() {      # prints: original | patched | UNKNOWN
  cam "/android/gammamod read $WRITER_PA 8 /android/w8.bin >/dev/null; \
       if busybox cmp /android/w8.bin /android/fn_orig8.bin >/dev/null 2>&1; then echo original; \
       elif busybox cmp /android/w8.bin /android/fn_patched8.bin >/dev/null 2>&1; then echo patched; \
       else echo UNKNOWN; fi" | tail -1
}

prep() {
  connect
  push "$HERE/gammamod" gammamod
  hex2bin "$ORIG8" "$T/o8"; hex2bin "${PATCH4}${ORIG8:8}" "$T/p8"
  hex2bin "${ORIG8:0:8}" "$T/o4"; hex2bin "$PATCH4" "$T/p4"
  push "$T/o8" fn_orig8.bin; push "$T/p8" fn_patched8.bin; push "$T/o4" fn_orig4.bin; push "$T/p4" fn_patch4.bin
  cam "chmod 755 /android/gammamod"
}

case "${1:-}" in
  status)
    prep
    echo "writer: $(writer_state)"; cam "/android/gammamod hwrd 0xF3215300 4"
    ;;
  apply)
    TBL=${2:?table file (96 x u32)}; [ "$(stat -c %s "$TBL")" = 384 ] || { echo "table must be 384 bytes"; exit 2; }
    prep
    st=$(writer_state); echo "writer: $st"
    [ "$st" = UNKNOWN ] && { echo "writer bytes differ from FW 3.21: refusing to patch"; exit 3; }
    push "$TBL" look_tbl.bin
    cam "/android/gammamod hwdump 0xF3215300 96 /android/hw_before.bin >/dev/null; /android/gammamod hwwr 0xF3215300 /android/look_tbl.bin"
    [ "$st" = original ] && cam "/android/gammamod write $WRITER_PA /android/fn_patch4.bin"
    echo "writer now: $(writer_state)"
    cam "/android/gammamod hwdump 0xF3215300 96 /android/chk.bin >/dev/null; busybox cmp /android/chk.bin /android/look_tbl.bin && echo 'look applied (SRAM == table)'"
    ;;
  revert)
    prep
    [ "$(writer_state)" = patched ] && cam "/android/gammamod write $WRITER_PA /android/fn_orig4.bin"
    echo "writer now: $(writer_state)"
    echo "reverted: the camera re-programs its own curve at its next event (mode change, shutter, video, style change)."
    ;;
  *) sed -n 2,12p "$0"; exit 1 ;;
esac

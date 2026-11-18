#!/bin/sh
# a6300_avcam.sh - unpack the a6300 updater and diff its av-cam gamma evidence
# against the A6000's, to decide if Picture Profiles are feasible on the A6000.
# Nothing here needs a camera: av-cam.bin comes from Sony's public updater.
#
#   1. Download Update_ILCE6300V201.exe from Sony's ILCE-6300 support page
#      (same flow as the A6000 updater), put it in firmware_a6300/.
#   2. tools/a6300_avcam.sh
#
# Mirrors data/README.md's exe->.dat->unpack method, then runs avcam_gamma.py.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FWTOOL=${FWTOOL:-$ROOT/../fwtool.py}
DIR=$ROOT/firmware_a6300
EXE=$(ls "$DIR"/Update_ILCE6300V201.exe "$DIR"/*.exe 2>/dev/null | head -1)
[ -n "$EXE" ] || { echo "put Update_ILCE6300V201.exe in $DIR/"; exit 1; }
[ -x "$FWTOOL/.venv/bin/python" ] || { echo "need fwtool.py venv at $FWTOOL (see data/README.md step 1)"; exit 1; }

echo "exe: $EXE"
# cut the FirmwareData .dat out of the exe (offset/size are not fixed per model)
"$FWTOOL/.venv/bin/python" - "$EXE" "$DIR/fw.dat" <<'PY'
import struct, sys, re
exe, out = sys.argv[1], sys.argv[2]
d = open(exe, 'rb').read()
off = d.index(b'\x89UFU\r\n\x1a\n')                      # DAT magic
# size = u32 just before the UTF-16LE .dat name in the archive table
m = re.search(rb'F\x00i\x00r\x00m\x00w\x00a\x00r\x00e\x00D\x00a\x00t\x00a\x00[^\x00]*\.\x00d\x00a\x00t\x00', d)
name = m.group(0)
size = struct.unpack('<I', d[d.index(name) - 4:d.index(name)])[0]
open(out, 'wb').write(d[off:off + size])
print("  .dat: %d bytes at exe offset %d" % (size, off))
PY
"$FWTOOL/.venv/bin/python" "$FWTOOL/fwtool.py" unpack -f "$DIR/fw.dat" -o "$DIR/unpacked"

AVCAM=$(find "$DIR/unpacked" -name 'av-cam.bin' | head -1)
A6000=$(find "$ROOT/firmware/unpacked" -name 'av-cam.bin' | head -1)
[ -n "$AVCAM" ] || { echo "av-cam.bin not found in a6300 unpack"; exit 1; }
echo "=========================================================="
echo "a6300 av-cam: $AVCAM"
echo "A6000 av-cam: $A6000"
echo "=========================================================="
python3 "$ROOT/tools/avcam_gamma.py" "$A6000" "$AVCAM"

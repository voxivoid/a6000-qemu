#!/bin/sh
# On-camera PA-mapping check. READ-ONLY. Run BEFORE any gammamod write/hold.
# Confirms av-cam is loaded at PA 0x83541000 on THIS unit by comparing DDR
# against the av-cam.bin file. Only a match at several offsets is trusted.
#   usage: sh check_map.sh [/system/av-cam.bin] [/android/gammamod]
AV=${1:-/system/av-cam.bin}
GM=${2:-/android/gammamod}
BASE=0x83541000
T=/android/chk.$$
[ -f "$AV" ] || { echo "FAIL: $AV not found (pass path as arg 1)"; exit 2; }
[ -x "$GM" ] || { echo "FAIL: $GM not executable"; exit 2; }
SZ=$(busybox wc -c < "$AV")
ok=0; bad=0
# sample 4 KB at several file offsets (all within the file)
for pair in 0:0x83541000 4096:0x83542000 65536:0x83551000 262144:0x83581000 1048576:0x83641000; do
  off=${pair%%:*}; pa=${pair##*:}
  [ $((off + 4096)) -le "$SZ" ] || continue
  "$GM" read "$pa" 0x1000 "$T" >/dev/null 2>&1 || { echo "read $pa failed"; bad=$((bad+1)); continue; }
  busybox dd if="$AV" bs=1 skip=$off count=4096 2>/dev/null | busybox cmp -s - "$T" \
    && { echo "OK   off=$off pa=$pa"; ok=$((ok+1)); } \
    || { echo "DIFF off=$off pa=$pa"; bad=$((bad+1)); }
done
busybox rm -f "$T"
if [ "$ok" -ge 3 ] && [ "$bad" -eq 0 ]; then
  echo "MAP CONFIRMED ($ok/$ok): safe to proceed to scan"; exit 0
fi
echo "MAP NOT CONFIRMED (ok=$ok bad=$bad): DO NOT WRITE. Reads only."; exit 1

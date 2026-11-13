#!/bin/bash
# One-time factory-style NAND programming: blank NAND -> Sony MUMIN format
# (nand.ko boot_mode=1) -> backup/NAND.BIN written through /dev/nflasha.
# Result: emulator/state/a5000.nand
set -e
R="$(cd "$(dirname "$0")/.." && pwd)"; E="$R/emulator"
F="$R/firmware/unpacked/firmware.tar_unpacked/0700_part_image/dev/nflasha3_unpacked_unpacked"
IMG="${1:-$R/backup/NAND.BIN}"; OUT="$E/state/a5000.nand"
mkdir -p "$E/state"; rm -f "$OUT.tmp"
python3 -I "$E/provision/mkinitramfs.py" "$E/provision/prov.cpio" init.sh
LOG="$E/state/provision.log"
"$E/qemu/build/qemu-system-arm" -M "sony-a5000,debug-console=on,nand=$OUT.tmp,provision=$IMG" \
  -m 512M -kernel "$F/vmlinux.bin" -initrd "$E/provision/prov.cpio" \
  -display none -serial file:"$LOG" -monitor none &
QP=$!
for i in $(seq 1 240); do
  sleep 5
  grep -q "a5k-provision: OK" "$LOG" && break
  grep -qE "a5k-provision: .*(missing|FAIL)" "$LOG" && { kill $QP; echo "provision failed, see $LOG"; exit 1; }
done
kill $QP; wait $QP 2>/dev/null || true
grep -q "a5k-provision: OK" "$LOG" || { echo "provision timed out, see $LOG"; exit 1; }
mv "$OUT.tmp" "$OUT"; echo "wrote $OUT"

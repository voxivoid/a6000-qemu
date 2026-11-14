#!/bin/bash
# Export the committed state of the local QEMU work tree (qemu/, branch
# sony-a5000) into this repository:
#   qemu-overlay/   every file we ADDED to QEMU, at its QEMU path
#   patches/qemu-v10.1.0-sony-a5000.patch
#                   only the edits to EXISTING upstream files (build glue,
#                   Cortex-A5, A9MPCore, L2C-310)
# Run after every `git -C qemu commit`.  setup-qemu.sh does the reverse.
set -euo pipefail
E="$(cd "$(dirname "$0")/.." && pwd)"
Q="$E/qemu"
BASE=v10.1.0
OV="$E/qemu-overlay"

rm -rf "${OV:?}"
git -C "$Q" diff --diff-filter=A --name-only -z "$BASE" HEAD | while IFS= read -r -d '' f; do
    mkdir -p "$OV/$(dirname "$f")"
    git -C "$Q" show "HEAD:$f" > "$OV/$f"
done
git -C "$Q" diff --diff-filter=M "$BASE" HEAD > "$E/patches/qemu-v10.1.0-sony-a5000.patch"
if [ -n "$(git -C "$Q" diff --name-only --diff-filter=DRC "$BASE" HEAD)" ]; then
    echo "export-qemu: deleted/renamed upstream files are not supported" >&2
    exit 1
fi
echo "exported $(find "$OV" -type f | wc -l | tr -d ' ') overlay files," \
     "$(grep -c '^diff --git' "$E/patches/qemu-v10.1.0-sony-a5000.patch") patched upstream files" \
     "(qemu HEAD $(git -C "$Q" rev-parse --short HEAD))"

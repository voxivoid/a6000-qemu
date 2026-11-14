#!/bin/bash
# Create qemu/ = upstream QEMU v10.1.0 + the a5000 machine:
#   1. clone the v10.1.0 tag (skipped if qemu/ already exists)
#   2. copy qemu-overlay/ (all files we added) into it
#   3. apply patches/qemu-v10.1.0-sony-a5000.patch (edits to upstream files)
# Then build:  (cd qemu/build && ../configure ... && ninja qemu-system-arm)
set -euo pipefail
E="$(cd "$(dirname "$0")" && pwd)"
Q="$E/qemu"
if [ ! -d "$Q" ]; then
    git clone --branch v10.1.0 --depth 1 https://gitlab.com/qemu-project/qemu.git "$Q"
fi
cp -R "$E/qemu-overlay/." "$Q/"
git -C "$Q" apply "$E/patches/qemu-v10.1.0-sony-a5000.patch"
mkdir -p "$Q/build"
echo "qemu/ ready; build with: (cd qemu/build && ../configure --target-list=arm-softmmu ... && ninja qemu-system-arm)"

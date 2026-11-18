#!/usr/bin/env python3
"""avcam_luts.py - find gamma / tone-curve LUT tables in an av-cam.bin.

For the custom-look mod (override the A6000's base develop gamma/matrix,
arri_isp-style): locate monotone ramp tables that look like gamma/tone LUTs so
the active output-gamma curve can be pinned and later overridden via /dev/mem.
Static only; no camera, no NAND.

    tools/avcam_luts.py <av-cam.bin> [min_points]

A hit = a run of >=min_points (default 96) monotone-nondecreasing u16 values
that starts low (<=64) and rises past 1024 - the shape of a gamma/tone LUT.
File offsets are not runtime PAs; they pin which table, to be mapped to its
load address separately (av-cam loads at PA 0x83541000; retained curve banks
may live elsewhere).
"""
import struct
import sys


def scan(d, minpts):
    n = len(d)
    res = []
    i = 0
    while i < n - 2 * minpts:
        prev, cnt, j = -1, 0, i
        first = struct.unpack_from('<H', d, i)[0]
        while j < n - 2:
            v = struct.unpack_from('<H', d, j)[0]
            if v >= prev:
                prev, cnt, j = v, cnt + 1, j + 2
            else:
                break
        if cnt >= minpts:
            last = struct.unpack_from('<H', d, i + (cnt - 1) * 2)[0]
            distinct = len(set(struct.unpack_from('<8H', d, i)))
            if first <= 64 and last >= 1024 and last - first >= 1000 and distinct > 3:
                res.append((i, cnt, first, last))
            i += cnt * 2
        else:
            i += 2
    return res


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    d = open(sys.argv[1], 'rb').read()
    minpts = int(sys.argv[2]) if len(sys.argv) > 2 else 96
    r = scan(d, minpts)
    print("%s: %d gamma/tone-LUT candidates (>=%d monotone u16, 0->1k+)"
          % (sys.argv[1], len(r), minpts))
    for off, cnt, a, b in r:
        print("  file 0x%06x  len %4d  %5d..%5d" % (off, cnt, a, b))


if __name__ == "__main__":
    main()

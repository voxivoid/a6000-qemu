#!/usr/bin/env python3
"""make_curve.py - build a custom tone curve in the A6000 av-cam LUT format.

Reads an original LUT dumped by `gammamod read` (little-endian u16, monotone,
output 0..max) and writes a same-shape LUT with a look applied, so it can be
written back with `gammamod write/hold`.  Keeps the point count, the endpoints
and max of the original, and stays monotone.

    make_curve.py <orig.bin> <out.bin> <look> [amount]

looks:
    identity   copy (sanity check: writing it must change nothing)
    flat       lift shadows + lower contrast (log-ish test look)   amount 0..1
    contrast   S-curve (deeper blacks, brighter highs)             amount 0..1
    invert     reversed curve (dramatic, unmistakable on-camera)

The input axis is assumed linear in [0,1] across the points; the output is
remapped through the look then rescaled to the original's [first,last] span.
Validate on a camera you own; RAM-only, revert by stopping gammamod.
"""
import struct
import sys


def load(path):
    d = open(path, "rb").read()
    return list(struct.unpack("<%dH" % (len(d) // 2), d))


def save(path, v):
    open(path, "wb").write(struct.pack("<%dH" % len(v), *v))


def clampmono(v, lo, hi):
    out = []
    prev = lo
    for x in v:
        x = max(lo, min(hi, int(round(x))))
        if x < prev:
            x = prev
        out.append(x)
        prev = x
    out[0], out[-1] = lo, hi
    return out


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    orig = load(sys.argv[1])
    out, look = sys.argv[2], sys.argv[3]
    amt = float(sys.argv[4]) if len(sys.argv) > 4 else 0.5
    n = len(orig)
    lo, hi = orig[0], orig[-1]
    span = hi - lo or 1

    res = []
    for i in range(n):
        t = i / (n - 1)                     # input position 0..1
        y = (orig[i] - lo) / span            # original normalized output
        if look == "identity":
            ny = y
        elif look == "flat":
            # pull toward a lifted, lower-contrast line: lift blacks, compress
            lift = 0.12 * amt
            ny = lift + (1 - lift) * (y ** (1 - 0.45 * amt))
        elif look == "contrast":
            # smoothstep S-curve blended by amount
            s = y * y * (3 - 2 * y)
            ny = y + (s - y) * amt
        elif look == "invert":
            ny = 1 - y
        else:
            sys.exit("unknown look: " + look)
        res.append(lo + ny * span)

    res = clampmono(res, lo, hi)
    if look == "invert":                     # invert isn't monotone-increasing
        res = [max(lo, min(hi, int(round(lo + (1 - (orig[i] - lo) / span) * span))))
               for i in range(n)]
    save(out, res)
    print("%s look='%s' amt=%s  %d pts  %d..%d -> %d..%d"
          % (out, look, amt, n, lo, hi, res[0], res[-1]))
    print("  sample:", [res[i] for i in range(0, n, max(1, n // 8))])


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""make_hwgamma.py - build a 96 x u32 table for the A6000 ISP gamma SRAM (PA 0xF3215300).

The hardware table holds the OUTPUT (0..1023) at 96 fixed input knots (11-bit linear, 0..2047, log-like
spacing). See docs/A6000_LOOK_MOD_ISP.md. This evaluates a transfer function at each knot.

    make_hwgamma.py OUT.bin LOOK [--grey-x N] [--range legal|full] [--amt A]

LOOK:
  slog2     Sony S-Log2 (10-bit code = 64 + 876*y legal, or y*1023 with --range full)
  slog3     Sony S-Log3
  logc3     ARRI LogC3 EI 800 (full range, the curve of the a6400 "ARRI Wide Gamut" preset; grey 18% -> 0.391)
  arri709   APPROXIMATE ARRI-Classic-like Rec.709 view (filmic shoulder, gamma 2.4); not ARRI's LUT
  rec709    ITU-R BT.709 OETF (a "flat-ish" sanity reference)
  invert    1023 - stock-like gamma curve is NOT built here; use make_curve.py/invert on the RAM curve
  identity  output = knot*1023/2047 (linear passthrough, very dark: sanity test)

--grey-x N  knot position where scene 18% grey lands at the exposure you shoot (default 76: where the stock
            curve puts ~46% output). Scene reflectance R = 0.18 * knot / N. Larger N = more headroom
            above grey in the curve (darker, flatter); the SENSOR clip (knot 2047) is fixed.
Prints a preview; writes 96 little-endian u32.
"""
import argparse
import math
import struct

KNOTS = ([2 * i for i in range(1, 33)] + list(range(68, 129, 4)) + list(range(136, 257, 8)) +
         list(range(272, 513, 16)) + list(range(576, 1025, 64)) + list(range(1152, 1921, 128)) + [2047])
assert len(KNOTS) == 96


def slog2(r):
    """Sony S-Log2: y in 0..~1.06 for reflectance r (0.18 grey, 0.90 white). x = r/0.9."""
    x = r / 0.9
    return 0.432699 * math.log10(155.0 * x / 219.9 + 0.037584) + 0.616596 + 0.03


def slog3(r):
    if r >= 0.01125:
        return (420.0 + math.log10((r + 0.01) / (0.18 + 0.01)) * 261.5) / 1023.0
    return (r * (171.2102946929 - 95.0) / 0.01125 + 95.0) / 1023.0


def logc3(r):
    """ARRI LogC3 EI800 (published ALEXA constants), r = scene linear (0.18 grey)."""
    if r > 0.010591:
        return 0.247190 * math.log10(5.555556 * r + 0.052272) + 0.385537
    return 5.367655 * r + 0.092809


def arri709(r):
    """Approximation only: ACES-fit filmic tone map (grey 0.18 -> ~0.40 code) then gamma 2.4 encode."""
    r *= 0.5
    L = r * (2.51 * r + 0.03) / (r * (2.43 * r + 0.59) + 0.14)
    return max(0.0, min(1.0, L)) ** (1 / 2.4)


def rec709(r):
    r = max(0.0, min(1.0, r))
    return 4.5 * r if r < 0.018 else 1.099 * r ** 0.45 - 0.099


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("look", choices=["slog2", "slog3", "logc3", "arri709", "rec709", "identity"])
    ap.add_argument("--grey-x", type=float, default=76.0)
    ap.add_argument("--range", choices=["legal", "full"], default="legal")
    a = ap.parse_args()

    out = []
    for k in KNOTS:
        r = 0.18 * k / a.grey_x
        if a.look == "slog2":
            y = slog2(r)
            code = (64 + 876 * y) if a.range == "legal" else y * 1023
        elif a.look == "slog3":
            y = slog3(r)
            code = (64 + 876 * y) if a.range == "legal" else y * 1023
        elif a.look == "logc3":
            code = logc3(r) * 1023
        elif a.look == "arri709":
            code = arri709(r) * 1023
        elif a.look == "rec709":
            code = rec709(r / 5.0) * 1023           # headroom 5x
        else:
            code = k * 1023 / 2047
        out.append(max(0, min(1023, int(round(code)))))
    # keep it monotone non-decreasing (the stock table has outliers; a clean curve is safer)
    for i in range(1, 96):
        out[i] = max(out[i], out[i - 1])
    open(a.out, "wb").write(struct.pack("<96I", *out))
    print("wrote %s (%s, grey-x %g, %s)" % (a.out, a.look, a.grey_x, a.range))
    for i in (0, 8, 16, 31, 40, 55, 70, 80, 87, 95):
        print("  knot %4d (R=%.3f) -> %4d (%.1f%%)" % (KNOTS[i], 0.18 * KNOTS[i] / a.grey_x, out[i], out[i] / 10.23))


if __name__ == "__main__":
    main()

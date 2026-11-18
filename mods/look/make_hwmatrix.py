#!/usr/bin/env python3
"""make_hwmatrix.py - build the 9 x u32 colour matrix for the A6000 ISP CC block (PA 0xF321550C).

The window holds a 3x3 signed Q10 matrix (one s16 per u32 word, row-major) between sensor RGB and the gamma;
stock rows sum to 1024 (white preserved). We left-multiply the camera's CURRENT stock matrix, so
the sensor->Rec.709-linear step already done by Sony is kept:

    make_hwmatrix.py OUT.bin STOCK.bin awg3          ARRI Wide Gamut 3 primaries (use with the logc3 gamma)
    make_hwmatrix.py OUT.bin STOCK.bin sat S         saturation S (1.0 = stock, 0 = mono) around Rec.709 luma
    make_hwmatrix.py OUT.bin STOCK.bin stock         copy of stock

STOCK.bin = `gammamod -F hwdump 0xF321550C 9 STOCK.bin` read from the camera. Output rows are renormalised to sum 1024.
"""
import struct
import sys

import numpy as np


def rgb2xyz(prim, white):
    (xr, yr), (xg, yg), (xb, yb) = prim
    P = np.array([[xr / yr, xg / yg, xb / yb], [1, 1, 1],
                  [(1 - xr - yr) / yr, (1 - xg - yg) / yg, (1 - xb - yb) / yb]])
    w = np.array([white[0] / white[1], 1, (1 - white[0] - white[1]) / white[1]])
    return P @ np.diag(np.linalg.solve(P, w))


D65 = (0.3127, 0.3290)
M709 = rgb2xyz([(0.64, 0.33), (0.30, 0.60), (0.15, 0.06)], D65)
MAWG = rgb2xyz([(0.6840, 0.3130), (0.2210, 0.8480), (0.0861, -0.1020)], D65)   # ARRI Wide Gamut 3


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    out, stock_f, mode = sys.argv[1:4]
    w = struct.unpack("<9I", open(stock_f, "rb").read(36))
    S = np.array([x - 65536 if x & 0x8000 else x for x in w], float).reshape(3, 3) / 1024.0
    if mode == "awg3":
        T = np.linalg.inv(MAWG) @ M709
    elif mode == "sat":
        s = float(sys.argv[4])
        Y = M709[1]
        T = s * np.eye(3) + (1 - s) * np.outer(np.ones(3), Y)
    elif mode == "stock":
        T = np.eye(3)
    else:
        sys.exit(__doc__)
    M = np.round(T @ S * 1024).astype(int)
    for i in range(3):                       # exact row sum 1024 (neutral stays neutral)
        M[i, i] += 1024 - M[i].sum()
    print("stock:\n", np.round(S * 1024).astype(int), "\nnew:\n", M)
    open(out, "wb").write(struct.pack("<9I", *[int(x) & 0xFFFF for x in M.flatten()]))


if __name__ == "__main__":
    main()

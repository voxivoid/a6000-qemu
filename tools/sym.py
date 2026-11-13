#!/usr/bin/env python3
"""Resolve kernel addresses: sym.py 0x4004f3e4 [...]"""
import sys, bisect, os
f = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../re/kernel_syms.txt')
syms = sorted((int(l.split()[0], 16), l.split()[2]) for l in open(f) if len(l.split()) == 3)
ad = [a for a, _ in syms]
def sym(a):
    i = bisect.bisect_right(ad, a) - 1
    return '%s+0x%x' % (syms[i][1], a - syms[i][0]) if i >= 0 else '?'
if __name__ == '__main__':
    for x in sys.argv[1:]: print(x, sym(int(x, 16)))

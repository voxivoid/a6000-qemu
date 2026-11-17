#!/usr/bin/env python3
"""Resolve kernel addresses: sym.py 0x4004f3e4 [...]

Symbol file: $A6K_SYMS, else docs/reference/kernel_syms.txt.  The bundled file
belongs to the reference kernel build (docs/reference/README.md), NOT to the
A6000 3.21 kernel: generate a 3.21 list (e.g. /proc/kallsyms from the guest
shell, as root) and point A6K_SYMS at it before trusting the names."""
import sys, bisect, os
f = os.environ.get('A6K_SYMS') or os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                               '../docs/reference/kernel_syms.txt')
syms = sorted((int(l.split()[0], 16), l.split()[2]) for l in open(f) if len(l.split()) == 3)
ad = [a for a, _ in syms]
def sym(a):
    i = bisect.bisect_right(ad, a) - 1
    return '%s+0x%x' % (syms[i][1], a - syms[i][0]) if i >= 0 else '?'
if __name__ == '__main__':
    for x in sys.argv[1:]: print(x, sym(int(x, 16)))

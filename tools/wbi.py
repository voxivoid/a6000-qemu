#!/usr/bin/env python3
"""Read words from the real-camera WBI RAM snapshot by physical or kernel VA.
usage: wbi.py <addr> [nwords]   (addr < 0x80000000 is treated as kernel VA)"""
import sys, struct, os
_E = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# WBI snapshot of your own camera: $A6K_WBI, else <repo>/data/wbi, else ../re/wbi
R = os.environ.get('A6K_WBI') or next((d for d in (os.path.join(_E, 'data/wbi'),
        os.path.join(os.path.dirname(_E), 're/wbi')) if os.path.isdir(d)), 'data/wbi')
secs = [tuple(int(x, 16) for x in l.split()) for l in open(os.path.join(R, 'map.tsv'))]
ram = open(os.path.join(R, 'ram.bin'), 'rb').read()
def rd(addr, n=1):
    for off, pa, va, ln in secs:
        for base in (pa, va):
            if base <= addr < base + ln:
                o = off + addr - base
                return struct.unpack_from('<%dI' % n, ram, o)
    return None
if __name__ == '__main__':
    a = int(sys.argv[1], 16); n = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    v = rd(a, n)
    if v is None and len(sys.argv) > 3 and sys.argv[3] == "-v": v = rdv(a, n)
    print('not in snapshot' if v is None else ' '.join('%08x' % x for x in v))

def walk(va, ttb=0x80034000):
    """ARMv7 short-descriptor walk through the snapshot's page tables."""
    l1 = rd(ttb + (va >> 20) * 4)
    if l1 is None: return None
    l1 = l1[0]
    if l1 & 3 == 2:                       # section
        return (l1 & 0xfff00000) | (va & 0xfffff)
    if l1 & 3 != 1: return None
    l2 = rd((l1 & 0xfffffc00) + ((va >> 12) & 0xff) * 4)
    if l2 is None: return None
    l2 = l2[0]
    if l2 & 2: return (l2 & 0xfffff000) | (va & 0xfff)
    if l2 & 3 == 1: return (l2 & 0xffff0000) | (va & 0xffff)
    return None

def rdv(va, n=1):
    pa = walk(va)
    return None if pa is None else rd(pa, n)

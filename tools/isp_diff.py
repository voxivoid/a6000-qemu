#!/usr/bin/env python3
"""isp_diff.py A.txt B.txt [--ignore-tables]  - list CC registers (offset from 0xF3200000) that differ between two isp_snap.sh snapshots.
Tables (gamma 0x15300-0x1547f, 3D LUT 0x17000+, 0x16000-0x16dff) are summarised as ranges when >8 words change."""
import re, sys
ABS = '--abs' in sys.argv
def load(p):
    d = {}
    for l in open(p):
        m = re.match(r'0x([0-9a-f]+): (\d+)', l)
        if m: d[int(m.group(1), 16) - (0 if ABS else 0xF3200000)] = int(m.group(2))
    return d
ABS = '--abs' in sys.argv
args = [x for x in sys.argv[1:] if not x.startswith('--')]
a, b = load(args[0]), load(args[1])
diff = sorted(k for k in a if k in b and a[k] != b[k])
print(len(diff), 'registers differ')
i = 0
while i < len(diff):
    j = i
    while j + 1 < len(diff) and diff[j + 1] - diff[j] <= 4: j += 1
    if j - i >= 8:
        print('%#x..%#x  %d words changed (table)' % (diff[i], diff[j], j - i + 1))
    else:
        for k in diff[i:j + 1]: print('%#x  %d (%#x) -> %d (%#x)' % (k, a[k], a[k], b[k], b[k]))
    i = j + 1

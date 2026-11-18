"""List CamCore register-table windows: leaf fns doing  r0=base+CONST ; r2=LEN ; b memcpy(0x43b7f878)  (A6000 av-cam 3.21).
usage: isp_windows.py av-cam.bin   -> 'fn  dir  base_offset  length'   dir: W=sw->hw (r0=dst hw) / R=hw->sw"""
import capstone, re, sys
d = open(sys.argv[1], 'rb').read(); VB = 0x43541000
MEMCPY = 0x43b7f878
md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB); md.skipdata = True
ins = list(md.disasm(d, VB))
for n, i in enumerate(ins):
    if i.mnemonic in ('b.w', 'bl') and i.op_str == '#%#x' % MEMCPY:
        off = 0; ln = None; hwreg = None; start = i.address; moved = False; j = n - 1; steps = 0
        while j >= 0 and steps < 12:
            p = ins[j]; steps += 1
            if p.mnemonic in ('bx', 'pop', 'pop.w', 'push', 'b.w', 'bl') or p.mnemonic.startswith('b'):
                break
            if p.mnemonic in ('mov.w', 'movs', 'movw') and p.op_str.startswith('r2, #'):
                ln = int(p.op_str.split('#')[1], 0)
            m = re.match(r'(r\d+), (r\d+), #(0x[0-9a-f]+|\d+)$', p.op_str)
            if m and p.mnemonic.startswith('add'):
                off += int(m.group(3), 0)
            m = re.match(r'(r\d+), #(0x[0-9a-f]+|\d+)$', p.op_str)
            if m and p.mnemonic == 'adds':
                off += int(m.group(2), 0)
            if p.mnemonic == 'mov' and p.op_str == 'r0, r1':
                moved = True
            start = p.address; j -= 1
        if off >= 0x1000 and ln:
            print('%x\t%s\t%#x\t%#x' % (start, 'W' if not moved else 'R?', off, ln))

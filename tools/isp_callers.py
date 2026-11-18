"""For each target fn (hex VAs on argv[2:]) list its callers and the log strings referenced near each call site.
usage: isp_callers.py av-cam.bin VA [VA ...]      (A6000 av-cam 3.21, VA base 0x43541000; PIC strings via ldr [pc]+add pc)"""
import capstone, re, struct, sys, collections
d = open(sys.argv[1], 'rb').read(); VB = 0x43541000
tg = set(int(x, 16) for x in sys.argv[2:])
md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB); md.skipdata = True
ins = list(md.disasm(d, VB))
def cstr(va):
    o = va - VB
    if not 0 <= o < len(d): return None
    e = d.find(b'\0', o); s = d[o:e]
    return s.decode('latin1') if 5 <= len(s) < 100 and all(32 <= c < 127 for c in s) else None
strs = {}      # instruction index -> string
pend = {}
for n, i in enumerate(ins):
    if i.mnemonic.startswith('ldr') and '[pc' in i.op_str and ',' in i.op_str and '!' not in i.op_str:
        rt = i.op_str.split(',')[0]
        imm = int(re.sub(r'[^0-9a-fx]', '', i.op_str.split('#')[1]), 16) if '#' in i.op_str else 0
        la = ((i.address + 4) & ~3) + imm
        if 0 <= la - VB < len(d) - 4: pend[rt] = struct.unpack_from('<I', d, la - VB)[0]
    if i.mnemonic == 'add' and i.op_str.endswith(', pc'):
        rt = i.op_str.split(',')[0]
        if rt in pend:
            s = cstr((pend.pop(rt) + i.address + 4) & 0xffffffff)
            if s: strs[n] = s
sidx = sorted(strs)
import bisect
for n, i in enumerate(ins):
    if i.mnemonic in ('bl', 'b.w', 'blx') and i.op_str.startswith('#'):
        t = int(i.op_str[1:], 16)
        if t in tg:
            k = bisect.bisect(sidx, n)
            near = [strs[x] for x in sidx[max(0, k - 3):k + 3] if abs(x - n) < 90]
            print('%x -> %x  %s' % (i.address, t, ' | '.join(near)[:200]))

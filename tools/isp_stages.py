"""List CamCore 'stage setter/getter' functions in an A6000 av-cam: functions that call >=3 register accessors.
usage: isp_stages.py av-cam.bin regs.json(from isp_regscan.py) [start end]   (VA base 0x43541000)"""
import capstone, json, re, sys
d = open(sys.argv[1], 'rb').read(); VB = 0x43541000
regs = json.load(open(sys.argv[2]))
acc = {}                                   # accessor VA -> (offset, kind)
for off, v in regs.items():
    for k in 'RW':
        for a, _ in v[k]:
            acc[int(a, 16)] = (int(off, 16), k)
st = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x43b60000
en = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0x43b80000
md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB)
fn = None; cur = {}
out = []
def flush():
    if fn and len(cur['hits']) >= 3:
        offs = sorted(set(o for o, k in cur['hits']))
        kinds = ''.join(sorted(set(k for o, k in cur['hits'])))
        out.append((fn, kinds, len(cur['hits']), offs))
for i in md.disasm(d[st - VB:en - VB], st):
    if i.mnemonic in ('push', 'push.w') and 'lr' in i.op_str:
        flush(); fn = i.address; cur = {'hits': []}
    if fn and i.mnemonic in ('bl', 'b.w', 'blx'):
        m = re.match(r'#(0x[0-9a-f]+)', i.op_str)
        if m and int(m.group(1), 16) in acc:
            cur['hits'].append(acc[int(m.group(1), 16)])
flush()
for fn_, kinds, n, offs in out:
    print('%x\t%s\t%d fields\t%#x..%#x' % (fn_, kinds, n, offs[0], offs[-1]))

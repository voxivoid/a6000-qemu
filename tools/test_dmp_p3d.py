"""Synthetic P3D command-list tests for hw/display/dmp_smaph.c.

Starts the sony-a6000 machine under the qtest accelerator (no guest code
runs), writes hand-built shader programs, vertex data and command lists
into RAM, kicks them through the GPU registers (+0x1804 / +0x1800) and
reads the colour / depth buffers back.  Covers the fixed-function parts the
stock UI lists do not exercise yet: depth and stencil test, points and
lines, mip levels / LOD bias / linear mip filter, border wrap.  Register
meanings: docs/reference/RE_GPU_P3D_CMD.md.  No firmware data is used except the
NAND image run.sh needs to build the machine.

usage: python3 -I emulator/tools/test_dmp_p3d.py <qemu-system-arm> <nand>
       [qtest port, default 46073]
"""
import os, socket, struct, subprocess, sys, time

QEMU, NAND = sys.argv[1], sys.argv[2]
RUN = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   'run.sh')
PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 46073
env = dict(os.environ, A6K_NAND=NAND, A6K_QEMU=QEMU)
proc = subprocess.Popen([RUN, '-display', 'none', '-serial', 'none', '-accel', 'qtest',
                         '-qtest', 'tcp:127.0.0.1:%d,server=on,wait=off' % PORT,
                         '-monitor', 'none'], env=env,
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(50):
    try:
        sk = socket.create_connection(('127.0.0.1', PORT)); break
    except OSError:
        time.sleep(0.2)
f = sk.makefile('rw')

def cmd(c):
    f.write(c + '\n'); f.flush()
    while True:
        l = f.readline().strip()
        if l.startswith(('OK', 'ERR', 'FAIL')):
            return l

def wmem(pa, data):
    for o in range(0, len(data), 4096):
        ch = data[o:o + 4096]
        cmd('write 0x%x %d 0x%s' % (pa + o, len(ch), ch.hex()))

def rmem(pa, n):
    return bytes.fromhex(cmd('read 0x%x %d' % (pa, n)).split()[1][2:])

GPU = 0xF1000000
W = H = 64
CB, ZB, VB, LST, SH, TEX = 0x88000000, 0x88100000, 0x88200000, 0x88300000, 0x88400000, 0x88500000
f32 = lambda x: struct.unpack('<I', struct.pack('<f', x))[0]

def tiled(x, y, w):
    m = 0
    for b in range(3):
        m |= ((x >> b) & 1) << (2 * b) | ((y >> b) & 1) << (2 * b + 1)
    return ((y // 8) * (w // 8) + x // 8) * 64 + m

class L:
    def __init__(s): s.e = []
    def w(s, reg, val): s.e.append(((reg << 16) | 0xF000, val & 0xffffffff)); return s

def kick(lst):
    data = b''.join(struct.pack('<II', h, v) for h, v in lst.e)
    wmem(LST, data)
    cmd('writel 0x%x 0x%x' % (GPU + 0x1804, LST))
    cmd('writel 0x%x %d' % (GPU + 0x1800, len(lst.e)))
    cmd('writel 0x%x 0' % (GPU + 0x1000))

D0 = (0xF << 26) | (0xE4 << 13) | 0xE4
def mov_o_v(o, v): return [(v << 28) | (2 << 24), (0x0c << 20) | (1 << 19) | (o << 8)]
END = [0, 0x4000]
VS = mov_o_v(0, 0) + mov_o_v(1, 1) + mov_o_v(2, 2) + END
FS_COL = mov_o_v(1, 1) + END
FS_TEX = [(2 << 28) | (2 << 24), (0x20 << 20) | (3 << 8),     # TEX X3 <- V2, unit 0
          (3 << 28), (0x0c << 20) | (1 << 19) | (1 << 8)] + END   # MOV o1 = X3

def setup():
    l = L()
    l.w(0x458, 0)
    for wd in VS: l.w(0x459, wd)
    l.w(0x458, 8)
    for wd in FS_COL: l.w(0x459, wd)
    l.w(0x458, 16)
    for wd in FS_TEX: l.w(0x459, wd)
    l.w(0x460, 0).w(0x461, D0)
    l.w(0x112, CB >> 4).w(0x113, W | H << 16).w(0x110, 5 << 16)
    l.w(0x42, f32(W / 2)).w(0x43, f32(H / 2)).w(0x49, W | H << 16)
    l.w(0x47, (-W & 0xffff) | (-H & 0xffff) << 16).w(0x48, W | H << 16)
    l.w(0x41, 0).w(0x10C, 0xF).w(0x100, 0).w(0x104, 0)
    l.w(0x107, 0xF00).w(0x105, 0).w(0x10D, 0).w(0x10E, 0)
    # vertex layout: slot0 vec4 pos, slot1 vec4 colour, slot2 vec2 uv; stride 40
    l.w(0x20D, 0).w(0x20E, 2).w(0x210, 0).w(0x212, 39).w(0x216, VB)
    l.w(0x204, 0x210).w(0x206, 0x333).w(0x208, 3 | 3 << 2 | 1 << 4)
    l.w(0x209, 0 | 16 << 8 | 32 << 16).w(0x442, 0).w(0x443, 8)
    kick(l)

def clear():
    wmem(CB, b'\0' * (W * H * 4))
    wmem(ZB, struct.pack('<I', 0x00FFFFFF) * (W * H))

def draw(verts, prim, topo, extra=()):
    data = b''
    for (x, y, z, col, uv) in verts:
        data += struct.pack('<10f', x, y, z, 1, *col, *uv)
    wmem(VB, data)
    l = L()
    for r, v in extra: l.w(r, v)
    l.w(0x401, prim).w(0x404, 0x20 | topo).w(0x203, 0).w(0x202, len(verts))
    kick(l)

def quad(x0, y0, x1, y1, z, col, uv=((0, 0), (1, 0), (0, 1), (1, 1))):
    return [(x0, y0, z, col, uv[0]), (x1, y0, z, col, uv[1]),
            (x0, y1, z, col, uv[2]), (x1, y1, z, col, uv[3])]

def px(x, y):
    return tuple(rmem(CB + tiled(x, y, W) * 4, 4))

RED, GRN, BLU = (1, 0, 0, 1), (0, 1, 0, 1), (0, 0, 1, 1)
res = []
def check(name, got, want):
    ok = got == want
    res.append(ok)
    print('%s %-40s got %s want %s' % ('PASS' if ok else 'FAIL', name, got, want))

setup()
# 1 depth test LESS + write (D24S8)
clear()
DZ = [(0x111, ZB >> 4), (0x10F, 3), (0x10D, 3), (0x10E, 3)]
dep = lambda func: DZ + [(0x107, 1 | func << 4 | 0xF00 | 0x1000)]
draw(quad(-1, -1, 1, 1, 0.0, RED), 0x20, 1, dep(4))
draw(quad(-1, -1, 1, 1, 0.4, GRN), 0x20, 1, dep(4))
draw(quad(-1, -1, 0, 1, -0.4, BLU), 0x20, 1, dep(4))
check('depth: nearer quad wins (left)', px(10, 32), (0, 0, 255, 255))
check('depth: farther quad rejected (right)', px(50, 32), (255, 0, 0, 255))
zv = struct.unpack('<I', rmem(ZB + tiled(10, 32, W) * 4, 4))[0] & 0xffffff
check('depth: buffer holds 0.3', round(zv / 0xffffff, 3), 0.3)
# 2 stencil: mark left half with ref 1 (colour writes off), then EQUAL 1
clear()
st = lambda func, ref, ops: DZ + [(0x107, 0xF00), (0x105, 1 | func << 4 | 0xff << 8 | ref << 16 | 0xff << 24), (0x106, ops)]
draw(quad(-1, -1, 0, 1, 0, RED), 0x20, 1, st(1, 1, 2 << 8) + [(0x10C, 0)])
draw(quad(-1, -1, 1, 1, 0, GRN), 0x20, 1, st(2, 1, 0) + [(0x10C, 0xF)])
check('stencil: EQUAL passes where marked', px(10, 32), (0, 255, 0, 255))
check('stencil: EQUAL fails elsewhere', px(50, 32), (0, 0, 0, 0))
# 3 lines and points (fallback rasteriser)
clear()
base = [(0x107, 0xF00), (0x105, 0)]
draw([(-1, 0.01, 0, RED, (0, 0)), (1, 0.01, 0, RED, (0, 0))], 0x10, 0, base)
check('line: horizontal span drawn', (px(5, 32), px(60, 32)), ((255, 0, 0, 255),) * 2)
check('line: off-line pixel untouched', px(5, 34), (0, 0, 0, 0))
draw([(8.5 / 32, 8.5 / 32, 0, GRN, (0, 0))], 0, 0, base)
check('point: pixel drawn', px(40, 40), (0, 255, 0, 255))
# 4 mipmaps: 16x16 RGBA (GL byte order), levels red / green / blue
clear()
lv = []
for k, c in enumerate([(255, 0, 0, 255), (0, 255, 0, 255), (0, 0, 255, 255)]):
    a = max(16 >> k, 8)
    lv.append(bytes(c) * (a * a))
wmem(TEX, b''.join(lv))
def texregs(smp, maxl, minl=0, bias=0):
    return base + [(0x443, 16), (0x585, 0x0000ffff), (0x586, 16 << 16 | 16), (0x587, 16 << 16 | 16),
                   (0x588, (bias & 0x1fff) | minl << 16 | maxl << 24), (0x589, TEX >> 3),
                   (0x58F, 25), (0x5DD, smp)]
def tq(n):  # n x n pixel quad at the screen origin
    s = n / 32
    return quad(-1, -1, -1 + s, -1 + s, 0, RED)
for n, want, name in [(16, (255, 0, 0, 255), 'level 0 at 1:1'),
                      (8, (0, 255, 0, 255), 'level 1 at 2:1'),
                      (4, (0, 0, 255, 255), 'level 2 at 4:1')]:
    clear()
    draw(tq(n), 0x20, 1, texregs(1, 2))
    check('mipmap NEAREST_MIPMAP_NEAREST ' + name, px(1, 1), want)
clear()
draw(tq(4), 0x20, 1, texregs(1, 0))
check('no mipmap (max level 0): level 0', px(1, 1), (255, 0, 0, 255))
clear()
draw(tq(8), 0x20, 1, texregs(1, 2, bias=-256))
check('LOD bias -1: level 0 at 2:1', px(1, 1), (255, 0, 0, 255))
clear()
draw(tq(6), 0x20, 1, texregs(1 | 1 << 24, 2))
p = px(1, 1)
check('LINEAR mip blend (lambda~1.4) green+blue', p[0] == 0 and p[1] > 0 and p[2] > 0, True)
# 5 border wrap: uv in [1.5, 2], border colour 0x0000ffff = (0,0,255,255)
clear()
draw(quad(-1, -1, -0.5, -0.5, 0, RED, ((1.5, 1.5), (2, 1.5), (1.5, 2), (2, 2))), 0x20, 1,
     texregs(1 | 1 << 12 | 1 << 8, 0))
check('CLAMP_TO_BORDER outside = border colour', px(1, 1), (0, 0, 255, 255))
print('%d passed, %d failed' % (sum(res), len(res) - sum(res)))
proc.terminate()
sys.exit(0 if all(res) else 1)

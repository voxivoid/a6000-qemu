#!/usr/bin/env python3
"""ispctl.py - apply a full RAM-only ISP "look" preset to a real A6000 (FW 3.21): gamma + colour matrix + saturation + sharpness + white-balance tint.

    CAM=<ip> ispctl.py status
    CAM=<ip> ispctl.py apply presets/slog2.json
    CAM=<ip> ispctl.py revert

Needs: OpenMemories-Tweak Wifi+Telnet+ADB on, adb, mods/look/gammamod built. Every controlled register group is "frozen" by patching the camera's own
setter to `bx lr` (RAM only; battery pull = stock). Register map / evidence: docs/A6000_ISP_REGISTER_MAP.md. Preset format: presets/README in this folder
(keys: gamma, matrix, saturation, sharpness, wb; every key optional).
"""
import json, os, struct, subprocess, sys, tempfile, shutil, math
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import make_hwgamma as G

CAM = os.environ.get('CAM') or sys.exit('set CAM=<camera ip>')
STATE = os.path.join(HERE, '.state_stock.json')          # stock register values captured before the first apply (git-ignored)
BASE = 0xF3200000
# group: setter PA (VA-0x40000000), original 4 bytes, register address, number of words
SETTERS = {
    'gamma':  (0x83b67414, '00f5a830', 'gamma'),
    'matrix': (0x83b675e0, '70b50c46', 'matrix'),
    'sat':    (0x83b69312, '70b50d46', 'sat'),
    'sharp':  (0x83b68c88, '70b50d46', 'sharp'),
    'wb':     (0x83b665b6, '70b50c46', 'wb'),
}
REGS = {'gamma': (BASE + 0x15300, 96), 'matrix': (BASE + 0x1550C, 9), 'sat': (BASE + 0x156AC, 6), 'sharp': (BASE + 0x15C14, 2),
        'wb_r': (BASE + 0x11900, 1), 'wb_b': (BASE + 0x11908, 1),
        'wb_r_src': (BASE + 0x14900, 1), 'wb_b_src': (BASE + 0x14908, 1)}   # 0x14900/908 = the camera's own current WB (0x11900/908 = hardware copy)


def sh(cmd, timeout=90):
    r = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'a6kshell.py'), '--host', CAM, '--port', '23', 'run', cmd],
                       capture_output=True, text=True, timeout=timeout)
    return (r.stdout + r.stderr)


def adb_push(local, remote):
    subprocess.run(['adb', 'connect', CAM + ':5555'], capture_output=True)
    subprocess.run(['adb', 'push', local, '/' + remote], capture_output=True, check=True)


def rd(addr, n):
    out = sh('/android/gammamod -F hwrd 0x%X %d' % (addr, n))
    vals = [int(l.split(': ')[1]) for l in out.splitlines() if l.startswith('0x') and ': ' in l]
    if len(vals) != n: sys.exit('read failed at %#x: %r' % (addr, out[-200:]))
    return vals


def wr(addr, words):
    tmp = tempfile.mktemp(); open(tmp, 'wb').write(struct.pack('<%dI' % len(words), *words))
    adb_push(tmp, 'ispw.bin'); os.unlink(tmp)
    out = sh('/android/gammamod -F hwwr 0x%X /android/ispw.bin' % addr)
    if 'readback ok' not in out: sys.exit('write failed/mismatch at %#x: %s' % (addr, out[-200:]))


def setter_state(name):
    pa, orig, _ = SETTERS[name]
    ob = bytes.fromhex(orig); pb = b'\x70\x47' + ob[2:]
    for tag, b in (('o', ob), ('p', pb)):
        tmp = tempfile.mktemp(); open(tmp, 'wb').write(b); adb_push(tmp, 'st_%s_%s.bin' % (name, tag)); os.unlink(tmp)
    out = sh('/android/gammamod read 0x%X 4 /android/st_c.bin >/dev/null; '
             'if busybox cmp /android/st_c.bin /android/st_%s_o.bin >/dev/null 2>&1; then echo STATE_original; '
             'elif busybox cmp /android/st_c.bin /android/st_%s_p.bin >/dev/null 2>&1; then echo STATE_patched; else echo STATE_UNKNOWN; fi' % (pa, name, name))
    for s in ('original', 'patched', 'UNKNOWN'):
        if 'STATE_' + s in out: return s
    sys.exit('cannot read setter state: ' + out[-200:])


def set_setter(name, patched):
    pa = SETTERS[name][0]
    sh('/android/gammamod write 0x%X /android/st_%s_%s.bin' % (pa, name, 'p' if patched else 'o'))


def prep():
    subprocess.run(['adb', 'connect', CAM + ':5555'], capture_output=True)
    adb_push(os.path.join(HERE, 'gammamod'), 'gammamod'); sh('chmod 755 /android/gammamod')


def capture_stock():
    """Stock values of every controlled group; only trusted when no setter is patched."""
    st = {k: rd(*REGS[k]) for k in REGS}
    json.dump(st, open(STATE, 'w'))
    return st


def s16(x): return x - 65536 if x & 0x8000 else x


def inv3(m):
    a, b, c, d, e, f, g, h, i = [x for r in m for x in r]
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    return [[(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det],
            [(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det],
            [(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det]]


def mul3(x, y): return [[sum(x[i][k] * y[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def rgb2xyz(prim, white):
    (xr, yr), (xg, yg), (xb, yb) = prim
    P = [[xr / yr, xg / yg, xb / yb], [1, 1, 1], [(1 - xr - yr) / yr, (1 - xg - yg) / yg, (1 - xb - yb) / yb]]
    w = [white[0] / white[1], 1, (1 - white[0] - white[1]) / white[1]]
    iP = inv3(P); s = [sum(iP[i][k] * w[k] for k in range(3)) for i in range(3)]
    return [[P[i][j] * s[j] for j in range(3)] for i in range(3)]


D65 = (0.3127, 0.3290)
M709 = rgb2xyz([(0.64, 0.33), (0.30, 0.60), (0.15, 0.06)], D65)
PRIMS = {'awg3': [(0.6840, 0.3130), (0.2210, 0.8480), (0.0861, -0.1020)],
         'rec2020': [(0.708, 0.292), (0.170, 0.797), (0.131, 0.046)],
         'p3': [(0.680, 0.320), (0.265, 0.690), (0.150, 0.060)],
         'slg3': [(0.730, 0.280), (0.140, 0.855), (0.100, -0.050)]}


def matrix_words(spec, stock9):
    S = [[s16(stock9[r * 3 + c] & 0xFFFF) / 1024.0 for c in range(3)] for r in range(3)]
    if isinstance(spec, str): spec = {'mode': spec}
    mode = spec.get('mode', 'stock')
    if mode == 'stock': T = [[1, 0, 0], [0, 1, 0], [0, 0, 1]]
    elif mode == 'identity': T = None
    elif mode in PRIMS: T = mul3(inv3(rgb2xyz(PRIMS[mode], D65)), M709)
    elif mode == 'sat':
        s = spec.get('value', 1.0); Y = M709[1]
        T = [[(s if i == j else 0) + (1 - s) * Y[j] for j in range(3)] for i in range(3)]
    elif mode == 'custom': T = None; M = spec['matrix']
    else: sys.exit('unknown matrix mode ' + mode)
    if mode == 'identity': M = [[1, 0, 0], [0, 1, 0], [0, 0, 1]]
    elif mode == 'custom': pass
    else: M = mul3(T, S)
    Mi = [[int(round(M[i][j] * 1024)) for j in range(3)] for i in range(3)]
    for i in range(3): Mi[i][i] += 1024 - sum(Mi[i])
    return [Mi[i][j] & 0xFFFF for i in range(3) for j in range(3)]


def gamma_words(spec):
    if isinstance(spec, str): spec = {'look': spec}
    if 'file' in spec: return list(struct.unpack('<96I', open(os.path.join(ROOT, spec['file']), 'rb').read()))
    look = spec['look']; gx = spec.get('grey_x', 76.0); rng = spec.get('range', 'legal'); out = []
    for k in G.KNOTS:
        r = 0.18 * k / gx
        if look == 'slog2': y = G.slog2(r); c = (64 + 876 * y) if rng == 'legal' else y * 1023
        elif look == 'slog3': y = G.slog3(r); c = (64 + 876 * y) if rng == 'legal' else y * 1023
        elif look == 'logc3': c = G.logc3(r) * 1023
        elif look == 'arri709': c = G.arri709(r) * 1023
        elif look == 'rec709': c = G.rec709(r / 5.0) * 1023
        elif look == 'identity': c = k * 1023 / 2047
        elif look == 'gamma': c = 1023 * (min(1.0, k / 2047.0) ** (1.0 / spec.get('g', 2.2)))
        else: sys.exit('unknown gamma look ' + look)
        out.append(max(0, min(1023, int(round(c)))))
    for i in range(1, 96): out[i] = max(out[i], out[i - 1])
    return out


def cmd_status():
    prep()
    for k in SETTERS: print('%-7s setter: %s' % (k, setter_state(k)))
    g = rd(*REGS['gamma']); print('gamma[0:6]', g[:6], ' matrix', [s16(x & 0xFFFF) for x in rd(*REGS['matrix'])])
    print('sat gains', rd(*REGS['sat']), ' sharp', rd(*REGS['sharp']), ' wb R,B hw', rd(*REGS['wb_r']) + rd(*REGS['wb_b']), ' src', rd(*REGS['wb_r_src']) + rd(*REGS['wb_b_src']))


def cmd_apply(path):
    preset = json.load(open(path)); prep()
    states = {k: setter_state(k) for k in SETTERS}
    if 'UNKNOWN' in states.values(): sys.exit('setter bytes differ from FW 3.21 (%s): refusing' % states)
    if not os.path.exists(STATE) or all(v == 'original' for v in states.values()): stock = capture_stock()
    else: stock = json.load(open(STATE))
    live = {k: rd(*REGS[k]) for k in REGS}
    todo = []
    if 'gamma' in preset: todo.append(('gamma', REGS['gamma'][0], gamma_words(preset['gamma'])))
    if 'matrix' in preset: todo.append(('matrix', REGS['matrix'][0], matrix_words(preset['matrix'], stock['matrix'])))
    if 'saturation' in preset:
        sp = preset['saturation']; base = stock['sat']
        if isinstance(sp, list): g = list(sp)
        else: g = [int(max(0, min(255, round(base[i] * sp)))) for i in (0, 1, 4, 5)]
        todo.append(('sat', REGS['sat'][0], [g[0], g[1], base[2], base[3], g[2], g[3]]))
    if 'sharpness' in preset:
        v = int(preset['sharpness']) & 0xF; todo.append(('sharp', REGS['sharp'][0], [v, v]))
    if 'wb' in preset:
        w = preset['wb']; r0, b0 = stock['wb_r_src'][0], stock['wb_b_src'][0]
        todo.append(('wb', REGS['wb_r'][0], [int(round(r0 * w.get('r_scale', 1.0)))]))
        todo.append(('wb', REGS['wb_b'][0], [int(round(b0 * w.get('b_scale', 1.0)))]))
    for grp in {t[0] for t in todo}:                         # freeze first, then write: no race with the camera's own writer
        if states[grp] == 'original': set_setter(grp, True)
    for grp, addr, words in todo:
        wr(addr, words); print('wrote %-6s @%#x %s' % (grp, addr, words if len(words) <= 9 else words[:6] + ['...']))
    ok = all(rd(addr, len(words)) == words for _, addr, words in todo)
    print('preset applied, registers verified' if ok else 'WARNING: readback differs (camera rewrote something?)')


def cmd_revert():
    prep()
    for k in SETTERS:
        if setter_state(k) == 'patched': set_setter(k, False)
    print('all setters restored; the camera re-programs its own values at its next event (change a setting / press shutter half-way / mode switch)')


if __name__ == '__main__':
    a = sys.argv[1:]
    if a[:1] == ['status']: cmd_status()
    elif a[:1] == ['apply'] and len(a) == 2: cmd_apply(a[1])
    elif a[:1] == ['revert']: cmd_revert()
    else: print(__doc__)

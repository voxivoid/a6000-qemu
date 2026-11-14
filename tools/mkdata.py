#!/usr/bin/env python3
"""Build data/ from the official ILCE-5000 1.10 updater (no camera needed).

The camera resumes from a factory warm-boot image (WBI) that Sony ships in the
updater as partition nflasha5.  This script decompresses it with fwtool.py
(https://github.com/ma1co/fwtool.py, MIT) and derives:

  data/wbi/ram.bin, data/wbi/map.tsv   RAM snapshot for tools/wbi.py
                                       (map.tsv: file offset, PA, VA, length)
  data/handoff_80021000.bin            bootloader hand-off page, PA 0x80021000
  data/submicro_frames.bin             sub-micro RX frames, PA 0x8334417C, with
                                       the power-off flag (frame 2 byte 33 bit 6)
                                       cleared and the frame CRC recomputed
                                       (docs/RE_SUBMICRO_LINK.md section 4)

usage: mkdata.py --fwtool <fwtool.py checkout> [--nflasha5 <path>] [--out data]
"""
import argparse
import os
import sys


def crc16(f):
    """CRC-16/CCITT-FALSE over bytes 0..125, stored LE in 126..127."""
    c = 0xFFFF
    for x in f[:126]:
        c ^= x << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021 if c & 0x8000 else c << 1) & 0xFFFF
    f[126], f[127] = c & 0xFF, c >> 8


def main():
    e = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    fw = os.environ.get('A5K_FW') or (e + '/firmware' if os.path.isdir(e + '/firmware')
                                      else os.path.dirname(e) + '/firmware')
    ap = argparse.ArgumentParser()
    ap.add_argument('--fwtool', required=True, help='fwtool.py checkout (provides the fwtool package)')
    ap.add_argument('--nflasha5', default=fw + '/unpacked/firmware.tar_unpacked/0700_part_image/dev/nflasha5')
    ap.add_argument('--out', default=e + '/data')
    a = ap.parse_args()

    sys.path.insert(0, os.path.abspath(a.fwtool))
    from fwtool.sony import wbi

    os.makedirs(a.out + '/wbi', exist_ok=True)
    secs, off = [], 0
    with open(a.nflasha5, 'rb') as src, open(a.out + '/wbi/ram.bin', 'wb') as ram:
        for c in wbi.readWbi(src):
            data = c.contents.read()
            ram.write(data)
            secs.append((off, c.physicalAddr, c.virtualAddr, len(data)))
            off += len(data)
    with open(a.out + '/wbi/map.tsv', 'w') as m:
        for s in secs:
            m.write('%08x\t%08x\t%08x\t%08x\n' % s)
    print('wbi: %d sections, %d bytes' % (len(secs), off))

    ram = open(a.out + '/wbi/ram.bin', 'rb').read()

    def rd(pa, n):
        for o, p, _, ln in secs:
            if p <= pa and pa + n <= p + ln:
                return bytearray(ram[o + pa - p:o + pa - p + n])
        raise SystemExit('PA 0x%08x not in the snapshot' % pa)

    open(a.out + '/handoff_80021000.bin', 'wb').write(rd(0x80021000, 0x1000))
    f = rd(0x8334417C, 256)
    f[128 + 33] &= ~0x40                 # snapshot was taken at power-off
    for k in (0, 128):
        frame = f[k:k + 128]
        crc16(frame)
        f[k:k + 128] = frame
    open(a.out + '/submicro_frames.bin', 'wb').write(f)
    print('wrote', a.out + '/handoff_80021000.bin,', a.out + '/submicro_frames.bin')


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Create a blank SD card image formatted per the SD Association File System
Specification (Part 2) for SDHC cards: MBR + one FAT32 partition aligned to the
4 MiB boundary unit, 32 KiB clusters, data area starting on a boundary unit.

    tools/mksd.py OUT.img [SIZE_GIB=4] [--label NAME]

The camera reformats cards this way [? exact Sony boot-sector strings are not
known; OEM name and volume id below are placeholders].  The image is sparse.
"""
import struct, sys, os

def chs(lba):
    c, r = divmod(lba, 255 * 63)
    h, s = divmod(r, 63)
    if c > 1023:
        return b'\xfe\xff\xff'
    return bytes((h, ((c >> 2) & 0xc0) | (s + 1), c & 0xff))

def mksd(path, gib=4, label='NO NAME'):
    ts = gib * 1024 * 1024 * 2           # total sectors
    bu = 8192                            # boundary unit (4 MiB) for SDHC
    sc = 64                              # sectors per cluster (32 KiB)
    nom = bu                             # sectors before the partition
    # SD FS spec appendix: choose SF and RSC so that the data area starts on BU
    sf = 1
    while True:
        rsc = bu - (2 * sf) % bu
        if rsc < 9:
            rsc += bu
        clusters = (ts - nom - rsc - 2 * sf) // sc
        sf2 = -(-((clusters + 2) * 4) // 512)
        if sf2 > sf:
            sf = sf2
            continue
        break
    psize = ts - nom
    with open(path, 'wb') as f:
        f.truncate(ts * 512)
        mbr = bytearray(512)
        mbr[446:462] = (b'\x00' + chs(nom) + b'\x0c' + chs(ts - 1) +
                        struct.pack('<II', nom, psize))
        mbr[510:512] = b'\x55\xaa'
        f.seek(0); f.write(mbr)
        bs = bytearray(512)
        bs[0:3] = b'\xeb\x58\x90'
        bs[3:11] = b'MSDOS5.0'           # [?] OEM name
        struct.pack_into('<HBHBHHBHHHII', bs, 11, 512, sc, rsc, 2, 0, 0, 0xf8,
                         0, 63, 255, nom, psize)
        struct.pack_into('<IHHIHH', bs, 36, sf, 0, 0, 2, 1, 6)
        bs[64] = 0x80; bs[66] = 0x29
        struct.pack_into('<I', bs, 67, 0x12345678)   # [?] volume id
        bs[71:82] = label.encode().ljust(11)[:11]
        bs[82:90] = b'FAT32   '
        bs[510:512] = b'\x55\xaa'
        fsi = bytearray(512)
        struct.pack_into('<I', fsi, 0, 0x41615252)
        struct.pack_into('<III', fsi, 484, 0x61417272, clusters - 1, 3)
        fsi[510:512] = b'\x55\xaa'
        for base in (nom, nom + 6):
            f.seek(base * 512); f.write(bs); f.write(fsi)
        fat0 = struct.pack('<III', 0x0ffffff8, 0x0fffffff, 0x0fffffff)
        for k in range(2):
            f.seek((nom + rsc + k * sf) * 512); f.write(fat0)
        # root directory (cluster 2): volume label entry
        f.seek((nom + rsc + 2 * sf) * 512)
        f.write(label.encode().ljust(11)[:11] + b'\x08' + bytes(20))
    return dict(total=ts, nom=nom, rsc=rsc, sf=sf, clusters=clusters,
                data=nom + rsc + 2 * sf)

if __name__ == '__main__':
    a = [x for x in sys.argv[1:] if not x.startswith('--')]
    lab = 'NO NAME'
    if '--label' in sys.argv:
        lab = sys.argv[sys.argv.index('--label') + 1]
        a.remove(lab)
    info = mksd(a[0], int(a[1]) if len(a) > 1 else 4, lab)
    print(info, 'data area on BU:', info['data'] % 8192 == 0)

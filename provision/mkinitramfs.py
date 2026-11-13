#!/usr/bin/env python3
"""Build the provisioning initramfs (cpio newc, uncompressed).

Contents: Sony's own busybox + glibc (rootfs nflasha7), Sony's NAND kernel
modules (initrd /kmod), a5kcopy, and init.sh as /init.  Nothing here is
modified firmware: it is a factory-style programming environment that lets
Sony's MUMIN FTL format the emulated NAND and receive the logical image.

usage: mkinitramfs.py <out.cpio> [init-script (default init.sh)]
"""
import os, sys, stat
R = os.path.abspath(os.path.join(os.path.dirname(__file__), '../..'))
DEV = R + '/firmware/unpacked/firmware.tar_unpacked/0700_part_image/dev'
ROOTFS = DEV + '/nflasha7_unpacked_unpacked'
INITRD = DEV + '/nflasha3_unpacked_unpacked/initrd.img_unpacked'
HERE = os.path.dirname(os.path.abspath(__file__))

ents = []   # (path, mode, data or linktarget)
def d(p): ents.append((p, stat.S_IFDIR | 0o755, b''))
def f(p, src, mode=0o755): ents.append((p, stat.S_IFREG | mode, open(src, 'rb').read()))
def l(p, tgt): ents.append((p, stat.S_IFLNK | 0o777, tgt.encode()))
def n(p, kind, maj, mi): ents.append((p, kind | 0o600, (maj, mi)))

for x in ('bin', 'sbin', 'lib', 'kmod', 'dev', 'proc', 'sys', 'tmp', 'mnt', 'usr', 'usr/bin', 'usr/sbin'):
    d(x)
f('bin/busybox', ROOTFS + '/bin/busybox')
for a in ('sh', 'ash', 'mount', 'umount', 'insmod', 'lsmod', 'cat', 'ls', 'echo', 'dd', 'md5sum',
          'sleep', 'mkdir', 'mknod', 'sync', 'hexdump', 'poweroff', 'reboot', 'halt', 'dmesg', 'grep', 'sed', 'cmp', 'tail', 'head', 'printf'):
    l('bin/' + a, 'busybox')
for so in sorted(os.listdir(ROOTFS + '/lib')):
    p = ROOTFS + '/lib/' + so
    if os.path.isfile(p): f('lib/' + so, p)
for k, v in {'ld-linux.so.3': 'ld-2.11.2.so', 'libc.so.6': 'libc-2.11.2.so',
             'libcrypt.so.1': 'libcrypt-2.11.2.so', 'libm.so.6': 'libm-2.11.2.so',
             'libdl.so.2': 'libdl-2.11.2.so', 'libpthread.so.0': 'libpthread-2.11.2.so',
             'librt.so.1': 'librt-2.11.2.so', 'libstdc++.so.6': 'libstdc++.so.6.0.14'}.items():
    l('lib/' + k, v)
for ko in ('devno', 'dmac', 'ldec', 'lld', 'nand'):
    f('kmod/%s.ko' % ko, INITRD + '/kmod/%s.ko' % ko, 0o644)
f('bin/a5kcopy', HERE + '/a5kcopy')
f('init', HERE + '/' + (sys.argv[2] if len(sys.argv) > 2 else 'init.sh'))
n('dev/console', stat.S_IFCHR, 5, 1)
n('dev/mem', stat.S_IFCHR, 1, 1)
n('dev/null', stat.S_IFCHR, 1, 3)

def hdr(ino, mode, size, nlink, name, rdev=(0, 0)):
    nm = name.encode() + b'\0'
    h = b'070701' + b''.join(b'%08X' % v for v in (
        ino, mode, 0, 0, nlink, 0, size, 0, 0, rdev[0], rdev[1], len(nm), 0))
    out = h + nm
    return out + b'\0' * (-len(out) % 4)

buf = bytearray()
for i, (p, mode, data) in enumerate(ents, 1):
    if isinstance(data, tuple):
        buf += hdr(i, mode, 0, 1, p, data)
        continue
    buf += hdr(i, mode, len(data), 2 if stat.S_ISDIR(mode) else 1, p)
    buf += data + b'\0' * (-len(data) % 4)
buf += hdr(0, 0, 0, 1, 'TRAILER!!!')
buf += b'\0' * (-len(buf) % 512)
open(sys.argv[1], 'wb').write(buf)
print('%s: %d entries, %d bytes' % (sys.argv[1], len(ents), len(buf)))

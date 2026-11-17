#!/usr/bin/env python3
"""Build the provisioning initramfs (cpio newc, uncompressed).

Contents: Sony's own busybox + glibc (rootfs nflasha7), Sony's NAND kernel
modules (initrd /kmod), a5kcopy, and init.sh as /init.  Nothing here is
modified firmware: it is a factory-style programming environment that lets
Sony's MUMIN FTL format the emulated NAND and receive the logical image.

usage: mkinitramfs.py <out.cpio> [init-script (default init.sh)]
"""
import os, sys, stat, struct


def module_core_offset(ko, sym):
    """Offset of `sym` in the module's core image, as Linux 3.0's
    layout_sections() places it (module base from /proc/modules + this = VA).
    Checked against the a5000 1.10 nand.ko: g_dev at 0x1bd20, the
    hardcoded 0x3f03ad20 - base 0x3f01f000."""
    d = open(ko, 'rb').read()
    shoff, = struct.unpack_from('<I', d, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from('<3H', d, 0x2e)
    sh = [struct.unpack_from('<10I', d, shoff + i * shentsize) for i in range(shnum)]
    name = lambda s: d[sh[shstrndx][4] + s[0]:].split(b'\0', 1)[0].decode()
    A, W, X = 2, 1, 4                       # SHF_ALLOC, SHF_WRITE, SHF_EXECINSTR
    off, size = {}, 0
    for need, deny in ((X | A, 0), (A, W | X), (W | A, 0), (A, 0)):
        for i, s in enumerate(sh):
            n, fl = name(s), s[2]
            # the kernel drops SHF_ALLOC from .modinfo/__versions before layout;
            # .init* goes to the init image
            if ((fl & need) != need or fl & deny or i in off or n.startswith('.init')
                    or n in ('.modinfo', '__versions')):
                continue
            al = max(s[8], 1)
            size = -(-size // al) * al
            off[i] = size
            size += s[5]
    symtab = next(s for s in sh if s[1] == 2)       # SHT_SYMTAB
    strtab = sh[symtab[6]]
    for k in range(symtab[5] // 16):
        nm, val, _, _, _, shn = struct.unpack_from('<IIIBBH', d, symtab[4] + k * 16)
        if d[strtab[4] + nm:].split(b'\0', 1)[0] == sym.encode():
            return off[shn] + val
    raise SystemExit('%s: no symbol %s' % (ko, sym))


E =os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
# unpacked official updater: $A5K_FW, else <repo>/firmware, else ../firmware
FW = os.environ.get('A5K_FW') or (E + '/firmware' if os.path.isdir(E + '/firmware')
                                  else os.path.dirname(E) + '/firmware')
DEV = FW + '/unpacked/firmware.tar_unpacked/0700_part_image/dev'
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
# nand.ko device struct (MUMIN hand-off): offset in the module image, per
# build (a5000 1.10 0x1bd20, A6000 3.21 0x1bf38); loader.sh adds the base
ents.append(('kmod/nand.g_dev', stat.S_IFREG | 0o644,
             b'0x%x\n' % module_core_offset(INITRD + '/kmod/nand.ko', 'g_dev')))
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

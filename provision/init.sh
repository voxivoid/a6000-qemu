#!/bin/sh
# Factory-style NAND programming for the emulated A6000.
#  1. load Sony's NAND stack; nand.ko boot_mode=1 lets MUMIN format a blank
#     device ("erase all blks except bad blks") exactly like the updater does
#  2. copy the logical /dev/nflasha image exposed at PA 0x40000000 into it
export PATH=/bin
mount -t proc proc /proc
mount -t sysfs sys /sys
echo "a6k-provision: loading NAND stack"
insmod /kmod/devno.ko; echo "a6k-provision: devno rc=$?"
insmod /kmod/dmac.ko pri_mode_ch_0_3=1 pri_mode_ch_4_7=1
insmod /kmod/ldec.ko; echo "a6k-provision: ldec rc=$?"
insmod /kmod/lld.ko; echo "a6k-provision: lld rc=$?"
insmod /kmod/nand.ko boot_mode=1
echo "a6k-provision: nand.ko loaded rc=$?"
echo "a6k-provision: partitions:"; cat /proc/partitions
# initramfs has no pre-made nodes (the stock ext2 initrd ships them):
# create the whole-device node from /proc/partitions (nflasha0 = 252:0)
MAJ=$(grep nflasha0 /proc/partitions | while read a b c d; do echo $a; done)
[ -n "$MAJ" ] && mknod /dev/nflasha b $MAJ 0
if [ -e /dev/nflasha ]; then
    echo "a6k-provision: writing provision image to /dev/nflasha"
    a6kcopy 0x40000000 auto /dev/nflasha && sync && echo "a6k-provision: OK"
else
    echo "a6k-provision: /dev/nflasha missing"
fi
exec /bin/sh

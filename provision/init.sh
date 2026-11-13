#!/bin/sh
# Factory-style NAND programming for the emulated a5000.
#  1. load Sony's NAND stack; nand.ko boot_mode=1 lets MUMIN format a blank
#     device ("erase all blks except bad blks") exactly like the updater does
#  2. copy the logical /dev/nflasha image exposed at PA 0x40000000 into it
export PATH=/bin
mount -t proc proc /proc
mount -t sysfs sys /sys
echo "a5k-provision: loading NAND stack"
insmod /kmod/devno.ko
insmod /kmod/dmac.ko pri_mode_ch_0_3=1 pri_mode_ch_4_7=1
insmod /kmod/ldec.ko
insmod /kmod/lld.ko
insmod /kmod/nand.ko boot_mode=1
sleep 1
ls -l /dev | grep -i nflash
LEN=$(cat /proc/cmdline | sed -n 's/.*a5k.provlen=\([0-9a-fx]*\).*/\1/p')
if [ -n "$LEN" ] && [ -e /dev/nflasha ]; then
    echo "a5k-provision: writing $LEN bytes to /dev/nflasha"
    a5kcopy 0x40000000 $LEN /dev/nflasha && sync && echo "a5k-provision: OK"
else
    echo "a5k-provision: no image or no /dev/nflasha"
fi
exec /bin/sh

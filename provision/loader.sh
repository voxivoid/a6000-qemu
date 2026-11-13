#!/bin/sh
# Stage-1 loader: performs, with Sony's own NAND stack, the NAND-dependent
# duties of the Sony bootloader on every cold boot (see docs/BOOT.md):
#   - mount MUMIN and leave its state at PA 0x80000000 (nand.ko boot_mode=0
#     re-registers it), 128 KiB
#   - /setting/Backup.bin  -> the PA in its header (0x82EC0000)
#   - /system/av-cam.bin   -> PA 0x83541000 (liro.ko adr=)
#   - /system/vmlinux.bin, /system/initrd.img -> staging, then reset into
#     stage 2 through the board's loader descriptor at PA 0x80027000
export PATH=/bin
mount -t proc proc /proc
insmod /kmod/devno.ko
insmod /kmod/dmac.ko pri_mode_ch_0_3=1 pri_mode_ch_4_7=1
insmod /kmod/ldec.ko
insmod /kmod/lld.ko
insmod /kmod/nand.ko boot_mode=1
# device numbers as in the stock initrd's /dev:
# nflashaN = 252:N (raw), dnflashaN = 252:128+N (LZPT-decompressing)
mknod /dev/nflasha2 b 252 2
mknod /dev/dnflasha3 b 252 131
mkdir -p /mnt/setting /mnt/system
fail() { echo "a5k-loader: FAILED: $1"; cat /proc/partitions; exec /bin/sh; }
# nand.ko's device struct (fixed: same module load order as the stock initrd)
a5kcopy -mumin 0x3f03ad20 || fail mumin
mount -t vfat -o ro /dev/nflasha2 /mnt/setting || fail setting
a5kcopy -backup /mnt/setting/Backup.bin || fail backup
mount -t vfat -o ro /dev/dnflasha3 /mnt/system || fail "system mount"
a5kcopy -f2p /mnt/system/av-cam.bin 0x83541000 || fail av-cam
a5kcopy -stage2 /mnt/system/vmlinux.bin /mnt/system/initrd.img || fail stage2
# read-only mounts: no unmount needed (and umount of the LZPT device
# blocks); the reset discards stage 1 anyway
echo "a5k-loader: entering stage 2"
reboot -f

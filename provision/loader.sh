#!/bin/sh
# Stage-1 loader: performs, with Sony's own NAND stack, the NAND-dependent
# duties of the Sony bootloader on every cold boot (see docs/reference/BOOT.md):
#   - mount MUMIN and leave its state at PA 0x80000000 (nand.ko boot_mode=0
#     re-registers it), 128 KiB
#   - /setting/Backup.bin  -> the PA in its header (0x82EC0000), then the
#     loader-owned boot mode 0x51000F = 1 (COLD) in that RAM copy
#   - boot-mode decision (docs/reference/RE_UPDATER_BOOT.md): if /setting/updater/mode
#     exists, boot the updater core from nflasha1 (/boot/vmlinux.bin +
#     /boot/initrd.img at the addresses in /boot/loadaddr.txt); otherwise
#   - /system/av-cam.bin   -> PA 0x83541000 (liro.ko adr=)
#   - the \sabin\ rows of the bootloader load table (Backup 0xFA0080):
#     idt_cam.bin -> 0x83501000, sa_dfdet.bin -> 0x9EC00000
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
mknod /dev/nflasha1 b 252 1
mknod /dev/nflasha2 b 252 2
mknod /dev/dnflasha3 b 252 131
mkdir -p /mnt/setting /mnt/system /mnt/updater
fail() { echo "a6k-loader: FAILED: $1"; cat /proc/partitions; exec /bin/sh; }
# nand.ko's device struct g_dev = module base (/proc/modules) + its offset in
# this nand.ko build (kmod/nand.g_dev, computed by mkinitramfs.py).
# A6000 3.21 with the stock load order: 0x3f01f000 + 0x1bf38.
NBASE=$(sed -n 's/^nand .* Live \(0x[0-9a-f]*\).*/\1/p' /proc/modules)
GOFF=$(cat /kmod/nand.g_dev)
echo "a6k-loader: nand.ko base $NBASE + g_dev offset $GOFF"
[ -n "$NBASE" ] && [ -n "$GOFF" ] || fail "nand.ko g_dev"
a6kcopy -mumin $NBASE $GOFF || fail mumin
mount -t vfat -o ro /dev/nflasha2 /mnt/setting || fail setting
a6kcopy -backup /mnt/setting/Backup.bin || fail backup
# loader boot mode 1 = COLD (the file keeps the last real power-on's 2 = HOT).
# upm k_upm_setup_boot_factor maps it to g_bf+8 = 1, so av-cam's SA manager
# takes its cold branch (43AC9084) and frees boot unit 0x0400 (Backup
# 0x93000A); with HOT it never does and RAW unit 0x173762 cannot be placed
# (docs/reference/RE_STILL_CAPTURE.md 6, docs/reference/RE_COLD_BOOT_MODE.md).
# hypothesis: the Sony bootloader writes this on its cold path (no code).
# A HOT resume bypasses this loader and keeps 2 (docs/reference/RE_WARM_RESUME.md).
a6kcopy -bkset 0x0051000f 01 || fail "boot mode"
if [ -f /mnt/setting/updater/mode ]; then
    # updater core (nflasha1, plain FAT): "<kernel PA>,<initrd PA>"
    echo "a6k-loader: /setting/updater/mode set -> updater core (nflasha1)"
    mount -t vfat -o ro /dev/nflasha1 /mnt/updater || fail "updater mount"
    LA=$(cat /mnt/updater/boot/loadaddr.txt) || fail loadaddr
    KDST=$(echo "$LA" | sed 's/,.*//')
    IDST=$(echo "$LA" | sed 's/^[^,]*,//; s/[^0-9a-fA-Fx].*//')
    len() { sed -n 's/^a6kcopy: loaded \(0x[0-9a-f]*\) bytes.*/\1/p'; }
    KLEN=$(a6kcopy -f2p /mnt/updater/boot/vmlinux.bin 0x90000000 | len)
    ILEN=$(a6kcopy -f2p /mnt/updater/boot/initrd.img 0x90400000 | len)
    [ -n "$KDST" ] && [ -n "$IDST" ] && [ -n "$KLEN" ] && [ -n "$ILEN" ] ||
        fail "updater stage2"
    # descriptor {"A6KB", kpa, klen, ipa, ilen, "A6KX", kdst, idst},
    # magic word written last
    a6kcopy -w32 0x80027004 0x90000000 $KLEN 0x90400000 $ILEN \
        0x584b3641 $KDST $IDST || fail "updater desc"
    a6kcopy -w32 0x80027000 0x424b3641 || fail "updater desc"
    echo "a6k-loader: updater kernel $KLEN @$KDST, initrd $ILEN @$IDST"
else
    mount -t vfat -o ro /dev/dnflasha3 /mnt/system || fail "system mount"
    a6kcopy -f2p /mnt/system/av-cam.bin 0x83541000 || fail av-cam
    # bootloader load table, Backup prop 0xFA0080 (docs/reference/RE_RAW_DEVELOP.md
    # 3.2): its \sabin\ rows, i.e. idt_cam.bin -> 0x83501000 (IDT init
    # 4397BB1C/4397BAF8 checks u16 0x3D there, prop 0xFA0074) and
    # sa_dfdet.bin -> 0x9EC00000 (SA boot unit 0x0400, Backup 0x93000A).
    # The windows are passed to the board, which keeps the wbimem regions
    # they hit and presents hand-off wbi_memclr = 0 for stage 2
    # (docs/reference/RE_COLD_BOOT_MODE.md 5.4; hypothesis: the real cold-boot value).
    a6kcopy -loadtable /mnt/system || fail "load table"
    a6kcopy -stage2 /mnt/system/vmlinux.bin /mnt/system/initrd.img || fail stage2
fi
# read-only mounts: no unmount needed (and umount of the LZPT device
# blocks); the reset discards stage 1 anyway
echo "a6k-loader: entering stage 2"
reboot -f

#!/bin/sh
# Stage-1 loader: performs, with Sony's own NAND stack, the NAND-dependent
# duties of the Sony bootloader on every cold boot (see docs/BOOT.md):
#   - mount MUMIN and leave its state at PA 0x80000000 (nand.ko boot_mode=0
#     re-registers it), 128 KiB
#   - /setting/Backup.bin  -> the PA in its header (0x82EC0000), then the
#     loader-owned boot mode 0x51000F = 1 (COLD) in that RAM copy
#   - boot-mode decision (docs/RE_UPDATER_BOOT.md): if /setting/updater/mode
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
fail() { echo "a5k-loader: FAILED: $1"; cat /proc/partitions; exec /bin/sh; }
# nand.ko's device struct (fixed: same module load order as the stock initrd)
a5kcopy -mumin 0x3f03ad20 || fail mumin
mount -t vfat -o ro /dev/nflasha2 /mnt/setting || fail setting
a5kcopy -backup /mnt/setting/Backup.bin || fail backup
# loader boot mode 1 = COLD (the file keeps the last real power-on's 2 = HOT).
# upm k_upm_setup_boot_factor maps it to g_bf+8 = 1, so av-cam's SA manager
# takes its cold branch (43AC9084) and frees boot unit 0x0400 (Backup
# 0x93000A); with HOT it never does and RAW unit 0x173762 cannot be placed
# (docs/RE_STILL_CAPTURE.md 6, docs/RE_COLD_BOOT_MODE.md).
# hypothesis: the Sony bootloader writes this on its cold path (no code).
# A HOT resume bypasses this loader and keeps 2 (docs/RE_WARM_RESUME.md).
a5kcopy -bkset 0x0051000f 01 || fail "boot mode"
# Optional boot shoot-mode (docs/RE_MODE_DIAL.md): when the board is started
# with -M sony-a5000,shoot-mode=p|a|s|m|scn it appends a5kmodeset=<EE><DD> to
# the cmdline (EE = exposure-mode enum -> Backup 0x1070009, DD = mode-dial
# position -> Backup 0x10708BF).  The a5000 has no mode-dial hardware; the
# shooting mode is a soft dial persisted in Backup, so setting these two bytes
# in the RAM Backup copy boots the camera in that mode exactly as the on-screen
# mode-dial selector would have left it.  Absent token = stock (SCN default).
MS=$(sed -n 's/.*a5kmodeset=\([0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F]\).*/\1/p' /proc/cmdline)
if [ -n "$MS" ]; then
    EN=$(echo "$MS" | sed 's/^\(..\).*/\1/')
    DP=$(echo "$MS" | sed 's/^..\(..\)/\1/')
    a5kcopy -bkset 0x01070009 "$EN" || echo "a5k-loader: shoot-mode enum failed"
    a5kcopy -bkset 0x010708bf "$DP" || echo "a5k-loader: shoot-mode dial failed"
    echo "a5k-loader: shoot-mode enum=$EN dial=$DP"
fi
if [ -f /mnt/setting/updater/mode ]; then
    # updater core (nflasha1, plain FAT): "<kernel PA>,<initrd PA>"
    echo "a5k-loader: /setting/updater/mode set -> updater core (nflasha1)"
    mount -t vfat -o ro /dev/nflasha1 /mnt/updater || fail "updater mount"
    LA=$(cat /mnt/updater/boot/loadaddr.txt) || fail loadaddr
    KDST=$(echo "$LA" | sed 's/,.*//')
    IDST=$(echo "$LA" | sed 's/^[^,]*,//; s/[^0-9a-fA-Fx].*//')
    len() { sed -n 's/^a5kcopy: loaded \(0x[0-9a-f]*\) bytes.*/\1/p'; }
    KLEN=$(a5kcopy -f2p /mnt/updater/boot/vmlinux.bin 0x90000000 | len)
    ILEN=$(a5kcopy -f2p /mnt/updater/boot/initrd.img 0x90400000 | len)
    [ -n "$KDST" ] && [ -n "$IDST" ] && [ -n "$KLEN" ] && [ -n "$ILEN" ] ||
        fail "updater stage2"
    # descriptor {"A5KB", kpa, klen, ipa, ilen, "A5KX", kdst, idst},
    # magic word written last
    a5kcopy -w32 0x80027004 0x90000000 $KLEN 0x90400000 $ILEN \
        0x584b3541 $KDST $IDST || fail "updater desc"
    a5kcopy -w32 0x80027000 0x424b3541 || fail "updater desc"
    echo "a5k-loader: updater kernel $KLEN @$KDST, initrd $ILEN @$IDST"
else
    mount -t vfat -o ro /dev/dnflasha3 /mnt/system || fail "system mount"
    a5kcopy -f2p /mnt/system/av-cam.bin 0x83541000 || fail av-cam
    # bootloader load table, Backup prop 0xFA0080 (docs/RE_RAW_DEVELOP.md
    # 3.2): its \sabin\ rows, i.e. idt_cam.bin -> 0x83501000 (IDT init
    # 4397BB1C/4397BAF8 checks u16 0x3D there, prop 0xFA0074) and
    # sa_dfdet.bin -> 0x9EC00000 (SA boot unit 0x0400, Backup 0x93000A).
    # The windows are passed to the board, which keeps the wbimem regions
    # they hit and presents hand-off wbi_memclr = 0 for stage 2
    # (docs/RE_COLD_BOOT_MODE.md 5.4; hypothesis: the real cold-boot value).
    a5kcopy -loadtable /mnt/system || fail "load table"
    a5kcopy -stage2 /mnt/system/vmlinux.bin /mnt/system/initrd.img || fail stage2
fi
# read-only mounts: no unmount needed (and umount of the LZPT device
# blocks); the reset discards stage 1 anyway
echo "a5k-loader: entering stage 2"
reboot -f

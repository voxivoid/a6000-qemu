# data/ — inputs that are not in git

Nothing here is published: the files are Sony firmware or derived from it, or come from your own camera.
Paths are relative to the repository root.

**You need an A6000 that you own.** Sony's updater ships the firmware but not the per-unit data written at the
factory: the settings store `Backup.bin` (the emulator reads the camera's button map and boot load table from it),
factory calibration, and the updater core. A dump of someone else's camera must not be used or shared: it is that
owner's personal data (serial number, device name, Wi-Fi settings, face registrations) and Sony's.

| File | Source | Device-specific? | Made by |
|---|---|---|---|
| `firmware/unpacked/…` | Sony's official updater `Update_ILCE6000V321.exe` | no | step 1 |
| `data/wbi/ram.bin`, `data/wbi/map.tsv` | factory warm-boot image (nflasha5) inside the updater | no | step 2 |
| `data/handoff_80021000.bin` | same image, PA 0x80021000 (4 KiB) | no | step 2 |
| `data/submicro_frames.bin` | same image, PA 0x8334417C (256 B), power-off flag cleared, CRC recomputed | no | step 2 |
| `dumps/<your dump>` | full NAND of **your own** A6000 | **yes** | step 3 |
| `state/a6000.nand` | emulated NAND (Sony FTL format) | yes | step 4 |
| `provision/*.cpio` | initramfs built from the updater's kernel modules | no | `run.sh` / `provision.sh` (automatic) |

## 1. Official updater (no camera needed)
1. Download the ILCE-6000 system software update **version 3.21** for Windows from Sony's support site
   (`Update_ILCE6000V321.exe`; the emulator targets 3.21 only).
2. Get [fwtool.py](https://github.com/ma1co/fwtool.py) (MIT) next to this repository and its dependencies:
   ```sh
   git clone https://github.com/ma1co/fwtool.py ../fwtool.py
   python3 -m venv ../fwtool.py/.venv && ../fwtool.py/.venv/bin/pip install pycryptodomex pyyaml
   ```
3. Current Sony downloads wrap the firmware in an archive that fwtool rejects (`Unknown exe file`). Cut the
   `FirmwareData_ILCE6000V321.dat` out of the exe: it starts at the DAT magic `\x89UFU\r\n\x1a\n` (file offset 524866),
   and its size is the u32 just before its UTF-16 name in the exe's archive table:
   ```sh
   python3 - <<'EOF'
   import struct
   d = open('firmware/Update_ILCE6000V321.exe', 'rb').read()
   name = 'FirmwareData_ILCE6000V321.dat'.encode('utf-16-le')
   size = struct.unpack('<I', d[d.index(name) - 4:d.index(name)])[0]
   off = d.index(b'\x89UFU\r\n\x1a\n')
   open('firmware/FirmwareData_ILCE6000V321.dat', 'wb').write(d[off:off + size])
   EOF
   ../fwtool.py/.venv/bin/python ../fwtool.py/fwtool.py unpack -f firmware/FirmwareData_ILCE6000V321.dat -o firmware/unpacked
   ```
   The scripts expect `firmware/unpacked/firmware.tar_unpacked/0700_part_image/dev/nflasha3_unpacked_unpacked/vmlinux.bin`
   (and `nflasha5`, `nflasha7…`, `nflasha15…`). Use `A6K_FW=<dir>` if the tree lives elsewhere.

## 2. Warm-boot-image files (no camera needed)
```sh
../fwtool.py/.venv/bin/python tools/mkdata.py --fwtool ../fwtool.py
```
Decompresses the factory warm-boot image (≈173 MB RAM image, 1164 sections) into `data/wbi/` and cuts out the hand-off
page and the sub-micro frames. These are Sony's factory state, not your camera's.

## 3. NAND dump of your own camera (camera needed, read-only)
The bootloader area, the updater core (nflasha1) and the settings partition (nflasha2: `Backup.bin`, calibration) are
not in the updater, so the emulator needs a full NAND image.

Either a root shell on the camera ([OpenMemories-Tweak](https://github.com/ma1co/OpenMemories-Tweak) installed via
[Sony-PMCA-RE](https://github.com/ma1co/Sony-PMCA-RE), telnet):
```sh
# on the camera (busybox shell); the SD card is only visible through the Android FUSE mount, 8.3 names
dd if=/dev/nflasha of=/android/mnt/sdcard/NAND.BIN bs=131072
busybox md5sum /dev/nflasha /android/mnt/sdcard/NAND.BIN   # both must match
```
or the Sony-PMCA-RE updater shell over USB: `pull /dev/nflasha <dir>\nflasha.bin`.

Keep the dump (≈483 MiB) under `dumps/` (git-ignored) and check its MD5 again on the computer.

Safety:
- This only **reads** the flash. Never run a `dd` or any other command with `of=/dev/nflasha*`: writing the NAND can
  brick the camera.
- The camera has little free RAM (≈20 MB). Close other apps and don't start the dump during an app install.
- Keep the dump as a backup of your camera, and **never share it**.

## 4. Emulated NAND
```sh
./provision.sh dumps/<your dump>                 # -> state/a6000.nand (2nd argument: another output path)
cp --reflink=auto state/a6000.nand state/a6000.nand.pristine
```
Keep the untouched copy: a failed boot can erase the emulated NAND. The emulator writes only to `state/a6000.nand`;
your camera is never touched.

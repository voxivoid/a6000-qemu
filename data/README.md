# data/ — inputs that are not in git

Nothing here is published: the files are Sony firmware or derived from it, or come
from your own camera. Paths are relative to the repository root.

| File | Source | Device-specific? | Made by |
|---|---|---|---|
| `firmware/unpacked/…` | Sony's official updater `Update_ILCE5000V110.exe` | no | step 1 |
| `data/wbi/ram.bin`, `data/wbi/map.tsv` | factory warm-boot image (nflasha5) inside the updater | no | step 2 |
| `data/handoff_80021000.bin` | same WBI, PA 0x80021000 (4 KiB) | no | step 2 |
| `data/submicro_frames.bin` | same WBI, PA 0x8334417C (256 B), power-off flag cleared, CRC recomputed | no | step 2 |
| `data/NAND.BIN` | full NAND of **your own** camera | **yes** | step 3 |
| `state/a5000.nand` | emulated NAND (Sony FTL format) | yes | `./provision.sh` |
| `provision/*.cpio` | initramfs built from the updater's kernel modules | no | `run.sh` / `provision.sh` (automatic) |

## 1. Official updater (no camera needed)
1. Download the ILCE-5000 firmware update **version 1.10** from Sony's support site
   (Windows `Update_ILCE5000V110.exe`; the emulator targets 1.10 only).
2. Unpack it with [fwtool.py](https://github.com/ma1co/fwtool.py) (MIT):
   ```sh
   git clone https://github.com/ma1co/fwtool.py ../fwtool.py
   python3 ../fwtool.py/fwtool.py unpack -f Update_ILCE5000V110.exe -o firmware/unpacked
   ```
   fwtool unpacks the nested images into `*_unpacked` directories. The scripts expect
   `firmware/unpacked/firmware.tar_unpacked/0700_part_image/dev/nflasha3_unpacked_unpacked/vmlinux.bin`
   (and `nflasha5`, `nflasha7…`, `nflasha15…`). Use `A5K_FW=<dir>` if the tree lives elsewhere.

## 2. Snapshot-derived files (no camera needed)
```sh
python3 tools/mkdata.py --fwtool ../fwtool.py
```
Decompresses the factory WBI (127 MB RAM image, 1073 sections) into `data/wbi/` and cuts out
the hand-off page and the sub-micro frames. The camera itself resumes from this same image,
so the values are Sony's factory state, not your camera's.

## 3. NAND dump of your own camera (camera needed, read-only)
The bootloader area, the updater core (nflasha1) and the settings partition (nflasha2:
`Backup.bin`, calibration) are not in the updater, so the emulator needs a full NAND image.

You need a root shell on the camera. The usual route is
[OpenMemories-Tweak](https://github.com/ma1co/OpenMemories-Tweak) (installed via
[Sony-PMCA-RE](https://github.com/ma1co/Sony-PMCA-RE)), which can start a telnet server. Then:
```sh
# on the camera (busybox shell); the SD card is only visible through the Android FUSE mount, 8.3 names
dd if=/dev/nflasha of=/android/mnt/sdcard/NAND.BIN bs=131072
busybox md5sum /dev/nflasha /android/mnt/sdcard/NAND.BIN   # both must match
```
Copy `NAND.BIN` (≈483 MB) to `data/NAND.BIN` and check the MD5 again on the computer.
Then run `./provision.sh` once.

Safety:
- This only **reads** the flash. Never run a `dd` or any other command with `of=/dev/nflasha*`:
  writing the NAND can brick the camera.
- The camera has little free RAM (≈20 MB). Close other apps and don't start the dump during
  an app install.
- Keep the dump as a backup of your camera, and **never share it**: it contains your camera's
  settings, calibration and identifiers (serial number, Wi-Fi settings).

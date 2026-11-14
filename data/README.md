# data/ — device-specific inputs (not in git)

These files come from the owner's own camera and are not published:

| file | content | how to produce |
|---|---|---|
| `handoff_80021000.bin` | 4 KiB bootloader hand-off page (PA 0x80021000) | copy from a RAM snapshot of your own a5000 (`tools/wbi.py`) |
| `NAND.BIN` | full NAND dump of your own camera (input of `provision.sh`) | read-only dump from your camera |
| `wbi/` (`map.tsv`, `ram.bin`) | RAM snapshot used by `tools/wbi.py` | from your camera's WBI hibernation image |
| `submicro_frames.bin` | 256 B: sub-micro slot-A + status frame (snapshot PA 0x8334417C), power-off flag cleared, CRC recomputed | see `docs/RE_SUBMICRO_LINK.md` |

The firmware itself (`Update_ILCE5000V110.exe` → unpacked images under `firmware/`, or `A5K_FW`), the provisioned NAND (`state/`) and the
initramfs built from it (`provision/loader.cpio`, made by `provision/mkinitramfs.py`) are also local only.

# data/ — device-specific inputs (not in git)

These files come from the owner's own camera and are not published:

| file | content | how to produce |
|---|---|---|
| `handoff_80021000.bin` | 4 KiB bootloader hand-off page (PA 0x80021000) | copy from a RAM snapshot of your own a5000 (`tools/wbi.py`) |
| `submicro_frames.bin` | 256 B: sub-micro slot-A + status frame (snapshot PA 0x8334417C), power-off flag cleared, CRC recomputed | see `docs/RE_SUBMICRO_LINK.md` |

The firmware itself (`Update_ILCE5000V110.exe` → unpacked images), the provisioned NAND (`state/`) and the
initramfs built from it (`provision/loader.cpio`, made by `provision/mkinitramfs.py`) are also local only.

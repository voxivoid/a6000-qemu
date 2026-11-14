# a5000 full-system emulator (`sony-a5000` QEMU machine)

**English** | [한국어](README_KO.md)

Goal: run the official Sony ILCE-5000 (α5000) firmware 1.10 **unmodified**, without the real camera, behaving like the real camera.

## Layout
| Path | Contents |
|---|---|
| `qemu/` | **Not in this repository** (git-ignored local checkout, see *Getting QEMU* below). QEMU v10.1.0 + the a5000 machine (`hw/arm/sony_a5000.c`, `hw/timer/cxd4115_timer.c`, `hw/misc/cxd90014_*.c`, `hw/block/cxd90014_nand.c`, `hw/display/{cxd90014_vph,dmp_smaph,dmp_shader}.c`, Cortex-A5 in `target/arm/tcg/cpu32.c`) |
| `qemu-overlay/` | **All new QEMU source files** (machine, device models, GPU), at their QEMU paths — read the emulator code here |
| `patches/qemu-v10.1.0-sony-a5000.patch` | Only the small edits to existing upstream files (build glue, Cortex-A5, A9MPCore, L2C-310) |
| `setup-qemu.sh` | Creates `qemu/` = upstream v10.1.0 + overlay + patch |
| `run.sh` | Boots the stock kernel + initrd (stock `console=ttynull` by default; `A5K_DEBUG_CONSOLE=1` = serial kernel console for boot debugging) |
| `provision/` | Factory-style NAND programming initramfs (the Sony FTL formats an empty NAND, then writes your `NAND.BIN`) |
| `tools/` | `mkdata.py` (build `data/` from the updater), `a5ktrace.c` (TCG trace plugin, build: `cc -shared -fPIC -o tools/liba5ktrace.dylib tools/a5ktrace.c -Iqemu/include/qemu $(pkg-config --cflags --libs glib-2.0) -undefined dynamic_lookup`), `mon.py` (HMP), `klog.py` (Sony klog ring), `wbi.py` (WBI snapshot lookup / page walk), `sym.py` (kernel symbols) |
| `re/` | Kernel symbols (`kernel_syms.txt`). The ELF rebuild (`vmlinux.elf`) and real-camera logs stay local (not published) |
| `docs/` | Reverse-engineering notes (English). `docs/HARDWARE.md` = SoC model with the source of every value (V/S/H) |

All commands below run from the repository root.

## Inputs you provide (not in this repository)
| Path | What |
|---|---|
| `firmware/unpacked/...` | Sony's official updater `Update_ILCE5000V110.exe`, unpacked (fwtool layout; or set `A5K_FW`) |
| `data/NAND.BIN` | NAND dump of **your own** camera (or pass the path to `provision.sh`) |
| `data/*.bin`, `data/wbi/` | Factory RAM snapshot, hand-off page and sub-micro frames, generated from the updater by `tools/mkdata.py` |

Step-by-step instructions: [`data/README.md`](data/README.md).

## Getting QEMU
The `qemu/` directory is **not published**. It is rebuilt from upstream QEMU `v10.1.0` plus this repository:
```sh
./setup-qemu.sh     # clone v10.1.0 into qemu/, copy qemu-overlay/, apply the patch
```
Developers working in `qemu/` (branch `sony-a5000`) run `tools/export-qemu.sh` after each QEMU commit to refresh `qemu-overlay/` and the patch.
Camera buttons from the host: arrow keys / Enter = wheel and centre, `M` = MENU, `P` = PLAY, `R` = MOVIE, Delete = DELETE (or HMP `sendkey`). SD card: `-drive if=sd` / `A5K_SD`.

## Status (2026-11-14)
- Note: the debug serial console (`A5K_DEBUG_CONSOLE=1`) changes the stock cmdline and shares UART0 with av-cam's monitor; the UI is only tested without it
- ✅ Quad Cortex-A5 (VFPv4-D16), GIC, L2C-310, CXD4115 timers/WDT, SYSCTL, PL011, clock generator, USB OTG/HDC reset, BOSS DMAC
- ✅ NAND: ARM7 NAND firmware HLE (mailbox, LDEC LZ77) + raw NAND backing file; `provision.sh` writes a NAND dump through the Sony FTL
- ✅ Two-stage boot like the real camera (`docs/BOOT.md`): the bootloader's NAND work is replayed through the Sony NAND stack, then the **kernel read from NAND** boots
- ✅ Stock init → compressed rootfs and /usr mounted → 33 kernel modules → Android → **LIRO starts the av-cam.bin camera engine** → im.elf creates the input devices and initialises GLES
- ✅ Engine hardware: SYSV frame timer, VPH vsync 60 Hz (IRQ 98/101/102), SIO, GPIO, ADC → VDF programs the 640×480 panel
- ✅ av-cam memory DMA, sub-microcontroller HLE (transport + recorded frame replay, `docs/RE_SUBMICRO_LINK.md`), camera timing event block 0xF3C30000 (engine frame 29.97 fps + 1 MHz comparators)
- ✅ GIC SPI targets from the real camera → CPU1-3 tick, RCU healthy → stage i0/i1/i2 and appFw initialised
- ✅ BOSS LDEC LZ77 decoder (memory mode, /dev/ldec), per-CPU hwtimers free-running → VPH programs the panel OSD layer every frame
- ✅ **GPU P3D command-list execution** (decoder, rasteriser, textures, blending, `docs/RE_GPU_P3D_CMD.md`) + shader interpreter (`docs/RE_GPU_P3D_SHADER.md`) + RGBA4444 OSD → **the stock UI renders in the QEMU window**
- ✅ SD card: sub-micro slot switch model → stock media stack initialises the card and reads FAT
- ✅ Camera buttons (`docs/RE_ENGINE_TICK.md` §11): wheel/centre = ADC ladder ch 0x0D, DELETE = ch 0x0C, MOVIE = GPIO P10.10, MENU/PLAY = sub-micro slot A byte 28
- ✅ First-boot setup completes: area (world map) → date/time → PlayMemories notice → shooting mode (no SD: lens warning → shooting-screen OSD)
- ⏳ With SD: blank screen in what is probably the image-database repair view; menu/wheel unresponsive in shooting mode — under investigation
- ⏳ Some Rigel compositing shader opcodes, depth/stencil, lines/points, mipmaps not implemented (`docs/RE_ENGINE_TICK.md`)
- ⬜ Sensor/ISP (live view), lens, audio, RTC

## Firmware and legal notice
- This repository contains **no Sony firmware, binaries or device dumps.** To run it you need a NAND dump extracted from **a camera you own** and files from Sony's publicly distributed official updater (see `data/README.md`). Do not upload those files to this repository or its issues.
- This project is **not affiliated with, endorsed or sponsored by Sony Group Corporation.** "Sony", "α", "ILCE" and "BIONZ" are trademarks of their respective owners.
- The documents are reverse-engineering observations made for interoperability (emulation). Analysing or bypassing protection features such as battery authentication is out of scope.
- Work on real hardware is at your own risk. This emulator never writes to a real camera.

## License
- QEMU code (`qemu-overlay/`, `patches/`), `tools/`, `provision/` and scripts: **GPL-2.0-or-later** (`LICENSE`, same as QEMU)
- Documentation (`docs/`, `*.md`): **CC BY 4.0**
- QEMU itself remains under its original authors' licenses (`qemu/COPYING`, `qemu/LICENSE`).

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
| `run.sh` | Boots the stock kernel + initrd (stock `console=ttynull` by default; `A5K_SHELL=<port>` = root shell on UART2 as a TCP socket, `A5K_DEBUG_CONSOLE=1` = the same console on stdio) |
| `provision/` | Factory-style NAND programming initramfs (the Sony FTL formats an empty NAND, then writes your `NAND.BIN`) |
| `tools/` | `mkdata.py` (build `data/` from the updater), `a5ktrace.c` (TCG trace plugin, build: `cc -shared -fPIC -o tools/liba5ktrace.dylib tools/a5ktrace.c -Iqemu/include/qemu $(pkg-config --cflags --libs glib-2.0) -undefined dynamic_lookup`), `mon.py` (HMP), `klog.py` (Sony klog ring), `a5kshell.py` (guest shell + file push/pull), `wbi.py` (WBI snapshot lookup / page walk), `sym.py` (kernel symbols) |
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
SD card: `-drive if=sd` / `A5K_SD`.
Machine options, appended to `run.sh` as extra `-M` arguments (e.g. `./run.sh -M rtc-lost=on`): `rtc-lost=on` = clock backup lost (first-boot area/date/time setup); `lens=none|ekit` = empty E-mount (default) or kit lens (power + handshake only); camera clock = QEMU RTC (`-rtc base=...`).

### Keys (QEMU window)
| Host key | Camera button | Emulated hardware path |
|---|---|---|
| Enter (or keypad Enter) | control wheel centre (OK) | ADC unit 1 ch 1 (ch 0x0D) ladder, level 45 |
| ↑ / ← / ↓ / → | control wheel up / left / down / right | same ladder, levels 182 / 356 / 520 / 686 |
| Delete / Backspace (Mac: ⌫) | DELETE (trash) | ADC unit 1 ch 0 (ch 0x0C) ladder, level 45 |
| `M` | MENU | sub-micro slot A frame byte 28 bit 2 |
| `P` | PLAY (playback) | sub-micro slot A frame byte 28 bit 1 |
| `R` | MOVIE (record) | GPIO P10.10, active low |
| `O` | power lever (slide switch: each press flips OFF / ON; starts ON) | sub-micro slot A frame byte 28 bit 0 (1 = ON); while the camera is off, lever ON powers it up |
| `S` | shutter half press (S1, key 0x1F1) | GPIO P0.1 + P0.11 low (active low) |
| `Shift+S`, or `Space` | shutter full press (S1 + S2, key 0xA7) | additionally GPIO P1.5 + P0.13 low |
| `T` / `W` | zoom lever tele / wide (slow) | ADC unit 1 ch 3 (ch 0x0F), level 478 / 400 (value 1 / 0x8001) |
| `Shift+T` / `Shift+W` | zoom lever tele / wide (fast) | same channel, level 162 / 301 (value 2 / 0x8002) |
| — | control wheel rotation | not wired yet (av-cam JogTpu counter) |

Keys are also accepted from the HMP monitor: `sendkey ret`, `sendkey down`, `sendkey m`, ...
QEMU's own hot keys: Ctrl+Opt+G releases the mouse, Ctrl+Opt+1 / 2 switch between the panel (console 0) and the debug frame-buffer view.
Keys pressed in the first ~20 s after the first-boot dialog appears are ignored by the firmware (input service not active yet).

### USB (virtual cable)
The USB device controller (`cxd90014-usbhdc`, `docs/RE_USB_DEVICE_CONTROLLER.md`) serves the camera's real USB gadget over
TCP with the "A5KU" protocol (`docs/RE_USB_HOST_PATH.md` §5). A connected TCP client is a plugged cable (VBUS on); when the
client disconnects the cable is pulled. Without a client the camera sees no cable, as before. The cable stays plugged across
a guest reboot.
```sh
./run.sh -M usb-socket=127.0.0.1:7643        # or: -chardev socket,id=usb0,host=127.0.0.1,port=7643,server=on,wait=off -global cxd90014-usbhdc.chardev=usb0
```
Host side: Sony-PMCA-RE with the `socket` backend from `tools/pmca-socket/` (see its README):
```sh
python3 pmca-console.py info -d socket       # A5K_USB=host:port (default 127.0.0.1:7643), A5K_USB_TIMEOUT=ms, A5K_USB_DEBUG=1
```

### Guest shell (Track B)
A root shell inside the running guest, next to the stock UI (`docs/RE_GUEST_SHELL.md`). `A5K_SHELL=<port>` sets the machine
option `console=ttyAM1`: only the `console=ttynull` token of the stock cmdline becomes `console=ttyAM1 amba2.console=1`, and
UART2 (unused by the stock system) is served as a TCP socket. Stock init then runs its own root `ash` there; the UI comes up
as usual. Without the option the cmdline is unchanged.

> **Emulator-only developer aid.** The real camera has no accessible UART: on real hardware every CFW / deployment step
> goes over USB. Use this shell for inspecting and iterating inside the emulator; test real deployment flows over the
> USB virtual cable (`pmca-console`).
```sh
A5K_SHELL=46310 ./run.sh                      # = -M console=ttyAM1,shell-socket=127.0.0.1:46310 -serial null
python3 tools/a5kshell.py --port 46310        # interactive (Ctrl-] quits); --job-control makes Ctrl-C work
python3 tools/a5kshell.py --port 46310 push hello            # -> /android/a5k/hello (tmpfs, exec ok), md5 checked
python3 tools/a5kshell.py --port 46310 run '/android/a5k/hello; echo rc=$?'
python3 tools/a5kshell.py --port 46310 pull /android/a5k/out.bin out.bin
```
- Transfers use stock busybox only (`stty raw` + `cat`, ~50 KB/s) and are checked with md5. Push targets outside `/android`
  are refused (needs `--force`); `/android` is a tmpfs, so pushed files are lost at power-off.
- Keep the shell socket drained: UART output is written with `qemu_chr_fe_write_all()`, so a client that stays connected
  but stops reading blocks the vCPU that prints (with the BQL held) and with it the whole machine. Leave `a5kshell.py`
  running (it keeps reading) or disconnect.
- Other machine options: `console=ttynull|ttyAM1|ttyAM0`, `debug-console=on` (= `console=ttyAM1`; with `run.sh`:
  `A5K_DEBUG_CONSOLE=1` puts it on stdio). Without `shell-socket=` UART2 is the third `-serial`
  (`-serial null -serial null -serial tcp:...`). `console=ttyAM0` (`A5K_DEBUG_CONSOLE=ttyAM0`) is for early-boot logs only:
  UART0 belongs to av-cam's monitor and the UI does not start.
- Host load: do not run the emulator at background priority (e.g. `taskpolicy -b`) or many instances on one busy host.
  A starved guest CPU0 exposes a firmware race that real hardware never hits (kernel oops `PC is at 0x4394aa4c`, task
  `liro-kliro_113`; open, `docs/RE_ENGINE_OOPS.md`).
- The firmware's idle power save turns the camera off after about 5 minutes without key input (VM suspended until the
  power lever `O`, `S`/`M`/`P` or HMP `system_wakeup` powers it on again). Like the real camera this is a warm off: the
  next power-on resumes the kept RAM image (HOT resume, `warm-resume=on`), and the debug shell survives it (the debug
  console runs with `loglevel=4` to avoid a Sony UART driver deadlock, `docs/RE_UART_RESUME.md`); with `-M warm-resume=off` it cold-boots and `/android/a5k` is lost. Send a harmless
  key over HMP now and then, or work within that window.

## Status (2026-11-15)
- ✅ Guest root shell next to the stock UI (`docs/RE_GUEST_SHELL.md`): `console=ttyAM1` + `shell-socket=` (UART2), `tools/a5kshell.py` push/pull with md5 check; a static ARM binary pushed to `/android/a5k` runs while the shooting screen stays up. `debug-console=on` now maps to UART2 (UART0 = av-cam's monitor port; `console=ttyAM0` warns that the UI will not start)
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
- ✅ Sub-micro RTC (`docs/RE_RTC.md` §9): camera clock = host time (`-rtc base=`), a date set in the menu is acknowledged and kept for the session → a normal boot goes straight to the shooting screen
- ✅ First-boot setup (`rtc-lost=on`) completes: area (world map) → date/time → PlayMemories notice → shooting-screen OSD
- ⏳ Lens (`docs/RE_LENS_EMOUNT.md` §8): `lens=none` = the firmware's empty-mount idle state; `lens=ekit` = lens power acknowledged + handshake steps 0–1, then stops before the UART1 lens protocol (not modelled)
- ✅ Shutter idle level fixed (the camera had booted with S1/S2 held): MENU and the wheel centre button now work in shooting mode; shutter and zoom lever wired (`docs/RE_ENGINE_TICK.md` §12)
- ⏳ With SD: the image-database repair view (AUTOREPAIR) stays blank, and Format from the menu hangs; neither issues an SD command — under investigation
- ✅ GPU: all 779 draws from first boot to the shooting screen render (Rigel compositing included); depth/stencil, mipmaps, border wrap added and tested with synthetic lists (`tools/test_dmp_p3d.py`, `tools/test_dmp_shader.c`)
- ⏳ GPU: geometry programs (real point/line drawing), 14 GLES1-package shader opcodes, ETC1, depth/mip memory layouts unconfirmed (`docs/RE_GPU_P3D_CMD.md` §7)
- ✅ Updater mode: with `/setting/updater/mode` in the emulated NAND the stage-1 loader boots the updater core (nflasha1) like the bootloader; it reaches a root shell and the UFP USB wait (`docs/RE_UPDATER_BOOT.md`)
- ⏳ Camera front end: sensor writer (P2S, IRQ 38), front-end frame IRQs, AIF channel groups (IRQs 40–50) modelled; capture errors gone, live view still black (no YC-output request yet, `docs/RE_SENSOR_ISP.md` §7)
- ✅ SA DSP cores: null model (`cxd90014_sa.c`) completes the imager's defect-table job → the engine reaches monitoring; the shooting screen now shows the exposure readout (shutter/aperture scales). Live view itself still stops before YC state 2 (`docs/RE_LIVEVIEW_REQUEST.md` §9); PLAY and SD work: no-card playback view; with a card the image-database dialog creates the database on the card (first SD writes) (`docs/RE_PLAYBACK_SWITCH.md`)
- ✅ Live-view path: with SYSV as the master V clock the firmware enables the panel YUV layer and programs ch51 every frame (`docs/RE_LIVEVIEW_PIPELINE.md`). ch51 is an 8-bit luma plane (1024×516) scaled to the panel; nothing fills it yet (no ISP model), so it is black like a capped lens; `-global cxd90014-aif.lv-source=bars|ramp|file:PATH.pgm` writes a test luma frame into that buffer at the resize output (debug, `docs/RE_LIVEVIEW_IMAGE.md`)
- ✅ USB device (`docs/RE_USB_DEVICE_CONTROLLER.md` §9, `docs/RE_USB_HOST_PATH.md` §9): F_USB20HDC + HDMAC + VBUS glue model with a TCP virtual cable ("A5KU"); the camera enumerates (MTP 054C:08D3), `pmca-console info -d socket` prints ILCE-5000 / 1.10, `install -f <apk>` installs an app that then appears in the Application List and starts (`docs/RE_USB_APP_INSTALL.md`), and the updater core enumerates as 054C:03E2 and runs the PMCA updater shell. After SWITCH_MODE the camera powers off like the real one (sub-micro power-off handshake, DDR controller, p0.10 power-off line) and the sub-micro power-cycles it into the updater: `pmca-console updatershell -d socket` reaches the updater shell with no manual reset (second PMCA run with `-m ILCE-5000`, `docs/RE_REBOOT.md`). Machine option `poff=auto|restart|off` (default `auto`) decides what the power-off line does: `auto` follows the off data the CPU sent in the slot-A TX frame (+0x15 = 0x20 → power cycle, otherwise the camera stays off = VM suspended, `info status` → `paused (suspended)`; power lever `O`, after an idle power save also `S`/`M`/`P`, or HMP `system_wakeup` powers it on again, `docs/RE_REBOOT.md` B.7). A warm off (idle power save, power lever) with `sus_error` 0 keeps DDR and the next power-on resumes the running system in place like the real camera's HOT boot (same screen, uptime continues, no stage 1; CPU fix: MMU enable takes effect for instruction fetch at the next branch); `-M warm-resume=off` restores the cold power-on (`docs/RE_WARM_RESUME.md` §7).
- ⬜ Sensor/ISP (live view), lens serial protocol, audio

## Firmware and legal notice
- This repository contains **no Sony firmware, binaries or device dumps.** To run it you need a NAND dump extracted from **a camera you own** and files from Sony's publicly distributed official updater (see `data/README.md`). Do not upload those files to this repository or its issues.
- This project is **not affiliated with, endorsed or sponsored by Sony Group Corporation.** "Sony", "α", "ILCE" and "BIONZ" are trademarks of their respective owners.
- The documents are reverse-engineering observations made for interoperability (emulation). Analysing or bypassing protection features such as battery authentication is out of scope.
- Work on real hardware is at your own risk. This emulator never writes to a real camera.

## License
- QEMU code (`qemu-overlay/`, `patches/`), `tools/`, `provision/` and scripts: **GPL-2.0-or-later** (`LICENSE`, same as QEMU)
- Documentation (`docs/`, `*.md`): **CC BY 4.0**
- QEMU itself remains under its original authors' licenses (`qemu/COPYING`, `qemu/LICENSE`).

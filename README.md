# Sony α6000 full-system emulator (`sony-a6000` QEMU machine)

Goal: run the official Sony ILCE-6000 (α6000) firmware 3.21 **unmodified**, without the real camera, behaving like the real camera.

## Layout
| Path | Contents |
|---|---|
| `qemu/` | **Not in this repository** (git-ignored local checkout, see *Getting QEMU*). QEMU v10.1.0 + the A6000 machine (`hw/arm/sony_a6000.c`, `hw/timer/cxd4115_timer.c`, `hw/misc/cxd90014_*.c`, `hw/block/cxd90014_nand.c`, `hw/display/{cxd90014_vph,dmp_smaph,dmp_shader}.c`, Cortex-A5 in `target/arm/tcg/cpu32.c`) |
| `qemu-overlay/` | **All new QEMU source files** (machine, device models, GPU), at their QEMU paths — read the emulator code here |
| `patches/qemu-v10.1.0-sony-a6000.patch` | Only the small edits to existing upstream files (build glue, Cortex-A5, A9MPCore, L2C-310) |
| `setup-qemu.sh` | Creates `qemu/` = upstream v10.1.0 + overlay + patch |
| `run.sh` | Boots the camera from the emulated NAND (stock `console=ttynull`; `A6K_SHELL=<port>` = root shell on UART2 as a TCP socket, `A6K_DEBUG_CONSOLE=1` = the same console on stdio) |
| `provision/` | Factory-style NAND programming and the stage-1 loader (initramfs with Sony's own NAND stack) |
| `tools/` | `camui.py` + `camui.html` (camera-body web UI), `mkdata.py` (build `data/` from the updater), `mon.py` (HMP), `klog.py` (Sony klog ring), `a6kshell.py` (guest shell + file push/pull), `wbi.py` (warm-boot image lookup), `sym.py` (kernel symbols), `mksd.py` (SD image), `a6ktrace.c` (TCG trace plugin), `pmca-socket/` (USB virtual-cable backend for Sony-PMCA-RE), `test_dmp_*` (GPU tests) |
| `docs/A6000_ROADMAP.md` | Bring-up plan and **what is verified on the A6000** |
| `docs/reference/` | Background hardware notes from the platform analysis this project grew out of; **not A6000-verified**, firmware addresses there are for an older build (see `docs/reference/README.md`) |

All commands below run from the repository root.

## Inputs you provide (not in this repository)
**You must own an A6000 and supply a NAND dump of your own camera.** There is no generic NAND: the settings store
(`Backup.bin`: button map, boot load table, model and region data), the factory calibration and the updater core exist
only on a real camera and are not in Sony's updater. The dump also holds personal data (serial number, device name,
Wi-Fi settings, face registrations). Keep it local and never share or commit it; every path that holds it is git-ignored.

| Path | What |
|---|---|
| `firmware/unpacked/...` | Sony's official ILCE-6000 updater **3.21**, unpacked with fwtool (current Sony downloads need one extra step, `data/README.md` §1) |
| `dumps/` (e.g. `dumps/run2/nflasha.bin`) | full NAND dump of **your own** A6000 (`/dev/nflasha`, read-only) |
| `data/` | hand-off page and sub-micro frames from the 3.21 warm-boot image (`tools/mkdata.py`) |
| `state/a6000.nand` | emulated NAND, made by `./provision.sh <your dump>` |

Step-by-step instructions: [`data/README.md`](data/README.md). Then:
```sh
./run.sh                              # QEMU window with the camera panel
python3 tools/camui.py --launch       # or: camera-body web UI on http://localhost:8642
```

## Getting QEMU
The `qemu/` directory is **not published**. It is rebuilt from upstream QEMU `v10.1.0` plus this repository:
```sh
./setup-qemu.sh     # clone v10.1.0 into qemu/, copy qemu-overlay/, apply the patch
(cd qemu/build && ../configure --target-list=arm-softmmu --enable-gtk --enable-sdl --enable-slirp && ninja qemu-system-arm)
```
Ubuntu 22.04 / WSL2 build packages: `ninja-build pkg-config libglib2.0-dev libpixman-1-dev libsdl2-dev libgtk-3-dev libslirp-dev flex bison python3-venv python3-tomli`, plus `gcc-arm-none-eabi` for `provision/a6kcopy`.
Developers working in `qemu/` (branch `sony-a6000`) run `tools/export-qemu.sh` after each QEMU commit to refresh `qemu-overlay/` and the patch.

Machine options, appended to `run.sh` as extra `-M` arguments (e.g. `./run.sh -M rtc-lost=on`): `mode-dial=N` = mode-dial position (default 3 = P, table below); `rtc-lost=on` = clock backup lost (first-boot area/date/time setup); `lens=none|ekit` = empty E-mount (default) or kit-lens model; camera clock = QEMU RTC (`-rtc base=...`). SD card: `-drive if=sd` / `A6K_SD`.

## Camera controls
The camera-body UI (`tools/camui.py`) draws the top plate and back of the A6000 with every control in place, a live LCD,
and the same keyboard shortcuts as the QEMU window:

| Host key | Camera control | Emulated hardware path | A6000 |
|---|---|---|---|
| ↑ / ← / ↓ / → , Enter | control wheel DISP / Drive / ± / ISO, centre OK | ADC ch 0x0D ladder, levels 182 / 356 / 520 / 686, 45 | verified |
| `]` / `[` | control wheel turn CW / CCW (`Shift` = x4) | `cxd90014-jog` counter at 0xF2404014 | wired |
| Delete / Backspace | C2 / Delete | ADC ch 0x0C, level 45 | verified |
| `A` / `F` / `C` | AEL / Fn / C1 | ADC ch 0x0C, levels 182 / 356 / 520 | verified |
| PgUp / PgDn | mode dial, one detent | ADC ch 0x0E / 0x0F (two ladders), machine property `mode-dial` | verified |
| `M` / `P` | MENU / Playback | sub-micro slot A byte 28 bits 2 / 1 | verified |
| `H` | flash pop-up button | sub-micro CAkey1 bit 9, active low | wired |
| `R` | MOVIE | GPIO P10.10, active low | wired |
| `O` | ON/OFF switch (each press flips; starts ON) | sub-micro slot A byte 28 bit 0 | wired |
| `S` / `Shift+S` or `Space` | shutter half / full press | GPIO P0.1 + P0.11 / additionally P1.5 + P0.13, active low | wired |

Mode-dial positions (value of `KEY_POS_MODE_DIAL`, each checked on the OSD): 1 iA, 2 Superior Auto, 3 P, 5 A, 4 S, 6 M,
7 MR, 10 Movie, 8 Sweep Panorama, 11 SCN. Not wired yet: control dial, eye sensor, lens release. Mapping source: the
input.ko key table in the camera's own `Backup.bin` (`docs/A6000_ROADMAP.md`, "A6000 buttons").

Keys are also accepted from the HMP monitor (`sendkey ret 300`). Hold keys for about 300 ms: av-cam samples the buttons
only every ~43 ms, and the input→UI pipeline lags about one key, so for scripted navigation send one key at a time and
wait a few seconds before a screendump. Keys pressed in the first ~20 s after the first-boot dialog appears are ignored.
QEMU's own hot keys: Ctrl+Alt+G releases the mouse, Ctrl+Alt+1 / 2 switch between the panel and the debug frame-buffer view.

## Peripherals inherited from the platform model
These device models exist and boot with the A6000 firmware, but their behaviour has **not yet been re-checked on the
A6000**; the details and protocols are in `docs/reference/`.

### USB (virtual cable)
The USB device controller (`cxd90014-usbhdc`) serves the camera's USB gadget over TCP with the "A6KU" protocol. A
connected TCP client is a plugged cable (VBUS on); when it disconnects the cable is pulled.
```sh
./run.sh -M usb-socket=127.0.0.1:7643
python3 pmca-console.py info -d socket     # Sony-PMCA-RE with tools/pmca-socket/; A6K_USB=host:port, A6K_USB_TIMEOUT=ms, A6K_USB_DEBUG=1
```

### NFC (virtual phone)
The board NFC chip (`a6k-nfc`, FeliCa Plug–class dynamic tag on SIO2) idles like the real camera with no phone. A host
"phone" talks to it over a chardev line protocol (`tap`, `read`, `cmd <hex>`, `field on|off`, `status`):
```sh
./run.sh -chardev socket,id=nfc,host=127.0.0.1,port=7644,server=on,wait=off -M sony-a6000,nfc=nfc
nc 127.0.0.1 7644
```
The tag answers carry the camera's Wi-Fi Direct credentials from Backup: keep `-trace 'a6k_nfc_*'` logs local.

### Wi-Fi (virtual air)
An AR6003 SDIO module model on mmc1 (`ar6003-sdio`, chip-firmware HLE with virtual access points). Which module the A6000
carries is not yet verified.
```sh
./run.sh -netdev user,id=w0,net=192.168.77.0/24 -global ar6003-sdio.netdev=w0 -global 'ar6003-sdio.aps=ssid=TestAP,psk=testpass123'
```

### HDMI (virtual cable)
On-SoC HDMI TX (`cxd90014-hdmi`) and CEC (`cxd90014-cec`). `-M hdmi=on` plugs a virtual 1080p TV (hot-pluggable:
`qom-set /machine hdmi on|off`); the HDMI picture is QEMU console 1 (`screendump FILE vph 1`), `-M hdmi-edid=FILE` supplies an EDID.

### Guest shell
A root shell inside the running guest next to the stock UI: `A6K_SHELL=<port>` changes only the `console=ttynull` token
of the stock cmdline and serves UART2 as a TCP socket. Emulator-only developer aid (the real camera has no accessible UART).
```sh
A6K_SHELL=46310 ./run.sh
python3 tools/a6kshell.py --port 46310                       # interactive (Ctrl-] quits)
python3 tools/a6kshell.py --port 46310 push hello            # -> /android/a6k/hello (tmpfs), md5 checked
```
Keep the shell socket drained (a client that stops reading blocks the machine). The firmware's idle power save turns
the camera off after ~5 min without key input; `O`, `S`/`M`/`P` or HMP `system_wakeup` powers it on (warm resume).

## Status (A6000)
- ✅ Builds and runs on Linux / WSL2 (QEMU 10.1, TCG)
- ✅ Your NAND dump provisioned through Sony's own NAND stack; two-stage boot like the real camera
- ✅ Stock A6000 3.21 kernel, init, Android and the camera UI start; first-boot setup and the shooting screen render
- ✅ A6000 buttons and mode dial from the camera's key table (table above)
- ✅ Camera-body web UI with live LCD
- ⏳ Not re-checked on the A6000: USB, NFC, Wi-Fi, HDMI, lens, SD, still capture, power cycling
- ⬜ Control dial, eye sensor/EVF output, live view (no ISP model), Android/PMCA app runtime
Details and next steps: [`docs/A6000_ROADMAP.md`](docs/A6000_ROADMAP.md).

## Firmware and legal notice
- This repository contains **no Sony firmware, binaries or device dumps.** To run it you need a NAND dump of **a camera you own** and Sony's publicly distributed official updater (see `data/README.md`). Do not upload those files to this repository or its issues.
- This project is **not affiliated with, endorsed or sponsored by Sony Group Corporation.** "Sony", "α", "ILCE" and "BIONZ" are trademarks of their respective owners.
- The documents are reverse-engineering observations made for interoperability (emulation). Analysing or bypassing protection features such as battery authentication is out of scope.
- Work on real hardware is at your own risk. This emulator never writes to a real camera.

## Credits and references
Projects this work builds on or learned from:
- [Oein/a5000-emulator](https://github.com/Oein/a5000-emulator): the ILCE-5000 emulator this repository is forked from (GPL-2.0); its platform notes are kept in `docs/reference/`.
- [QEMU](https://gitlab.com/qemu-project/qemu): upstream emulator (`v10.1.0`) the `sony-a6000` machine is built on.
- [ma1co/fwtool.py](https://github.com/ma1co/fwtool.py): unpacks Sony's firmware updater (used by `tools/mkdata.py`, `provision.sh`).
- [ma1co/Sony-PMCA-RE](https://github.com/ma1co/Sony-PMCA-RE): PlayMemories Camera Apps tooling; `tools/pmca-socket/` is a USB backend for it (MIT).
- [ma1co/OpenMemories-Tweak](https://github.com/ma1co/OpenMemories-Tweak): root shell (Wi-Fi, Telnet, ADB) on a real camera, used to dump and to run the look mod.
- [ivica00/bionz-x-research](https://github.com/ivica00/bionz-x-research): the `arri_isp` mod that `mods/look/` is the A6000 analogue of.

## License
- QEMU code (`qemu-overlay/`, `patches/`), `tools/`, `provision/` and scripts: **GPL-2.0-or-later** (`LICENSE`, same as QEMU)
- Documentation (`docs/`, `*.md`): **CC BY 4.0**
- QEMU itself remains under its original authors' licenses (`qemu/COPYING`, `qemu/LICENSE`).
- Origin: this project is a fork of Oein/a5000-emulator (GPL-2.0), the emulator for the sibling ILCE-5000 on the same SoC; its notes are kept in `docs/reference/`.

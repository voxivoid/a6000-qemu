# A6000 emulator: status and roadmap

What runs today, what is verified on the A6000, and what is left. Background hardware notes from the platform
analysis this project grew out of are in `docs/reference/` (not A6000-verified; see its README).

## Status
| Area | State |
|---|---|
| Build on Linux / WSL2 (QEMU 10.1, TCG) | ✅ |
| Your NAND dump provisioned through Sony's NAND stack (`provision.sh`) | ✅ |
| Two-stage boot: stage-1 loader → stock 3.21 kernel/initrd read from NAND | ✅ |
| Stock init, Android init, `stella` WindowServer, camera UI; first-boot setup and shooting screen | ✅ |
| Buttons and mode dial from the camera's key table | ✅ (control dial, eye sensor open) |
| Camera-body web UI (`tools/camui.py`) | ✅ |
| USB, NFC, Wi-Fi, HDMI, lens, SD, still capture, power cycling | ⏳ device models present, not re-checked on the A6000 |
| EVF output + eye sensor, live view (no ISP model), Android/PMCA app runtime | ⬜ |

## Inputs
- Sony's official **3.21** updater (current exe format: cut out the `.dat`, `data/README.md` §1).
- A **NAND dump of your own A6000** (`/dev/nflasha`, read-only, ≈483 MiB). The updater lacks the per-unit data: the
  settings store `Backup.bin` (button map, boot load table, model/region), factory calibration, the updater core.
  The 3.21 updater's `init_bk.bak` is Android's initial data image, not factory settings, so there is no
  generic `Backup.bin`.
- The dump matches the official updater byte for byte where they overlap (vmlinux, initrd, av-cam, `kemco.txt`;
  the warm-boot image is the same up to the partition padding).

## Boot contract (verified, 3.21)
- `kemco.txt` (the bootloader's cmdline) is the board default, converted like the bootloader does ("kemco" header and the
  closing `*` line dropped, newlines → spaces).
- DDR: `mem=256M@0x80000000@0 mem=576M@0x9C000000@2`, 24 × 8 MiB `secgrp` sections (offline at boot) at
  0x90000000–0x9BFFFFFF → the board maps 1 GiB at 0x80000000 (H: the physical DDR size).
- initrd at 0x80628000. Hand-off page 0x80021000, `wbi_memclr=0x800210e8`, `klog.addr=0x834E0000`.
- Bootloader load table (Backup 0xFA0080): `\sabin\idt_cam.bin` 0x83501000, `\sabin\sa_dfdet.bin` 0x96800000,
  `\av-cam.bin` 0x83541000, `\kemco.txt` 0x80033000, `\initrd.img` 0x80628000, `\vmlinux.bin` 0x80038000.
  `a6kcopy -loadtable` reads it, no constants needed.
- Warm-boot image (nflasha5, 1164 sections): hand-off page; sub-micro frames at PA 0x8334417C (raw CRC valid).
- Kernel `3.0.27_nl-rt106`, built 2019-01-24, 3 478 912 B. UDIF peripheral table at VA 0x40379b28: 40 rows, identical
  to the reference build.
- initrd NAND stack (`devno`, `dmac`, `ldec`, `lld`, `nand.ko`) is a 3.21 build: the loader finds nand.ko's `g_dev`
  as module base (`/proc/modules`, 0x3f01f000) + offset computed by `mkinitramfs.py` from the packed nand.ko with
  Linux 3.0's `layout_sections` (3.21: 0x1bf38; the rule reproduces the reference build's value exactly). The stock
  busybox has no `printf`, so `a6kcopy -mumin <base> <off>` adds them.

## A6000 buttons
Source: the input.ko key table in the camera's `Backup.bin` (`BIN!` block, 25 rows of 64 B
`{id, src, port, bit, active_low, lo, hi, period, debounce, keycode, …}`). Key names from libObj's
`{code, "KEY_…"}` table (3.21 `libObj.so` file offset 0xfcc090).

| Input | Key | Host key | Status |
|---|---|---|---|
| ADC ch 0x0C 0–91 / 92–273 / 274–439 / 440–599 | DELETE (C2) / AEL 0x7090 / Fn 0x7095 / C1 0x66 (`KEY_TACT_CUSTOM`) | Del / A / F / C | firmware reacts: Fn panel, C1 "Lens: invalid", AEL "not available: MF" |
| ADC ch 0x0D | wheel ENTER / UP / LEFT / DOWN / RIGHT | Enter, arrows | verified |
| ADC ch 0x0E + 0x0F | mode dial 0x7025 (`KEY_POS_MODE_DIAL`), value 1..11 | PgUp/PgDn, `-M mode-dial=N`, `qom-set /machine mode-dial N` | all 10 positions verified by OSD |
| GPIO P10.10 | MOVIE (STARTSTOP) | R | wired |
| sub-micro CAkey1 bits 0 / 1 / 2 / 9 | POWER / PLAY / MENU / FLASH_POPUP (bit 9 active low) | O / P / M / H | MENU, PLAY verified; flash pop-up no visible change yet |
| shutter S1/S2 GPIOs (not in the table) | | S / Space | wired |

Mode dial (two ladders, one of them in a band at each position): ch 0x0E bands 0–91 = 6, 92–273 = 4, 274–439 = 5,
440–599 = 3, 600–772 = 2; ch 0x0F: 1, 11, 8, 10, 7. Physical order: 1 iA, 2 Superior Auto, 3 P, 5 A, 4 S, 6 M,
7 MR, 10 Movie, 8 Sweep Panorama, 11 SCN. Being a position, the level is the ADC channel's idle level
(`cxd90014_adc_set_idle`) and survives resets; the board default is P.
No body zoom lever row (the A6000 has none; the kit PZ lens's lever is on the lens).

## Open work
1. **Control dial**: not in the key table → most likely a second TPU counter next to the wheel's `cxd90014-jog`
   (0xF2404014). Find av-cam's reader in 3.21.
2. **Eye sensor + EVF**: second panel output (OLED 1024×768) and the LCD/EVF switch; find the sensor input.
3. **Flash pop-up**: confirm what bit 9 changes on the OSD; built-in flash state.
4. **Re-check inherited models on the A6000**: USB (PMCA install), NFC, Wi-Fi module identity, HDMI, kit lens, SD,
   power off / warm resume, updater mode.
5. **Firmware addresses**: the board and device models quote av-cam/kernel addresses from the reference build in
   comments (and some hooks use them). List every one that is acted on and re-find it in 3.21.
6. **Recipe Lab goal**: Android 2.3 + PMCA runtime with an app installed over the USB virtual cable; settings-store
   writes persist across an emulated reboot; read the settings store after a recipe is applied.

## Risks
- Hooks still carrying reference-build addresses may misbehave silently on 3.21 (open work 5).
- Android/PMCA bring-up has not been done on this platform before.
- Image processing is not emulated: how a recipe looks still needs the real camera.
- Custom look (gamma) mod: the ISP gamma is SRAM at PA 0xF3215300, verified on hardware; see `docs/A6000_LOOK_MOD_ISP.md`.

## Build notes (Ubuntu 22.04 / WSL2)
- Packages: `ninja-build pkg-config libglib2.0-dev libpixman-1-dev libsdl2-dev libgtk-3-dev libslirp-dev flex bison
  python3-venv python3-tomli`, plus `gcc-arm-none-eabi`. Don't use apt meson (0.61 is too old; QEMU uses its own).
- GCC/GLib fixes over the macOS-built upstream: a trace-event format without conversions (`cxd_dmac_done`), and
  `g_string_replace` (GLib 2.68) replaced since QEMU caps the API at 2.66.
- `a6kcopy` must be linked with `-Wl,-Ttext-segment=0x10000`: arm-none-eabi 10.3 links at VA 0, the kernel refuses
  page 0 and exec dies with SIGKILL.
- `provision.sh` logs UART2 (the debug console is `ttyAM1`).

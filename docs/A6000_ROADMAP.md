# A6000 emulator roadmap

Steps from the current a5000 fork to a working `sony-a6000` machine. Each step has a "done when" check.
Background and verdict: `docs/A6000_FEASIBILITY.md`.

## Phase 0: inputs and baseline
1. **a5000 baseline boots here.** `./setup-qemu.sh` + build, ILCE-5000 1.10 updater unpacked, `tools/mkdata.py`, `./run.sh`.
   Done when: stock a5000 firmware reaches the first-boot dialog. Without this, porting bugs can't be told apart from a broken setup.
2. **A6000 updater unpacked** (ILCE-6000 v3.21, fwtool).
   Done when: partition layout checked against the a5000 one: `nflasha3` (kernel/initrd), `nflasha5` (factory WBI RAM image).
   If there's no `nflasha5`, the hand-off page and sub-micro frames must come from a real-camera RAM dump (big plan change).
3. **Own A6000 dumped**, read-only (OpenMemories-Tweak telnet, `dd if=/dev/nflasha`, md5 on both sides) into `/dumps/`.
   A real-camera RAM/IRAM dump too if possible (bootloader state).
   Done when: the dump's md5 matches the camera's and a copy is backed up.

### Phase 0 status (2026-11-17)
- Step 1: `qemu/` cloned (v10.1.0), overlay copied, patch fully applied (`git apply --check --reverse` is clean). The build is blocked on apt build deps (needs sudo). The ILCE-5000 1.10 updater isn't downloaded yet.
- Steps 2–3: **mostly done already** in `../a6000-dumps/run2/` (taken via updatershell, `pmca-scripts/run_fw.sh`):
  - `nflasha.bin`: full NAND, 506 462 208 B (camera-side md5 not recorded)
  - `fw/`: fwtool unpack. `nflasha3_unpacked_unpacked/` = `vmlinux.bin`, `initrd.img`, `av-cam.bin`, `sabin/`
  - `fw/nflasha5`: 60 MiB, magic `WBI1` → **a factory WBI exists**, so `mkdata.py`-style derivation is possible (not decompressed yet: needs fwtool.py)
  - `fw/nflasha2_unpacked/kemco.txt`: the A6000 stock cmdline
  - Still open: the official 3.21 updater (only needed to cross-check the dump); a RAM/IRAM dump.
- First A6000 vs a5000 differences from `kemco.txt`:
  - DDR: `mem=256M@0x80000000@0 mem=576M@0x9C000000@2` (a5000: `200M@0x80000000`, `312M@0x8C800000`), with 24 × 8 MiB `secgrp` sections at 0x90000000–0x9BFFFFFF. 0x80000000–0xBFFFFFFF all in use → likely 1 GiB DDR (H)
  - initrd at **0x80628000** (a5000 0x80428000)
  - its own `wbimem=` list, `klog.addr=0x834E0000`, `tmonitor`, `cmpr.*` and `warm.*` regions; hand-off page still 0x80021000, `wbi_memclr=0x800210e8` is the same

### Phase 0 progress (later on 2026-11-17)
- Build deps on Ubuntu 22.04 / WSL2: `ninja-build pkg-config libglib2.0-dev libpixman-1-dev libsdl2-dev libgtk-3-dev libslirp-dev flex bison python3-venv python3-tomli`, plus `gcc-arm-none-eabi` for `provision/a5kcopy`. Don't install apt meson (0.61 is too old; QEMU uses its own venv copy).
- Two Linux/GCC build fixes (macOS clang hadn't flagged them):
  - `hw/dma/trace-events`: the `cxd_dmac_done` format had no `%` placeholders for its 2 arguments → `-Werror=format-extra-args` (fixed in the patch)
  - `sony_a5000.c`: `g_string_replace` is GLib 2.68+, but QEMU caps the API at 2.66 → replaced with a `strstr` splice
- `qemu-system-arm` builds; `-M help` lists `sony-a5000`.
- **New Sony updater exe format** (current downloads): fwtool fails with `Unknown exe file`. The exe has a custom archive appended after the PE (UTF-16 names + u32 sizes) holding `FirmwareData_<model>.dat` (DAT magic `\x89UFU\r\n\x1a\n` at file offset 524866 in both the 5000 and 6000 exes). Cut the `.dat` out (offset + size field before the name), then `fwtool unpack -f <.dat>` works.
- fwtool deps live in `../fwtool.py/.venv` (pycryptodomex, pyyaml).
- `tools/mkdata.py` OK on the a5000 1.10 updater (WBI 1073 sections, 127 430 656 B).
- **No a5000 NAND exists** (the owner has an A6000 only), so a full a5000 `run.sh` baseline is impossible. Decision: skip it. The a5000 kernel is used only as a provisioning smoke test, then go straight to A6000 bring-up.

- **Step 9 done early: `state/a6000.nand` provisioned** from `dumps/run2/nflasha.bin` with the a5000 1.10 kernel + NAND stack (`a5k-provision: OK`; untouched copy `state/a6000.nand.pristine`). This also smoke-tests the a5000 kernel, SMP, GIC and NAND HLE under WSL. Two fixes:
  - `provision.sh` logged UART0, but `debug-console=on` now means `ttyAM1` = UART2 → the log stayed empty and the script waited out its timeout. Now it passes `-serial null -serial null -serial file:LOG`.
  - `a5kcopy` built with Ubuntu arm-none-eabi 10.3 links its first PT_LOAD at VA 0 → the kernel refuses page 0 → exec dies with SIGKILL (`Killed`). Build with `-Wl,-Ttext-segment=0x10000` (documented in `a5kcopy.c`).

### First A6000 boot (2026-11-17): stock UI reached
`A5K_MODEL=a6000 ./run.sh` → stage-1 loader (A6000 kernel + modules) mounts the NAND (partitions p1..p23) → stage 2 stock kernel/initrd from NAND → stock `/sbin/init`, Android init, `stella` WindowServer → **panel shows the stock "Set Area/Date/Time." first-boot dialog** (~100 s, WSL). What it took:
- `sony-a6000` machine type (subclass of `sony-a5000`) + `A5KModel` profile: 1 GiB DDR, initrd 0x80628000, cmdline = the 3.21 `kemco.txt` (conversion checked: the same rule reproduces the a5000 string exactly), ATAG banks
- `firmware-a6000/` = the official 3.21 updater unpacked; identical to the dump (vmlinux, initrd, av-cam, kemco, WBI prefix)
- `data-a6000/` from the 3.21 WBI (1164 sections). Sub-micro frames still at PA 0x8334417C (raw CRC valid, same 0x20/0x30 headers)
- load table (Backup 0xFA0080): av-cam.bin still 0x83541000; sa_dfdet.bin 0x96800000 (`-loadtable` reads it, no change needed)
- loader: nand.ko `g_dev` = module base from `/proc/modules` + offset computed by `mkinitramfs.py` from the packed nand.ko (Linux 3.0 `layout_sections`; a5000 0x1bd20 reproduces the old 0x3f03ad20; A6000 0x1bf38, base unchanged 0x3f01f000). The stock busybox has no `printf` → `a5kcopy -mumin <base> <off>` adds them.
- `run.sh`: `A5K_MODEL=a6000`; the loader cpio is rebuilt when stale
- Not yet checked: everything past the dialog; av-cam hooks (row 5); A6000-only hardware.

### Camera body UI
`python3 tools/camui.py --launch` (starts `run.sh` headless with a QMP socket) → http://localhost:8642: live LCD (QMP `screendump`, ~5 fps) + a drawn A6000 body (top plate, back, lens/sides). Controls send QMP `input-send-event` down/up, so holds are real. The shutter goes half → full after 0.7 s (or shift-click). Layout from product photos plus Sony's ILCE-6000 Help Guide "Identifying parts" (no AF/MF lever on the A6000: that's AEL; C2 = Delete).

### A6000 buttons (step 7, mostly done)
Source: the input.ko key table in the owner's `Backup.bin` (`BIN!` block at file 0x9ed74, 25 rows of 64 B; first row 0x9ed8c). Names from the A6000 libObj `{code, "KEY_..."}` table (file 0xfcc090).
| Input | Key | Host key | Status |
|---|---|---|---|
| ADC ch 0x0C 0–91 / 92–273 / 274–439 / 440–599 | DELETE (C2) / **AEL** 0x7090 / **Fn** 0x7095 / **C1** 0x66 | Del / A / F / C | firmware reacts: Fn panel, C1 "Lens: invalid", AEL "not available: MF" |
| ADC ch 0x0D | wheel ENTER/UP/LEFT/DOWN/RIGHT (as a5000) | arrows, Enter | verified |
| ADC ch 0x0E + 0x0F | **mode dial** 0x7025, value 1..11 | PgUp/PgDn, `-M sony-a6000,mode-dial=N`, `qom-set /machine mode-dial N` | all 10 verified by OSD: 1 iA, 2 iA+, 3 P, 5 A, 4 S, 6 M, 7 MR, 10 Movie, 8 Panorama, 11 SCN |
| GPIO P10.10 | MOVIE (as a5000) | R | wired |
| sub-micro CAkey1 bits 0/1/2/9 | POWER / PLAY / MENU / FLASH_POPUP (active low) | O / P / M / H | MENU, PLAY verified; flash pop-up no visible change yet |
| shutter S1/S2 GPIOs (not in the table, as a5000) | | S / Space | wired |
| no row | body zoom lever (A6000 has none; T/W kept for the a5000 only) | | — |
Still open: **control dial** (not in the key table: likely a second jog/TPU counter, like the wheel's `cxd90014-jog`), **eye sensor**, lens release, the flash pop-up's effect. The ADC model now keeps a per-channel idle level across resets (`cxd90014_adc_set_idle`), so the dial survives reboots.

### Phase 1 first results
- Kernel: same source `3.0.27_nl-rt106`, same size (3 478 912 B), rebuilt 2019-01-24 for the A6000. 183 157 bytes differ.
- UDIF peripheral table: **identical** (VA 0x40379b28, 40 rows, byte for byte).
- initrd: same 16-file tree. Rebuilt (different md5): `kmod/{devno,dmac,ldec,lld,nand}.ko`, `sbin/init`, `version.txt` (`34135_14.2019012401`). The `nand.ko` struct VA `0x3f03ad20` in `loader.sh` probably moves.
- NAND dump size = 483 MiB, the same as an a5000 → same flash geometry likely (H).

## Phase 1: static diff (no emulator)
4. **Kernel diff.** Extract symbols from the A6000 `vmlinux.bin`, then compare:
   - UDIF peripheral table (PA, IRQ) against `docs/HARDWARE.md`
   - `NR_IRQS`, memory map (`mem=` cmdline), Backup load-table prop `0x00FA0080`
   - initrd module list and load order (the nand.ko struct address `0x3f03ad20` in `provision/loader.sh` depends on it)
5. **`HARDWARE.md` vs nicolvus' A6000 notes**: a table of each device as same / changed / new.
6. **List everything tied to the firmware version**: hardcoded firmware VAs in `qemu-overlay/hw/arm/sony_a5000.c`, the device models and `tools/*.c`.
   Each one needs re-finding in 3.21.

## Phase 2: machine profile
7. `sony-a6000` machine: either a `model=` option on the existing board (keeps the a5000 from regressing) or a fork into `sony_a6000.c`. Swap in the addresses from step 6.
8. Hardware the A6000 has and the a5000 lacks (to confirm in step 5):
   - EVF (OLED 1024×768) + eye sensor that switches LCD/EVF: second VPH output or panel
   - physical mode dial + top control dial (the a5000 has none, see `RE_MODE_DIAL.md`): new GPIO/ADC/encoder wiring
   - flash pop-up, Multi Interface Shoe detect: GPIO
   - C1/C2 and AF/MF buttons, extra key IDs: the ADC ladder from `RE_INPUT_I2C_POWER.md` will change
   - sub-micro ("Darwin") link: frame layout and slot bytes (`RE_SUBMICRO_LINK.md`)
   - sensor: 24 MP with on-sensor PDAF; CamCore/IDT timing and frame size
9. Provisioning: A6000 modules and paths in `provision.sh`/`loader.sh`; FTL format, then write the dump.

## Phase 3: boot bring-up (iterative)
10. Kernel up to `/sbin/init` (`debug-console=on`, klog).
11. NAND/MUMIN mounted, `Backup.bin` loaded, av-cam started.
12. `im.elf` running, panel output, first-boot dialog.
13. Keys and dials take effect, menu navigation works.
14. Clean power-off and reboot cycle (as in `RE_REBOOT.md`).

## Phase 4: Recipe Lab goal
15. Android 2.3 + PMCA runtime booted. Not done by any existing project; biggest risk. Start from `RE_USB_APP_INSTALL.md`.
16. Settings-store writes persist: Backup writes reach the emulated NAND and survive an emulated reboot.
17. Hook that reads the settings store after a recipe is applied (`mdl_so.c` / `rd_so.c` pattern).

## Risks
- The A6000 updater may have no factory WBI, which forces RAM captures from the real camera.
- Firmware 3.21 addresses are all different: every IDA address in the overlay and tools has to be found again.
- Android/PMCA bring-up has never been done.
- Image processing is not emulated, so checking how a recipe looks still needs the real camera.

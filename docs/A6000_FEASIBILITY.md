# A6000 emulator: feasibility notes

Carried over from the recipe-lab-sony-pmca chat (2026-11-17) that started this repo.
Sources: `ivica00/bionz-x-research`, `nicolvus/a6000-fw-decomp`, `Oein/a5000-emulator`.

## Verdict
Feasible. Base on the a5000 emulator, not the a6400 one.

## Why not a6400 (`ivica00/bionz-x-research`, qemu-a6400)
- a6400 = BIONZ X (`CXD90045`, 4 cores, eMMC). A6000 = older BIONZ (`CXD90014`), NAND + separate "Darwin" microcontroller.
- Hardware models, addresses and boot chain all differ.
- It boots from data captured off a real a6400 (eMMC images + RAM snapshot). An A6000 would need an equivalent capture.

## Why a5000 (`Oein/a5000-emulator`, this repo's base)
- α5000 uses the same SoC as the A6000 (`CXD90014`).
- Runs Sony's unmodified α5000 firmware in QEMU 10.1: NAND, display, GPU, timers, buttons, SD card.
- Can inject a `.so` into the main UI program (`im.elf`).
- The a6400 machine itself was modelled on it.
- An `sony-a6000` machine = swap in: Darwin micro link, A6000 button/dial wiring, panel + EVF, A6000 firmware 3.21 + NAND dump.

## State of `nicolvus/a6000-fw-decomp`
Not a full-system emulator: userland on QEMU `virt` with a Debian kernel; `sonyshim.so` fakes Sony kernel drivers; backup store served from an NVRAM dump.
- Works: menus render, buttons, emulated SD, telnet.
- Android not booted (their task T6) -> a PMCA app cannot run.
- Backup store writes discarded (shim backup op 8).
- Image pipeline not emulated (Picture Profile preview is a WebGL approximation).

## Relevance to Recipe Lab
Useful target: Android 2.3 booted + backup store writing, so we can install the APK, apply a recipe, check bytes in the backup store, and check they survive an emulated reboot.
Still cannot show how a recipe looks: image processing is not emulated anywhere, so the real-camera power-cycle check stays required.

## Plan
1. Add an A6000 machine profile (fork of a5000 machine).
2. Dump own A6000 NAND (a5000 project uses owner dump + official updater; nicolvus has NVRAM backup / SWD tooling).
3. Bring up kernel + `im.elf` using nicolvus' notes (key table, sub-micro link, settings-store IDs).
4. Boot Android + PMCA runtime (unknown: neither project has done it).
5. In-emulator hook reading the settings store after a recipe is applied.

Multi-week effort; steps 1-3 mostly porting, step 4 is the unknown.
Next suggested task: diff `docs/HARDWARE.md` against nicolvus' A6000 notes and list differing device models.

## Licence / hygiene
- GPL-2.0 (inherited from a5000 emulator); applies if the repo is shared.
- Never commit firmware, NAND dumps, RAM snapshots, unpacked updater (see `.gitignore`).

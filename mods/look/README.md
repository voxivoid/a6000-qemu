# A6000 custom-look mod (RAM-only, experimental)

Install a **custom tone curve** (and later a color matrix) on a Sony A6000 by
overriding its av-cam develop tables in DDR through `/dev/mem`. This is the
A6000 analogue of [ivica00/bionz-x-research `arri_isp`](https://github.com/ivica00/bionz-x-research/tree/main/mods/arri_isp),
which does the same on the a6400 by repainting its S-Log3 slot. The A6000 has
**no Picture-Profile / S-Log firmware**, so this targets the *base* develop
gamma instead — one baked look, not selectable profiles.

**RAM-only. Nothing is written to flash.** av-cam's EC refills its working
gamma from a read-only copy within ~20 ms, so the curve is *held* while you
shoot and reverts the instant you stop (or pull the battery). Worst case — a
write glitches av-cam and capture hangs — is cleared by a battery pull; the
next boot is 100 % stock. **For a camera you own. The look is unverified; the
emulator can't run the ISP, so only your camera shows the result.**

## What's proven vs not
- **Proven in the emulator:** `/dev/mem` access, `scan` finds the gamma LUT
  banks by content, curve generation in the right format, `write` + readback
  are byte-identical, restore works. av-cam loads raw-contiguous at
  `PA 0x83541000`, so a table at `av-cam.bin` file offset *F* is at
  `PA 0x83541000 + F`.
- **Only your camera can show:** which LUT is the live still-capture gamma,
  whether EC refills it (needs `hold`), and the actual look.

## The gamma LUT format (A6000 3.21)
130× little-endian `u16`, monotone, output `0..1024`. There is a **bank** of
these (several `0..1024` 129-point tables = per-mode curves). One is the active
still output gamma — you find it empirically with the `invert` test below.

## Build
```sh
arm-none-eabi-gcc -march=armv7-a -marm -O2 -nostdlib -static -ffreestanding \
    -Wl,-Ttext-segment=0x10000 -o mods/look/gammamod mods/look/gammamod.c
```

## On-camera procedure
Get a root shell the same way you dumped the NAND (OpenMemories-Tweak via
Sony-PMCA-RE, telnet). Have the battery door ready — a battery pull is the
reset.

1. **Copy the tool to RAM** (tmpfs, gone at power-off):
   ```sh
   # from the computer, over the telnet/adb bridge you used for the dump
   # (push gammamod to /android, chmod 755)
   ```
2. **List the live gamma tables:**
   ```sh
   /android/gammamod scan 0x83541000 0xb00000 128
   ```
   Note the `len 130  0..1024` entries — those are tone curves.
3. **Save every candidate** (so you can always restore):
   ```sh
   /android/gammamod read <PA> 0x104 /android/orig_<PA>.bin
   ```
   Pull them to the computer.
4. **Find the active one with an unmistakable test** — invert one table and
   shoot while it's held:
   ```sh
   # computer: mods/look/make_curve.py orig_<PA>.bin inv.bin invert ; push inv.bin
   /android/gammamod hold <PA> /android/inv.bin 60000   # holds 60 s
   # during the hold: take a photo / watch live view.
   ```
   If the image flips to a negative, that `<PA>` is a live output curve. If
   nothing changes, try the next candidate. `hold` (not `write`) is required —
   EC refills the stock curve within ~20 ms.
5. **Apply a real look** once you've found the table:
   ```sh
   # computer: make_curve.py orig_<PA>.bin flat.bin flat 0.7   (or: contrast)
   /android/gammamod hold <PA> /android/flat.bin 600000
   ```
6. **Revert:** stop `gammamod` (the next EC refill restores stock), or write a
   saved original back, or pull the battery.

If `scan` of the image region finds no live effect, the working copy may live
in a separate DDR buffer: scan wider (e.g. `scan 0x80000000 0x20000000 128`,
read-only is safe) and use `-F` to write outside the av-cam window **only** at
a PA you confirmed holds a gamma table.

## Files
- `gammamod.c` — the on-camera `/dev/mem` tool (`scan` / `read` / `write` / `hold`).
- `make_curve.py` — host: build a curve in the LUT format (`identity`/`flat`/`contrast`/`invert`).

## Safety
- RAM-only; no flash write. Battery pull = stock.
- Writes are bounded to the av-cam DDR window unless `-F`.
- Keep the `orig_*.bin` dumps; they are your restore.
- Don't run on low battery or during any firmware update.
- Experimental: a wrong write can glitch a frame or stall capture → battery pull.

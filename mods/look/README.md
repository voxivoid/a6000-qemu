# A6000 custom-look mod (RAM-only, experimental)

> **Update (hardware-verified): the ISP gamma is memory-mapped SRAM at PA `0xF3215300`** (96 x u32). Writing it
> inverts the saved image and the live view. The DDR tables the sections below talk about (the `0..1024` u16 tables,
> the 97-word Creative-Style curve) are **inert working copies**: poking them does nothing. Use
> `gammamod hwrd | hwdump | hwwr` (see "Hardware gamma path"). Full findings and the RE trail:
> [`docs/A6000_LOOK_MOD_ISP.md`](../../docs/A6000_LOOK_MOD_ISP.md).

## Hardware gamma path (the one that works)
Needs a root shell on the camera (OpenMemories-Tweak: Wifi + Telnet + ADB; push the binary with `adb push`, never over
telnet). Save the original first; restore = write it back.
```sh
/android/gammamod hwrd 0xF3215300 8                       # read-only look
/android/gammamod hwdump 0xF3215300 96 /android/hw_orig.bin   # SAVE THIS (also pull it to the PC)
# PC: build 96 x u32 (0..1023), e.g. 1023 - curve for the invert test; adb push as /hw_new.bin
/android/gammamod hwwr 0xF3215300 /android/hw_new.bin 60000   # write once, then poll+rewrite for 60 s (real clock)
/android/gammamod hwwr 0xF3215300 /android/hw_orig.bin        # restore
```
`hwwr` with a duration polls the table and rewrites it the instant the camera restores the stock curve (it does so ~50x/s).
Without a duration it writes once (stills may still pick it up, live view flickers). Access is locked to
`0xF3215300..0xF321547F` unless `-F`. Other commands: `find` (RAM byte-pattern search), `scan`/`scan32` (monotone ramps
with checksums, for diffing two camera states). Battery pull = stock; nothing is written to flash.

---

## Older (DDR-table) notes, superseded

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
1b. **Confirm the PA mapping on THIS camera (read-only, mandatory):**
   ```sh
   sh /android/check_map.sh /system/av-cam.bin   # must print MAP CONFIRMED
   ```
   If it does not, do not write anything; the load base differs on this unit.
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
- `check_map.sh` — on-camera read-only check that av-cam is at PA 0x83541000 (run first).
- `make_curve.py` — host: build a curve in the LUT format (`identity`/`flat`/`contrast`/`invert`).

## Safety
- RAM-only; no flash write. Battery pull = stock.
- Writes are bounded to the av-cam DDR window unless `-F`.
- Keep the `orig_*.bin` dumps; they are your restore.
- Don't run on low battery or during any firmware update.
- Experimental: a wrong write can glitch a frame or stall capture → battery pull.

# Reference notes (background, not A6000-verified)

These notes come from the emulator this project grew out of. They were reverse-engineered on the
**ILCE-5000**, a sibling camera on the **same CXD90014 SoC**, running its firmware **1.10**. They are kept
because most of the hardware analysis carries over: the peripheral table in the A6000 3.21 kernel is
byte-for-byte identical, and the device models built from these notes boot the A6000 firmware.

Read them with care:
- **Hardware facts** (register meanings, IRQs, protocols) are a strong starting point, but only what
  `docs/A6000_ROADMAP.md` lists as checked has been verified on the A6000.
- **Firmware addresses** (av-cam, kernel, `im.elf`/libObj functions, module offsets) belong to the 1.10
  build and are **wrong for A6000 3.21**. Re-find them in the 3.21 binaries before relying on them.
- Mode-dial, zoom-lever and button notes describe the sibling's body; the A6000 body is documented in
  `docs/A6000_ROADMAP.md` ("A6000 buttons").

The notes are kept as they were; links inside them may point to their old `docs/` locations.

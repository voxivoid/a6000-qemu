# sony-a5000 machine — hardware model & provenance

Every value here was recovered from the official 1.10 firmware (kernel
`vmlinux.bin`, modules) or from the real camera's WBI RAM snapshot
(`re/wbi/ram.bin`, read with `emulator/tools/wbi.py`).  Status:
**V** = verified by the unmodified firmware running in the emulator,
**S** = taken from the snapshot of the real camera, **H** = hypothesis.

## CPU / core
| Item | Value | Source | St |
|---|---|---|---|
| CPU | Cortex-A5 MPCore, MIDR 0x410FC051 (r0p1) | `__v7_ca5mp_proc_info`, boot log | V |
| FPU | VFPv4-**D16**, no NEON, FPSID 0x41023051 | Sony `vfp_init` BUG()s "VFP is not D16 type" otherwise | V |
| Cores | **4** (snapshot log "CPU1..3: shutdown"); emulator: "SMP: Total of 4 processors activated" | secondary release: boot CPU writes entry to 0xC0005010+4·(cpu-1) + SEV; secondaries (boot-ROM stub in the board) jump there, then kernel holding pen (GICC IAR + pen_release) | V |
| MPCore private | 0xF8000000 (SCU, GICC +0x100, GTimer +0x200, TWD +0x600, GICD +0x1000) | `gic_init(0,29,0xF8001000,0xF8000100)` | V |
| L2C-310 | 0xF8002000, CACHE_ID 0x410000C8, 8-way 128 KiB | `cxd90014_map_io`, boot log | V |
| GIC lines | 256 (`NR_IRQS:256`) | boot log | V |

## Memory
| Region | PA | Notes | St |
|---|---|---|---|
| DDR | 0x80000000, 512 MiB | cmdline `mem=200M@0x80000000@0 mem=312M@0x8C800000@2`; log "Memory: 200MB 312MB" | V |
| On-chip RAM | 0xC0000000, 16 MiB | `map_io` maps it cached @0xC0000000 and uncached @0xC1000000; +0x5000 = SMP mailbox | V |
| ARM7 SRAM | 0x00000000, 16 KiB | UDIF `arm7_sram`; +0x3F00 = NAND dev-info left by bootloader | V/S |
| Provision window | 0x40000000 | emulator-only, `-M ...,provision=<img>` | — |

## Boot contract (what the Sony bootloader does, reproduced by the board)
* Raw kernel Image at PA 0x80038000 (PAGE_OFFSET 0x40000000, TEXT_OFFSET 0x38000), r1 = 0xDA8 "ARM-CXD90014".
* **Command line is passed as `"kemco" + cmdline` at PA 0x80033000** (setup_arch strncmp). ATAGs are ignored: Sony head.S never vets r2.
* initrd (ext2) at 0x80428000, `initrd=0x80428000,8M`.
* **GIC is pre-configured by the bootloader**: Sony's `gic_dist_init` only toggles GICD_CTLR and installs handlers — it never writes ITARGETSR/IPRIORITYR/ICFGR. The board stub routes all SPIs to CPU0 before entering the kernel (without it, nothing reaches CPU0 on SMP). Saved GIC state in the snapshot (gic_cxd90014_suspend → VA 0x40393f80) only holds ISENABLER0..7 and PMR=0xF0.
* Exact stock command line recovered from the snapshot is the default (`console=ttynull`). `debug-console=on` → `console=ttyAM0 amba2.console=1` (Sony PL011 driver drops console output unless `amba2.console` is set).

## Peripherals (kernel UDIF table @VA 0x40379b28, stride 0x5C: name[8], PA, size, VA, ?, irqname[16], irq, flags)
| Name | PA | IRQ | Model |
|---|---|---|---|
| UART0/1/2 (PL011, ttyAM0/–/ttyAM1) | 0xF2000000 / 1000 / 2000 | 150/151/152 | QEMU pl011 — V |
| WDT | 0xF2402000 | — | cxd4115-timer (watchdog=on): CTRL 0x80000012 setup, 0x80001012 start, kick CLR=0x10 — V |
| Timer block (CXD4115) | 0xF2403000, ch n at +0x100·n | 153+n | cxd4115-timer — V (hwtimer0..3 = ch5..8 = IRQ 158..161 confirms the formula) |
| sircs (IR remote) | 0xF2400000 | 198 | — |
| i2c | 0xF2401000 | 199 | — |
| sio0..4 | 0xF2405000 + 0x200·n | 201..205 | — |
| clkrst3 | 0xF2900000 | — | unimp |
| ldec (LZ decoder) | 0xF2903000 | 171 | — |
| gpio_p0..p17 | 0xF2910000 + 0x100·n | — | unimp (`set_led` uses p0 +0x44/+0x48) |
| system ctrl | 0xF2915000 | — | cxd90014-sysctl: +0 chip id 0x500 (S), +0x44/+0x48 rst set/clr, 0x11 = SoC reset — V |
| otg | 0xF2920000 | 232 (VBUS), 233 (ID) | cxd90014-usbhdc: +0x10 bit0 VBUS active low, bit1 ID; 3 = no cable, 2 = A5KU client connected; GIC 232 pulse on change — V (`docs/RE_USB_DEVICE_CONTROLLER.md` §6/§9) |
| hdmi | 0xF2A24000 | — | — |
| clkrst4 | 0xF2A30000 | — | unimp |
| cec | 0xF3C10000 | — | — |
| dmac ch0-3 / ch4-7 | 0xF0000000 / 0xF0010000 | 189..196 | — |
| usb ho / h2xbb | 0xF0200000 / F0203000 | — | — (host side, unused in device mode) |
| usb hdmac | 0xF0204000 | 228, 229 (errors only, never raised) | cxd90014-usbhdc region 1 — V for bulk DMA (`docs/RE_USB_DEVICE_CONTROLLER.md` §5/§9) |
| usb hdc (F_USB20HDC) | 0xF0210000 (0x20000) | 227 | cxd90014-usbhdc region 0; reset values DEVC 0x00010020, EPCTRL 0x500/0x400 are H; enumeration + MTP over A5KU — V (`docs/RE_USB_DEVICE_CONTROLLER.md` §9) |
| mmc0/1/2 | 0xF0300000/1000/2000 | 218/221/224 | — |
| ms (Memory Stick) | 0xF0303000 | 206 | — |
| nand_reg | 0x00020000 (64 KiB) | 172 | cxd90014-nand (reg file + logging) |
| nandc data window | 0x10000000 (64 KiB) | — | unimp |
| arm7io | 0x00011000 | 170 | cxd90014-nand |

## NAND geometry (lld.ko cache in the snapshot, identical in the emulator)
Registers left by the bootloader: 0x140=0 (dies-1), 0x150=0x40 (pages/block), **0x170=0x1000 (main page size 4096)**,
**0x180=0x100 (spare 256)**, 0x270 also pre-set — see `docs/NAND_PROTOCOL.md`. → 4096+256 B pages, 64 pages/block,
2048 blocks, 1 die, 1 plane (512 MiB); MUMIN keeps 8 "extra" (OOB) bytes per page. (An earlier note here read 0x170 as a
block count — wrong.)
SRAM dev-info blob (+0x3F00): `00000000 c0000c00 00000006 00000001 00000800 0000002e 00000005 0000c350 0 0 0 0 00000004`.
Bootloader also leaves ARM7 firmware status SRAM+0x3F40 = 2 (running) and a bad-block list at PA 0xC0000C00.

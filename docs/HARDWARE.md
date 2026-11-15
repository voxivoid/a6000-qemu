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
* **GIC is pre-configured by the bootloader**: Sony's `gic_dist_init` only toggles GICD_CTLR and installs handlers — it never writes ITARGETSR/IPRIORITYR/ICFGR. The board stub routes all SPIs to CPU0 before entering the kernel (without it, nothing reaches CPU0 on SMP). Saved GIC state in the snapshot (gic_cxd90014_suspend → VA 0x40393f80) only holds ISENABLER0..7 and PMR=0xF0. ICFGR set by the stub: IRQ 98/99/101/102 (ICFGR6 = 0x28A0) and the NFC data-ready IRQ 125 (ICFGR7 = 0x08000000) edge-triggered, everything else level — the real values are not in the snapshot (H; 125: `nfc_rsrc_irq` only posts an event, as a level input one command re-entered ~49 times, `docs/RE_NFC.md` §4.3).
* **Bootloader load table = Backup prop 0x00FA0080** (oversize property: u16 length 0x2400, then 0x90-byte rows `{type, start, end, flags, path[0x80]}`): `\sabin\idt_cam.bin` → 0x83501000 (IDT init `4397BB1C` checks u16 0x3D there, prop 0xFA0074), `\sabin\sa_dfdet.bin` → 0x9EC00000, `\av-cam.bin` → 0x83541000, `\kemco.txt` → 0x80033000, `\initrd.img` → 0x80428000, `\vmlinux.bin` → 0x80038000 (S, the unit's `Backup.bin`). The stage-1 loader copies the `\sabin\` rows (`a5kcopy -loadtable`), the other rows are its existing lines (`docs/RE_RAW_DEVELOP.md` §3.2).
* **Hand-off word +0xE8 (PA 0x800210E8) = `wbi_memclr`**: 1 = the kernel (`wbi_cmdline_parse_mem` 0x4033B56C) clears every `wbimem=` region above 0x80028000 at boot. Factory page value 1 (S). On a loader (cold) boot whose load table preloaded a `wbimem` region (sa_dfdet.bin at 0x9EC00000) the board presents 0 for that stage 2 and clears the other `wbimem` regions itself (descriptor `"A5KM"` at PA 0x80027040 from stage 1); cold-boot value H (`docs/RE_COLD_BOOT_MODE.md` §3, §5.4).
* Exact stock command line recovered from the snapshot is the default (`console=ttynull`). `debug-console=on` → `console=ttyAM0 amba2.console=1` (Sony PL011 driver drops console output unless `amba2.console` is set).

## Peripherals (kernel UDIF table @VA 0x40379b28, stride 0x5C: name[8], PA, size, VA, ?, irqname[16], irq, flags)
| Name | PA | IRQ | Model |
|---|---|---|---|
| UART0/1/2 (PL011, ttyAM0/–/ttyAM1) | 0xF2000000 / 1000 / 2000 | 150/151/152 | QEMU pl011 — V |
| WDT | 0xF2402000 | — | cxd4115-timer (watchdog=on): CTRL 0x80000012 setup, 0x80001012 start, kick CLR=0x10 — V |
| Timer block (CXD4115) | 0xF2403000, ch n at +0x100·n | 153+n | cxd4115-timer — V (hwtimer0..3 = ch5..8 = IRQ 158..161 confirms the formula) |
| sircs (IR remote) | 0xF2400000 | 198 | — |
| i2c | 0xF2401000 | 199 | — |
| sio0..4 | 0xF2405000 + 0x200·n | 201..205 | sub-micro link on ch0/ch3. Slot-A RX byte 28 (CAkey1): bit0 = power lever, slide switch, 1 = ON (V: 0 → firmware starts an off with factor 7), bit1 = PLAY, bit2 = MENU. Slot-A TX (frame[0] = 0x21) off data: +0x13 bit0 valid, +0x14 factor, +0x15 next boot (0x20 restart / 0x21 stay off), +0x1B bit0 warm, +0x1E factor, +0x1F..+0x21 wake masks (bit meaning H) — V by the factor 7/8/13 offs (`docs/RE_REBOOT.md` B.1, B.5) |
| NFC tag (board, `a5k-nfc`) | SIO ch2 + GPIO | 125 (data ready, P0.4 ext-int), 129 (RFDET P0.8) | FeliCa Plug–class dynamic Type 3 tag [? part]. SW = P7.10 out (1 = chip powered), SEL = P7.14 out (traced), RFDET = P0.8 in, active low, **idles 1** (no phone; was 0 = phone always present), IRQ 125 driven directly by the model, one edge per reader command. SIO ch2 +0x04 bit29 = TX, bit28 = RX (H, passed to the slave). 9-byte TX after power-on = set parameter `1B FFFF 0000 00000000` (owner Backup 0x1820001..05); 9-byte TX with SW = 0 = first-comm frame, ignored; reader command served as 128 + 102 B RX; replies `0000`+16·n B (read) / `01A5` (write). Pins/IRQs V (driver reaches handover), trigger type of 125 H. Host side `nfc=<chardev>` (`docs/RE_NFC.md` §4) |
| clkrst3 | 0xF2900000 | — | unimp |
| ldec (LZ decoder) | 0xF2903000 | 171 | — |
| gpio_p0..p17 | 0xF2910000 + 0x100·n | — | cxd90014-gpio; p0.10 = power-off line: the board sets it high at reset (bootloader stand-in, H) and a 1→0 edge (IRAM sleep code, `0xF2910048 = 0x400`) triggers the sub-micro power cycle / power off (`poff=auto|restart|off`, default `auto` = slot-A TX off data +0x15: 0x20 power cycle, else stay off = VM suspended) — V for the updater restart (factor 13) and the idle power-save off (factor 8) (`docs/RE_REBOOT.md` §4.4, B.4, B.7) |
| DDR controller | 0xF0104000 | — | cxd90014-ddrc: +0xAC command (bit1 → +0x128 bit12 done), +0x12C W1C, +0x78[31:24] requested state, +0x7C low byte 0x25 when 0x0A requested; register names H — V for the IRAM sleep code (`docs/RE_REBOOT.md` §4.3) |
| system ctrl | 0xF2915000 | — | cxd90014-sysctl: +0 chip id 0x500 (S), +0x44/+0x48 rst set/clr, 0x11 = SoC reset — V |
| otg | 0xF2920000 | 232 (VBUS), 233 (ID) | cxd90014-usbhdc: +0x10 bit0 VBUS active low, bit1 ID; 3 = no cable, 2 = A5KU client connected; GIC 232 pulse on change — V (`docs/RE_USB_DEVICE_CONTROLLER.md` §6/§9) |
| hdmi | 0xF2A24000 | — | cxd90014-hdmi (`hdmi=on`, default off = no cable): TX core as an 8-bit-on-32-bit register file. SYS_STAT +0x024 bit1 HPD / bit2 RSEN / **bit0 = busy, reads 0** (V: power-on cell `hdmi_core_check_p_stable` @0x9984 loops until bit0 = 0); INTR1 +0x1C4 bit6 HPD-change W1C; DDC master +0x3B4..+0x3D0 serves EDID (built-in generic 1080p60, `edid=<file>`); PHY 0x2008 bit0 + 0x4F4 bit0 + HPD = TMDS active → VPH line output. `hpd` run-time settable (`qom-set /machine hdmi on`). V: stock boot/`qom-set` with the cable → detect, EDID parse OK, AVI VIC 0x51C = 16 (1080p60), TMDS on (`docs/RE_HDMI.md` §6) |
| clkrst4 | 0xF2A30000 | — | unimp |
| cec | 0xF3C10000 | — | cxd90014-cec (`hdmi=on` connects the bus): 0x180 status W1C (bit0 RX, bit1 TX no-ACK, bit3 TX ACK), TX 0x000/len 0x040/start 0x044, RX 0x054/count 0x094, own LA 0x0A0. Virtual TV at LA 0 (`tv=on`) ACKs directed frames and answers Give Physical Address / Power Status / CEC Version / OSD Name; polling frame no-ACKs so the camera adopts its address. Cable out = no RX-ready, no churn (also the §4.1 W1C fix). V: logical-address allocation + TV queries, no `set_power` storm (`docs/RE_HDMI.md` §6) |
| CamCore input unit "Pig" | 0xF3120000 (0x500) | 35 | cxd90014-campig, `input-unit=on`. +0x48/+0x4C status (W1C, 32 bit), +0x50/+0x54 their enables bit for bit; IRQ = `(st48 & +0x50) \| (st4c & +0x54)` (V, `43A46272`/`43A4651C`, ISR `436A6904`). **Sgc1 sync generator** +0x210 bit0 run / bit8 continuous, +0x220 = (V−1)<<16 \| (H−1), +0x224 = line of event A (+0x48 bit0); frame end = event B (+0x4C bit0); a one-shot drops +0x210 bit0 at its end (V: live view ~30 events/s, lever-off exposure completes). 27 MHz clock: V for the live 900 900-clock frame, H for the still one-shots; B is held until an enabled A is acked (H, emulator order guarantee). **Iif** +0x410 bit0 run bit cleared at the next frame end (H; V that CamCore start then returns 0 instead of 0x208), +0x410 bit16 reads 0, +0x47C reads back (`docs/RE_CAPTURE_COMPLETION.md` §5, §9) |
| CamCore units 0/1 "Pig" | 0xF3000000 / 0xF3100000 (0x400) | 36 / 37 | cxd90014-campig: path k frame done = +0x48 bit 24+k once per 33 367 µs engine frame, enable +0x50 (`docs/RE_SENSOR_ISP.md` §2.3) — V |
| IDT develop / FOPD control | 0xF3020400 (0xC00; AIF group 0xF3020000 keeps +0x000..+0x3FF) | 72 | cxd90014-idt: +0x42C status W1C, ISR `436ACA24` tests bits 25:24 (01 event 1, 10 event 2) — V; event 1 sets 0x01000001, event 2 0x02000100 (low bits H); +0x444 enables, bit0 event 1 / bit1 event 2 (H; FOPD kick writes 3, live view 0); IRQ = enabled status; +0xC00 bit0 0→1 with +0x444 & 3 starts a frame: event 1 after 100 µs, event 2 at the next completion of AIF group 0xF3020000 (aif "done" GPIO) or a 5 ms fallback (timing H); all other offsets read back. V: lever-off capture passes `F22_wait(0x440)` (`docs/RE_RAW_DEVELOP.md` §2, §7.2) |
| OPD units | 0xF3030000 (0x2000) | 67 / 68 / 69 / 70 (units +0x100 / +0x140 / +0x200 / +0x300) | cxd90014-opd: unit +0x04 status byte W1C, +0x08 enable byte, IRQ = status & enable (V, ISRs `436AC9B4..436ACA08`); +0x0C bit1 0→1 (`436ADA9C`) → after 1 ms status bit0 in every enabled unit and bit1 cleared (H: unit selection, status bit, self-clear, delay); 0xF3031000.. tables read back. V: IRQ 69 completes the synchronous IDT pass `43973F50` (`docs/RE_RAW_DEVELOP.md` §4, §7.3) |
| av-cam memory DMA (16 ch) | 0xF0102000 / 0xF0103000 | 173..188 | cxd90014-mdma: ch k +0x100+0x20·k = {src, dst, cfg}; cfg[11:0] = word count, bit31 = done-IRQ enable; unit +0x00 done status (bit k), +0x08 W1C. Default = memory→memory copy on start (V, entry_pre `.data` + lens TX/RX). **Peripheral-paced mode** (`cxd90014_mdma_set_periph`, `lens=ekit`): a channel whose src/dst is the UART1 DR (0xF2001000) does not copy memory — TX (dst = DR) hands count·width bytes to `a5k-lens` and raises done after the serial time; RX (src = DR) stays armed, the lens writes reply bytes at +0x04 (advancing the current-dst pointer `43993840` measures), delivered on the guest's read of +0x04 (`docs/RE_LENS_EMOUNT.md` §9.4, §10) — V (lens handshake completes) |
| E-mount lens MCU (`a5k-lens`) | mount: GPIO P0.9/P5.4/P0.5/P2.11/P2.10 + UART1 serial via mdma | — | HLE of the kit-lens MCU (`lens=ekit`; no lens firmware exists). P0.9 detect low (property `lens-attached`); supply ack from the sub-micro TLV 0x03 data[3] bit0; handshake echo P5.4→P0.5, P2.11→P2.10 (1 ms, H); then the §9.5 serial protocol over UART1 — answers each type-2 request (blocks 1/7/9/0b/08/0d/10/0a) with the block-table B length, capability bitmap = ids 1,5,6,7,8,9,0x0a,0x0b,0x0d,0x10, power class via `power-class` prop. Reply timing: `LENS_REPLY_NS` 0.6 ms after P5.4 falls, copied at the body's next RX-progress read (H). V: BL state reaches 0x05, lens recognised, no SYNC-TIMEOUT escalation. Lens-info aperture/focal bytes and periodic Info1/2 frames **undecoded** → F-number still "F--" (`docs/RE_LENS_EMOUNT.md` §9.5, §9.8) |
| dmac ch0-3 / ch4-7 | 0xF0000000 / 0xF0010000 | 189..196 | — |
| usb ho / h2xbb | 0xF0200000 / F0203000 | — | — (host side, unused in device mode) |
| usb hdmac | 0xF0204000 | 228, 229 (errors only, never raised) | cxd90014-usbhdc region 1 — V for bulk DMA (`docs/RE_USB_DEVICE_CONTROLLER.md` §5/§9) |
| usb hdc (F_USB20HDC) | 0xF0210000 (0x20000) | 227 | cxd90014-usbhdc region 0; reset values DEVC 0x00010020, EPCTRL 0x500/0x400 are H; enumeration + MTP over A5KU — V (`docs/RE_USB_DEVICE_CONTROLLER.md` §9) |
| mmc0 (SD slot) | 0xF0300000 | 218 | QEMU generic-sdhci (spec 3, capareg 0x017834b4); vendor window +0x100..+0x1FF in the io-f0 register file — V |
| mmc1 (Wi-Fi SDIO) | 0xF0301000 | 221 | generic-sdhci as mmc0 + SDIO card interrupt (NORINTSTS bit 8, level, follows the card while NORINTSTSEN bit 8 is set; `mmc_hw_irq_handler` @0x7d08) — V. Channel = Backup 0x013E000C & 0xF = 1 (S). Card: `ar6003-sdio` (`docs/RE_WIFI.md` §11): CMD5 R4 0x90FF8000, RCA 1, CCCR 0x00 = 0x32, cap 0x12, CIS MANFID 0x0271/0x0301 (bytes H, real module unread); function-1 HIF registers 0x400–0x47F, mailboxes 0x800/0x4000 (V: `ath6kl_sdio` probes, BMI, HTC, WMI). Power: GPIO p1.4 (CHIP_PWD_L) and p6.2/p6.3 (supplies, H) all high = powered, any edge resets the chip — V for the `setPortToStartWlan_Stamina` sequence |
| mmc2 | 0xF0302000 | 224 | — (not mapped) |
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

# USB device controller (F_USB20HDC), HDMAC and VBUS glue: register-level spec for a device-mode model

Scope: everything the unmodified firmware 1.10 kernel driver `f_usb20hdc_udc` (built into vmlinux), its DMA path
(`f_usb20hdc_set_dma_controller` etc., programming the HDMAC directly) and the `fusb_otg` glue expect from the hardware,
written so a code agent can build `cxd90014-usbd` + `cxd90014-hdmac` and connect a virtual host (USB/IP or usbredir).
This document extends `docs/RE_USB.md` (read it first: software stack, connection sequence, userland blocker) and
`docs/RE_STORAGE_USB.md` §6. Facts already proven there are referenced, not repeated.

Evidence:
* Hex-Rays of the whole driver (`emulator/re/ida_stor/stor_vmlinux_usb.c`, local only) post-processed with a script that
  resolves the register-table indirection (see §1.1) into register names; `control_epcmd_register_cache_bits` was
  decompiled freshly (IDA 9.4, copy of `stor_vmlinux.elf.i64`).
* `objdump -m armv7` of `re/vmlinux.elf` for arguments Hex-Rays dropped (all `memcpy` calls).
* Constant tables read straight from vmlinux (`f_usb20hdc_register` @ 0x4029c0e0, EP table @ 0x4029c468, ...).
* WBI snapshot (`tools/wbi.py`) for the fusb state struct and the platform device (IRQ numbers).
* `rootfs_usr/lib/libusb.so` `.rodata` for the endpoint lists each gadget configuration uses.
* No new dynamic run was made for this document. The boot-time accesses observed in RE_USB §2.2/§4.1 (EPCMD0 = 0x800,
  EPCONF0 = 0x80040, CONF autoclear, DEVS bit0 poll) are consistent with everything below.

Conventions: addresses are kernel VAs (peripherals are identity mapped, RE_USB). `hdc` = 0xF0210000, `hdmac` =
0xF0204000, `otg` = 0xF2920000. "W0C" = write 0 to clear, write 1 = no effect. Claims without direct code evidence are
marked **[?]**. Bit *names* are mine unless stated; the upstream Fujitsu driver source was not found (only the f_usb20ho
host patch is public), so no name is taken from upstream. Status tags as in AI_WORKFLOW §2: everything in this file is
**S/H** (static analysis), nothing is **V** yet.

---------------------------------------------------------------------------------------------------------------------

## 0. Summary

| Area | Result | Confidence |
|---|---|---|
| Register file | Complete list of the 105 table registers + 0x18 + 0x104 + buffer RAM + 2 DMA FIFO ports. Write semantics recovered from the per-register "cache bits" callbacks (W0C masks, EPCMD read reconstruction). | high |
| EPCMD / EPCTRL | EPCMD = command strobes (bits 0-8, 18-22) + field writes that mirror EPCTRL fields (bits 9-16, 23-29). EPCTRL = endpoint state + 5 interrupt enables + 5 latched interrupt status bits. Mapping proven by `control_epcmd_register_cache_bits`. | high (layout), medium (meaning of strobes 7, 11, status 28-30) |
| Buffer RAM | CPU-mapped at hdc+0x8000+off (first 0x100 hidden by registers). EP0 IN 0x8100, EP0 OUT 0x8140; EPn at 0x8180+(n-1)·0x800. SETUP is a plain `memcpy` of 8 bytes from hdc+0x8140. | high |
| Device events | DEVS/DEVC bits 24-29: resume, suspend, SOF?, SETUP, bus-reset-end(+speed in DEVS bit17), bus-reset. | high (handlers), medium (names) |
| DMA | Two channels. EP odd → channel 1, EP even → channel 0 (both HDC side and HDMAC side). HDC registers DMACk/DMASk/DMATCIk/DMATCk; HDMAC channel registers at +0x10+0x10·k. Completion is signalled by the **HDC** (INTS bit 8+k, IRQ 227); the HDMAC IRQ (228/229) is only an error path. | high (programming), medium (HDC DMAC bits 2-5, 16-26) |
| VBUS glue | otg+0x10 bit0 active-low VBUS, bit1 ID; pulse GIC 232/233. SDP vs DCP only via hdc+0x104 bits 5/6. | high (from RE_USB) |
| Reset values | Not in the snapshot (MMIO). Derived from what the driver writes after a soft reset and what it polls. | medium |

---------------------------------------------------------------------------------------------------------------------

## 1. How the driver accesses the hardware

### 1.1 Register table and the "cache bits" callbacks
`f_usb20hdc_register` @ 0x4029c0e0: 112 entries `{u32 offset; u8 readable; u8 writable; u16 pad}`, then (at +0x460 =
0x4029c540) 105 function pointers, one per entry 0-104. Every read-modify-write is:
```
v = readable ? readl(base + off) : 0;
cache_bits[idx](base, idx, &v);     // fix up bits the hardware does not read back correctly
v = (v & ~mask) | value;
if (writable) writel(v, base + off);
```
| callback | entries | effect (disassembly) | meaning for the model |
|---|---|---|---|
| `control_default_register_cache_bits` @ 0x401c8a38 | all others | no-op | plain RW |
| `control_ints_register_cache_bits` @ 0x401c8a48 | 3 (INTS) | `v |= 0x318` | INTS bits 3,4,8,9 are W0C; every other INTS bit is written as 0, so those bits must be RO |
| `control_devs_register_cache_bits` @ 0x401c9128 | 21 (DEVS) | `v |= 0xFF000000` | DEVS bits 24-31 are W0C; bits 0-23 RO |
| `control_otgstsc_register_cache_bits` @ 0x401c9144 | 26 (OTGSTSC) | `v |= 0x441` | OTGSTSC bits 0,6,10 W0C (unused in device mode) |
| `control_epcmd_register_cache_bits` @ 0x401c8a64 | 4-19 (EPCMDn) | `v &= 0xFF83FE00`, then OR in fields copied from EPCTRLn, plus bit 11 | EPCMD read value is masked away except bit31 (busy) and bits 9-17/23-31; see §2.6 |

Entries 107-111 (0x4029c438 = `f_usb20hdc_hdmac_register`) are the HDMAC registers, used with the HDMAC base.
Side tables used by the DMA code (dword index from 0x4029c0e0): 224 → {32,36} DMASk; 278 → {31,35} DMACk;
390 → {33,37} DMATCIk; 481 → {34,38} DMATCk; 392-399 → PIO/DMA set/end functions; 483-495 → standard-request handlers;
byte table at +0x604 (0x4029c6e4) = HDMAC request-select values `00 0e 10 11 12 …`.

### 1.2 Driver state (useful for gdb verification)
udc struct (`f_usb20hdc_udc_probe` @ 0x401cf774): +16 bus speed (2 FS, 3 HS), +320 gadget driver, +388 hdc VA, +392 IRQ
227, +396/+400 HDMAC VA for channel 0/1 (both 0xF0204000 = `fusb_otg_get_base_addr(2)`; the fusb struct in the
snapshot at 0x4bd1d880 holds otg/ho/hdc/hdmac/h2xbb/clkrst4 at words 6-11), +404/+408 HDMAC IRQ 228/229 (platform
device 0x40384418 +0x50/+0x54 = 0xE4/0xE5 in the snapshot), +412 + 68·n endpoint n, +1232 device state (0 detached,
2 powered, 5 default, 6 address, 8 suspended), +1241 EP0 stage (§4.3), +1242 "EP0 IN first" flag, +1244+k EP that owns
HDC DMA channel k, +1246+k EP that owns HDMAC channel k.
Endpoint struct E: +20 maxpacket, +24 udc, +28 number, +29 is-IN, +30 type, +32/+34 buffer 0/1 address (hdc offset),
+36 buffer size, +38 number of buffers, +39 PIO-fallback allowed, +40 "wait for EMPTY before completing IN",
+41 HDC DMA channel, +42 HDMAC channel, +43 DMA allowed / request-select index, +44 descriptor (enabled),
+48 current request, +60 halted, +62 "ZLP needed", +63 DMA active, +64 DMA FIFO port physical address.

---------------------------------------------------------------------------------------------------------------------

## 2. HDC register map (hdc = 0xF0210000, size 0x20000, GIC 227)

All registers are 32-bit and accessed with `ldr/str` only, except buffer RAM (§2.9: `memcpy`, any width).

| Offset | Name | Access | Notes |
|---|---|---|---|
| 0x000 | CONF | RW | §2.1 |
| 0x004 | MODE | RW | §2.1 |
| 0x008 | INTEN | RW | §2.2 |
| 0x00C | INTS | bits 3,4,8,9 W0C; rest RO | §2.2 |
| 0x018 | PHYTUNE2 [?name] | RW | not in the table; §2.10 |
| 0x040 + 4n | EPCMDn (n = 0..15) | W (reads: bit31 busy) | §2.6; only 0..11 used |
| 0x104 | LINESTATE [?name] | RO | not in the table; bit5 D+, bit6 D- (RE_USB §1.4) |
| 0x200 | DEVC | RW | §2.3 |
| 0x204 | DEVS | bits 24-31 W0C; rest RO | §2.3 |
| 0x208 | FADDR | RW | §2.4 |
| 0x20C | TSTAMP | RO | [10:0] frame number (`f_usb20hdc_udc_gadget_get_frame` @ 0x401cd6f8) |
| 0x300, 0x310-0x324 | OTGC, OTGSTS(RO), OTGSTSC, OTGSTSFALL, OTGSTSRISE, OTGTC, OTGT | RW | never accessed by the device driver; RAM semantics |
| 0x400/0x404/0x408/0x40C | DMAC1 / DMAS1 (RO) / DMATCI1 / DMATC1 (RO) | | HDC DMA channel 0, §2.5 |
| 0x420/0x424/0x428/0x42C | DMAC2 / DMAS2 (RO) / DMATCI2 / DMATC2 (RO) | | HDC DMA channel 1 |
| 0x500 | TESTC | RW | test modes, §2.4 |
| 0x8000 + 4n | EPCTRLn | RO in practice (§2.7) | |
| 0x8040 + 4n | EPCONFn | RW | §2.8 |
| 0x8080 + 8n / +4 | EPCOUNTnA / EPCOUNTnB | RW lo / RO hi | §2.8 |
| 0x8100-0xFFFF | buffer RAM (CPU view) | RW any width | §2.9 |
| 0x10000-0x17FFF | DMA FIFO port, channel 0 | HDMAC only | §2.9 |
| 0x18000-0x1FFFF | DMA FIFO port, channel 1 | HDMAC only | §2.9 |

### 2.1 CONF (0x000), MODE (0x004)
| Reg | Bit | Use in firmware | Meaning |
|---|---|---|---|
| CONF | 0 | cleared in `f_usb20hdc_initialize_controller` @ 0x401cbf90 (path a2=0) | [?] byte order / bus option; store |
| CONF | 1 | set in the same place | [?] bus option; store |
| CONF | 2 | set, then polled until 0 (≤ 0xFFFF reads): `initialize_controller(a2=1)`, `transition_to_idle`, `enable_host_connect(0)` | **soft reset, self-clearing**. Must reset the device side (DEVC, INTEN, EP state, DMA) to reset values, including DEVC bit16 = 1 (disconnect, §2.3) |
| MODE | 0 | `initialize_controller` returns early if set; cleared on connect (`f_usb20hdc_enable_host_connect` @ 0x401d220c) | host mode [?]; must read 0 in device mode |
| MODE | 1 | set by `transition_to_idle` (then polls DEVS bit0 = 1), cleared in `initialize_controller(0)`, set again on connect | device enable |
| MODE | 2, 3 | set on connect | [?] bit3 is probably "load FADDR after the status stage". The driver writes FADDR inside the SET_ADDRESS SETUP handler, before the status stage (§2.4) |

`initialize_controller(a2=1)` (from `f_usb20hdc_udc_otg_probe` @ 0x401cfb44) saves hdc+0x18, does the CONF bit2 reset, restores hdc+0x18,
and sets INTEN bit2. When HS is disabled it also sets DEVC bit0.

### 2.2 INTEN (0x008) / INTS (0x00C), IRQ 227 (level)
| Bit | INTEN set by | INTS semantics | ISR action (`f_usb20hdc_on_usb_function` @ 0x401d55d4) |
|---|---|---|---|
| 1 | connect (`enable_host_connect(1)`), cleared at disconnect | RO summary [?] | none directly. The ISR tests DEVS/DEVC itself. Model: INTS bit1 = OR(DEVS[31:24] & DEVC[31:24]) [?] |
| 2 | always (both `initialize_controller` paths) | RO summary [?] | none. Model: keep 0, or let it mirror bit1 [?] |
| 3 | connect | latched, W0C | checked first; cleared, nothing else. Model never sets it |
| 4 | cleared | latched, W0C | none. Never set |
| 8 | `f_usb20hdc_enable_dma_transfer(1)` @ 0x401ca4cc for HDC DMA ch0 | latched, W0C | **HDC DMA channel 0 finished**: INTS = (INTS \| 0x318) & ~0x100, then `on_end_in/out_transfer(EP owning ch0)` |
| 9 | same, ch1 | latched, W0C | same for channel 1 |
| 16 | `f_usb20hdc_initialize_endpoint.clone.37` for EP0 | RO summary of EPCTRL0 | EP0 handler (§2.7) |
| 16+n (n = 1..11) | same, for EPn | RO summary of EPCTRLn | EPn handler; checked **before** EP0 |

Model: INTS[16+n] = OR(EPCTRLn[30:26] & EPCTRLn[22:18]). IRQ 227 = OR(INTS & INTEN) over bits 1-4, 8, 9, 16-27,
level-triggered. The ISR handles one event per call and returns IRQ_HANDLED, so it relies on the line staying high
while anything is pending.

ISR priority order: INTS3 → DEVS25 → DEVS24 → DEVS29 → DEVS28 → DEVS26 → DEVS27 → INTS8 → INTS9 → EP1..EP11 → EP0.

### 2.3 DEVC (0x200) / DEVS (0x204)
DEVS bit k (k ≥ 24) is an event; the DEVC bit with the same number is its enable. Status is cleared with
`(v & 0xFFFFFF) | (0xFF000000 & ~(1<<k))` (W0C).

| Bit | DEVC (RW) | DEVS | Evidence / handler |
|---|---|---|---|
| 0 | **force full speed** (set when `fusb_otg_gadget_get_hs_enable()` == 0; that value comes from otgcore ioctl 0x4001E30C → `fusb_set_speed` @ 0x401d8814) | **must read 1** after MODE bit1 is set (`transition_to_idle` polls it). Existing board preset | |
| 1 | — | level: when set, `f_usb20hdc_is_setup_transferred` @ 0x401ca258 treats the control transfer as aborted [?: suspend/reset-in-progress]. Keep 0 | |
| 2 | remote-wakeup signalling (`f_usb20hdc_udc_gadget_wakeup` @ 0x401cd620 sets it in state 8) | — | |
| 3 | cleared at init [?] | — | |
| 5 | cleared on pull-up enable; `fusb_start_host` @ 0x401d8a30 requires it to read 1 | — | reset value **1** [?] |
| 16 | **soft disconnect**: cleared = D+ pull-up on (`enable_host_connect(1)` when the pull-up flag is set, `f_usb20hdc_udc_gadget_pullup` @ 0x401d32d4). The driver only ever sets it again through the CONF bit2 soft reset, so reset value = **1** [?] | must read 0 at the end of `initialize_controller(0)` (polled) | |
| 17 | — | **bus speed: 1 = full speed, 0 = high speed**. Read in the bit28 handler and for udelay selection | |
| 24 | resume IE | resume (W0C) | state ← previous, gadget->resume (the handler tests DEVS twice and never DEVC: a firmware bug, harmless) |
| 25 | suspend IE | suspend (W0C) | state 8, gadget->suspend |
| 26 | not enabled by the driver | W0C | clear only [?: SOF]. Never set |
| 27 | SETUP IE | **SETUP received** (W0C) | §4.3 |
| 28 | reset-end IE | **bus reset finished / speed known** (W0C) | speed = DEVS17 ? FS : HS; EP0 maxpacket 64; EPCONF0[23:13] = 64; state 5 (DEFAULT) |
| 29 | bus-reset IE | **bus reset** (W0C) | abort all EPs, re-init EP0/EPn, reset DMA, dequeue all (-103); gadget->disconnect if configured; state 0. Returns immediately (one event per call) |
| 30, 31 | cleared | W0C, unused | never set |

`enable_host_connect(1)` sets DEVC bits 24, 25, 27, 28, 29 (not 26). `initialize_controller(0)` clears DEVC bits 2, 3,
24-31 and all DEVS events.

### 2.4 FADDR (0x208), TSTAMP, TESTC
* FADDR[6:0] = device address, written by `f_usb20hdc_respond_standard_request_set_address` @ 0x401ca394 **during the
  SETUP stage** (state 5 or 6 only, wIndex = 0, wLength = 0). FADDR bit8 is cleared at init [?]. A virtual-host model
  can store FADDR and otherwise ignore it: USB/IP and usbredir carry no device address.
* TSTAMP[10:0]: frame number, read only by `get_frame`. Optional; a 1 kHz counter is enough.
* TESTC bits 0-3: test modes (gadget ioctl 4: case 4 → bit0, 1 → bit1, 2 → bit2, 3 → bit3, 0 → clear all). Store only.

### 2.5 HDC DMA channel registers (k = 0: 0x400.., k = 1: 0x420..)
EP → channel mapping (EP table @ 0x4029c468, bytes 13/14; both the HDC and the HDMAC channel use the same number):
EP1, 3, 5, 7, 9, 11 → **channel 1**; EP2, 4, 6, 8, 10 → **channel 0**; EP0 none.

| Reg | Bits | Written by `f_usb20hdc_set_dma_controller` @ 0x401ca9d8 / `enable_dma_transfer` | Meaning |
|---|---|---|---|
| DMACk | 0 | set to start, cleared to stop/abort | **start/enable** |
| DMACk | 2 | cleared every setup | [?] |
| DMACk | 3 | IN, final chunk: = E+62 (ZLP needed); non-final: 0 | send a zero-length packet after the last byte [?] |
| DMACk | 4 | IN, final chunk: = E+40 (1 for EP1-11); non-final: 0 | [?] report end only once the buffers are empty |
| DMACk | 5 | OUT: cleared | [?] |
| DMACk | 11:8 | endpoint number | which EP's buffers the FIFO port serves |
| DMACk | 26:16 | cleared, then bit18 (word mode, length not a multiple of 64) or bit22 (64-byte burst mode) | [?] request/burst mode; must match the HDMAC programming (§5). Store only |
| DMASk | 1 | read in `f_usb20hdc_end_out_transfer_set_request_status` @ 0x401c9994 (DMA path): set means the OUT transfer ended early | **ended by a short packet / ZLP** |
| DMASk | 0 | read in `f_usb20hdc_end_out_transfer_dma` @ 0x401d44b0: when 0 (and DMATC is a packet multiple ≠ DMATCI) the driver adds EPCOUNT[cur].hi to the byte count | [?] "last (short) packet was moved by DMA". Model: set to 1 whenever the short packet was moved |
| DMATCIk | 31:0 | total byte count of this DMA chunk (≤ 0x400000, multiple of maxpacket unless it is the whole remainder) | |
| DMATCk | 31:0 | read only | bytes actually moved (OUT: compared with DMATCI) |

`initialize_controller(0)` clears DMACk bits 0, 2-5, 8-11, 16-26 and writes DMATCIk back unchanged.

### 2.6 EPCMDn (0x040 + 4n)
Reconstruction done by `control_epcmd_register_cache_bits` before every write (`v &= 0xFF83FE00`, then):
```
EPCMD[9]  = EPCTRL[16]   EPCMD[10] = EPCTRL[17]   EPCMD[11] = 1 (constant)
EPCMD[12] = EPCTRL[18]   EPCMD[13] = EPCTRL[19]   EPCMD[14] = EPCTRL[20]
EPCMD[15] = EPCTRL[21]   EPCMD[16] = EPCTRL[22]
EPCMD[24:23] = EPCTRL[2:1]   EPCMD[25] = EPCTRL[3]   EPCMD[27:26] = EPCTRL[5:4]   EPCMD[29:28] = EPCTRL[15:14]
```
So **every EPCMD write carries the full value of these EPCTRL fields**, and the model applies them on every write. Bits
0-8 and 18-22 are one-shot strobes (always written 0 unless issued). Bit31 = busy, polled `>= 0` before and after every
command (≤ 0xFFFF reads): the model reads EPCMD as 0 and executes commands immediately.

| EPCMD bit | Kind | Effect (evidence) |
|---|---|---|
| 0 | strobe | **START**: EPCTRL0 EN = 1 (`f_usb20hdc_enable_endpoint` @ 0x401d1f6c, after INIT) |
| 1 | strobe | **STOP**: EN = 0, followed by a udelay (`initialize_endpoint`, `enable_endpoint(0)`) |
| 2 | strobe | **INIT**: reset the buffer state (all empty, CPU/USB pointers 0, counts 0). Issued in every `initialize_endpoint` |
| 3 | strobe | **BUFWR**: the CPU-side buffer (EPCTRL[7:6]; for EP0 the IN buffer) is full with EPCOUNT[buf].lo bytes. Advance the CPU pointer. Count 0 = ZLP |
| 4 | strobe | **BUFRD**: the CPU has consumed the CPU-side buffer (for EP0 the OUT buffer). Mark it empty and advance |
| 5 | strobe | **STALL set** → EPCTRL12 = 1 (`f_usb20hdc_halt_transfer` @ 0x401ceae0) |
| 6 | strobe | **STALL clear** → EPCTRL12 = 0 (halt clear; `initialize_endpoint(a2=1)`) |
| 7 | strobe | never issued [?: toggle set] |
| 8 | strobe | **data toggle reset** (halt clear, `initialize_endpoint(a3=1)`). Model: no-op |
| 9 | field → EPCTRL16 | [?] NULL-response mode. Never set by the driver |
| 10 | field → EPCTRL17 | **NAK all** (OUT): set briefly in `f_usb20hdc_set_out_transfer_pio` @ 0x401cb56c for DMA-capable EPs |
| 11 | constant 1 | [?]. Ignore |
| 12-16 | fields → EPCTRL18-22 | interrupt enables for status bits 26-30 |
| 18-22 | strobes | clear EPCTRL status bit 26-30 respectively |
| 24:23 | field → EPCTRL[2:1] | endpoint type = bmAttributes & 3 (0 control, 1 iso, 2 bulk, 3 interrupt) (`f_usb20hdc_udc_ep_enable` @ 0x401d349c) |
| 25 | field → EPCTRL3 | direction, 1 = IN |
| 27:26 | field → EPCTRL[5:4] | number of buffers - 1 (EP table byte10 = 2 → 1; EP0 = 0) |
| 29:28 | field → EPCTRL[15:14] | high-bandwidth multiplier = (wMaxPacketSize >> 11) & 3 |

### 2.7 EPCTRLn (0x8000 + 4n)
The driver writes it only in `initialize_controller(0)`: EPCTRL0 = **0x500**, EPCTRL1..11 = **0x400**. These are the
expected reset values (empty buffers). Model: treat a write as "restore reset state" (or ignore it) and log it; all
fields are owned by EPCMD and the endpoint engine.

| Bit | Meaning | Evidence |
|---|---|---|
| 0 | EN (endpoint enabled) | tested before STOP/START in `initialize_endpoint` |
| 2:1, 3, 5:4, 15:14 | type, dir, buffers-1, multiplier (from EPCMD) | §2.6 |
| 7:6 | **CPU-side buffer index** (which buffer the CPU writes/reads next) | `(u8)EPCTRLn >> 6` selects E+32/E+34 and EPCOUNTn A/B in all PIO paths of EP1-11 |
| 8 | EP0 only: IN buffer empty [?] | reset value 0x500 |
| 9 | EP0 only: **IN buffer full**. `set_in_transfer_pio`/`end_in_transfer_pio` refuse to write when set | |
| 10 | EPn: **all buffers empty**; EP0: OUT buffer empty. `end_out_transfer_pio` aborts when set; `set_out_transfer_pio` uses it | |
| 11 | EPn: **all buffers full** (IN EPs refuse to write when set); EP0: OUT full [?] | |
| 12 | **STALLed** | `halt_transfer` |
| 16, 17 | NULL-resp [?], NAK-all (from EPCMD 9/10) | |
| 18-22 | IE for status 26-30 (from EPCMD 12-16) | |
| 26 | status "READY". EPn IN: a buffer became free for the CPU. EPn OUT: a buffer holds received data. **EP0: IN buffer consumed** | ISR → `on_end_in/out_transfer` (EPn), `on_end_control_in_transfer` (EP0) |
| 27 | status "EMPTY". EPn IN: all buffers transmitted. **EP0: OUT buffer holds a packet** (data stage or status-stage ZLP) | ISR → `on_end_in_transfer` (EPn, last step of an IN request), `on_end_control_out_transfer` (EP0) |
| 28 | status [?] | cleared only. Never set |
| 29 | status [?] (IE bit21 is enabled by every `initialize_endpoint`) | ISR only clears it. Never set (stall-handshake event?) |
| 30 | status [?] | cleared only. Never set |

ISR per EP: test status26 & IE18, then status27 & IE19, then status29 & IE21. The status is read three times in a row
until three equal reads are seen (debounce), so the value must be stable.

### 2.8 EPCONFn (0x8040 + 4n) and EPCOUNTnA/B (0x8080 + 8n, +4)
* EPCONFn[12:0] = buffer-0 address >> 2 (13 bits, with the 0x8000 offset truncated away: 0x8100 → 0x040).
  [23:13] = maxpacket. [28:25] = endpoint number (bit 24 written 0; `udc_ep_enable` writes `ep << 25` under mask 0xE0FFFFFF).
  Buffer k of a multi-buffer EP is at buffer 0 + k · align4(maxpacket): `udc_ep_enable` recomputes buffer 1 as
  `align4(buf0 + maxpacket)`, and EP0's OUT buffer is buf0 + 64.
  Boot-time value EPCONF0 = 0x00080040 (seen in the emulator log).
* USB endpoint number N is always served by hardware endpoint N. `udc_ep_enable` indexes the hardware EP by
  `bEndpointAddress & 0xF`.
* EPCOUNTn{A,B} = buffer 0 / buffer 1. **[10:0] = byte count written by the CPU for IN**; **[26:16] = byte count received
  for OUT** (hardware-owned). EP0: A = IN buffer, B = OUT buffer (the SETUP check is `(EPCOUNT0B >> 16) & 0x7FF == 8`).
  `initialize_controller` writes all 24 registers back with their own values.

### 2.9 Buffer RAM and DMA FIFO ports
* CPU view: byte address hdc + 0x8000 + off, where off = EPCONF[12:0] << 2 (0x100 ≤ off < 0x8000). The driver uses
  `memcpy(hdc + E->buf[idx], …)` (asm: `ldrh r3,[r3,#32]; add r0,r0,r3; bl memcpy` @ 0x401ce390, 0x401d40e0, 0x401d6124).
  Kernel `memcpy` performs byte, halfword, word and `ldm/stm` accesses, so the region must be **byte-addressable RAM**.
* Layout computed by `f_usb20hdc_initialize_endpoint_configure` @ 0x401c9c0c (≤ 0x6000 checked in `udc_probe`):

| EP | buffer 0 | buffer 1 (after `ep_enable`: buf0 + align4(mps)) | size each / count |
|---|---|---|---|
| EP0 | 0x8100 (IN) | 0x8140 (OUT) | 64 / 2 |
| EPn (1..11) | 0x8180 + (n-1)·0x800 | initially +0x400; HS bulk → +0x200, FS → +0x40 | 0x400 / 2 |

* DMA ports: E+64 = `resource.start + buf0 - 0x8000 + {0x10000, 0x18000}[HDMAC channel]` (`udc_probe`, table entries 105/106),
  for example EP1 → 0xF0228180 and EP2 → 0xF0220980. The HDMAC accesses this address with a **fixed** address (FS or FD
  set), 32-bit beats. Model: any access in 0x10000-0x17FFF is the channel-0 FIFO, and 0x18000-0x1FFFF the channel-1 FIFO.
  It streams to or from the buffers of the EP selected in DMACk[11:8]. The offset inside the window is not significant [?].

### 2.10 PHY tuning
* 0xF020200C (PHY block 0xF0202000) and hdc+0x18: `(old & 0xFFFFF888) | (cfg & 0x77) | ((cfg << 8) & 0x700)` (RE_USB §1.3).
  `initialize_controller(1)` saves and restores hdc+0x18 around the soft reset. Model: plain RW storage. No effect.
* hdc+0x104 (line state, `usb_otgcore_get_line_state` @ 0x401da85c): bit5 = D+, bit6 = D-. Return **0** for a PC host
  (SDP), **0x60** for a dedicated charger (DCP). See §6.

### 2.11 Reset values (after power-on and after CONF bit2)
| Register | Value | Source |
|---|---|---|
| CONF | 0 (bit2 self-clears) | existing board (V for boot) |
| MODE | 0 | MODE bit0 must be 0 or the driver skips the device init |
| DEVS | 0x00000001 (bit0 = 1, bit16 = 0, bit17 = 0 until speed is known) | polls (H) |
| DEVC | 0x00010020 (disconnected, bit5 set) | H: §2.3 |
| EPCTRL0 / EPCTRLn | 0x00000500 / 0x00000400 | driver re-writes these after reset (H) |
| INTEN, INTS, FADDR, EPCONF, EPCOUNT, DMAC*, TESTC, +0x104 | 0 | H |
| buffer RAM | undefined (driver never relies on it) | |

---------------------------------------------------------------------------------------------------------------------

## 3. Endpoint engine rules the model must implement

Per hardware EP: N = EPCTRL[5:4] + 1 buffers (EP0: one IN buffer and one OUT buffer), each with a full flag and a length.
There is a CPU pointer (exposed as EPCTRL[7:6]) and a USB pointer (internal).

1. **IN (EPn with DIR = 1, or EP0's IN buffer)**. BUFWR marks buffer[cpu] full with length EPCOUNT[cpu][10:0] and
   advances cpu. A host IN token takes buffer[usb] if it is full: it sends the data (a ZLP if the length is 0), marks the
   buffer empty, advances usb, and **latches status26**. If no buffer is full, the model NAKs (holds the URB).
2. **OUT (EPn with DIR = 0, or EP0's OUT buffer)**. If EN = 1, NAK-all = 0, not stalled and buffer[usb] is empty: store
   the packet, set EPCOUNT[usb][26:16] = length, mark it full, advance usb, and latch **status26 (EPn) / status27 (EP0)**.
   Otherwise NAK. BUFRD marks buffer[cpu] empty and advances cpu. **If another buffer is still full, latch the ready
   status again.** Otherwise a double-buffered OUT EP stalls.
3. **Never latch a READY status while its condition is false.** `end_out_transfer_pio` @ 0x401d3edc on an EMPTY EP, and
   `end_in_transfer_pio` @ 0x401d4bb0 on a FULL IN EP, call the abort path, which re-initialises the endpoint. So the
   clear strobe (EPCMD 18-22) must not re-latch the status by itself. The simplest safe model keeps at most one packet in
   flight per direction per EP. Double-buffer pipelining is optional.
4. **EMPTY status27 on IN EPn** is best modelled as a level (all buffers empty). The driver enables it (EPCMD bit13)
   only after queueing the last packet, and it may already be true at that moment.
5. EP-flags EPCTRL bit8/9 (EP0 IN empty/full), bit10/11 (OUT or EPn empty/full) are live views of the buffer state.
6. **STALL** (EPCTRL12): the host gets a STALL handshake for that EP. **EP0's stall must be cleared by hardware when the
   next SETUP arrives.** The firmware never clears it: `halt_transfer(ep0,1,0)` is used as protocol stall
   (`f_usb20hdc_on_usb_function` SETUP path).
7. **INIT** empties all buffers and resets both pointers. STOP sets EN = 0 (the host is NAKed). START sets EN = 1.
8. When DMACk is running for an EP (§5), packets move between the buffers and the FIFO port without BUFWR/BUFRD. The
   READY interrupts are disabled by the driver at that time (`abort_*_transfer_pio` clears EPCMD 12/13).

---------------------------------------------------------------------------------------------------------------------

## 4. Event sequences

### 4.1 Plug-in (VBUS) up to pull-up
1. Board: otg+0x10 bit0 = 0 (VBUS present), keep bit1 = 1, **pulse GIC 232** (§6). If userland has not yet started USB
   control, `fusb_start_control` @ 0x401d9918 samples the level itself.
2. Firmware (RE_USB §2.3): otgcore event → libUsbControl → `start_gadget`: insmod the function, ioctl 0x4001E30C (speed),
   0xE305 → `fusb_start_gadget` (otg+0x08 = 16), gadgetcore descriptors, portmon 0xF000 (reads hdc+0x104 several times;
   0 → SDP).
   **Prerequisite outside this spec:** in the emulator userland currently never issues 0xE305 (RE_USB §4.2).
3. `usb_gadget_probe_driver` → `f_usb20hdc_enable_communicate` @ 0x401d2e68 → `f_usb20hdc_udc_otg_probe` (CONF reset,
   request IRQ 227/228/229) → `enable_host_connect(1)`:
   init DMA → `initialize_controller(0)` (DEVS bit16 poll) → MODE = (MODE & ~1) | 0xE → INTEN |= 0x0A →
   DEVC |= 0x3B000000 → EP layout → when the gadget asked for the pull-up: **DEVC &= ~0x10000, DEVC &= ~0x20**.
4. Model: on the DEVC bit16 1 → 0 edge, with MODE bit1 set and VBUS present, the device is "attached". Report it to the
   transport (USB/IP device becomes importable, or usbredir connect) and start §4.2.

### 4.2 Bus reset and speed
1. Set DEVS bit29 (when DEVC bit29 is set). Wait until the firmware clears it (W0C). The handler re-inits all EPs and
   EP0 and issues STALL-clear and toggle-reset on EP0.
2. Set DEVS bit17 = 1 if full speed (DEVC bit0 set or the transport is FS), else 0. Set DEVS bit28. The handler sets
   EP0 mps 64 and state DEFAULT.
3. HS is the normal case: bulk EPs then require wMaxPacketSize = 512 (`udc_ep_enable` rejects anything else at HS).
   FS requires ≤ 64.
4. SET_ADDRESS arrives as an ordinary SETUP (§4.3). FADDR is written in the SETUP stage. The model may ignore it.

### 4.3 Control transfers on EP0
SETUP delivery (model):
1. Optionally wait until no DEVS bit27 is pending (the transport serialises control transfers anyway).
2. Clear EP0 STALL. Write the 8 bytes to buffer RAM at EPCONF0 buf0 + 64 (= hdc+0x8140), set EPCOUNT0B[26:16] = 8 and
   mark the OUT buffer full. Do this **even if the OUT buffer is still full**: the status-stage ZLP of the previous IN
   transfer is never read with BUFRD.
3. Set DEVS bit27 (DEVC bit27 enabled) → IRQ 227.

Firmware SETUP handler (`f_usb20hdc_on_usb_function` +0xad0.., memcpy @ 0x401d6138): clear EP0 status26/27 (EPCMD 18/19) → check count == 8
(else stall) → dequeue old requests (-71) → `memcpy` 8 bytes from hdc+0x8140 → BUFRD → disable EP0 IE 12/13 → stage =
1 (IN data) if bmRequestType bit7, 2 (OUT data) if wLength ≠ 0, else 3 (no data) → dispatch (standard requests through
the table at 483+bRequest, class/vendor to the gadget). Unless the gadget returned "delayed" (>0), the handler pre-arms
the status stage:
* OUT direction or no data: EPCOUNT0A = 0, BUFWR, enable IE12 → **a ZLP is pre-loaded into the IN buffer**. The model must
  hand it to the host only as the status stage, after the OUT data stage.
* IN direction: enable IE13 (wait for the host's status ZLP).

Control IN (stage 1): `ep_queue(ep0)` → `set_in_transfer_pio`: EPCTRL0 bit9 must be 0 → memcpy ≤ 64 bytes into
hdc+0x8100 → EPCOUNT0A = n → BUFWR → IE12. Model: give the packet to the host and latch status26. The firmware
(`on_end_control_in_transfer` @ 0x401d553c → `end_in_transfer_pio`) writes the next packet, a ZLP when length % 64 == 0
and `req->zero`, or finishes with stage 4. The model collects packets until it has a short packet or wLength bytes.
Then **status stage**: inject a 0-byte OUT packet into the EP0 OUT buffer (EPCOUNT0B hi = 0, status27). The firmware
completes the request only on this event (`on_end_control_out_transfer` @ 0x401d4324, stage 4).

Control OUT (stage 2): `ep_queue(ep0)` → `set_out_transfer_pio`: IE13. Model: deliver the data in ≤ 64-byte packets
into the OUT buffer (status27 each, wait for BUFRD before the next). The firmware reads EPCOUNT0B hi, does the memcpy and
BUFRD, and moves to stage 3 when complete. **Status stage**: the pre-armed ZLP in the IN buffer is taken (latch
status26) and the firmware completes the request (stage 3 → done).

No-data (stage 3): status stage only = take the pre-armed IN ZLP, latch status26.

Stall: if EPCTRL0 bit12 is set when the model would transfer data or status, return STALL to the host for this control
transfer. Clear it on the next SETUP.

### 4.4 Bulk/interrupt OUT
* PIO (zero-length requests, unaligned buffers, or the DMA channel is busy with another EP; `f_usb20hdc_set_out_transfer`
  @ 0x401cbb40): the firmware enables IE12. Each received packet goes into buffer[usb], EPCOUNT hi = len, status26.
  The firmware reads buffer[EPCTRL[7:6]] and issues BUFRD. The request ends on a short packet (including a ZLP) or when
  full (`end_out_transfer_set_request_status`).
* DMA (`f_usb20hdc_set_out_transfer_dma` @ 0x401cdf00, buffer 4-byte aligned): HDMAC programmed with src = FIFO port
  (FS = 1), dst = memory; DMATCIk = L (multiple of mps unless L < mps, ≤ 4 MiB); DMACk[11:8] = EP; then
  `enable_dma_transfer(1)`: HDMAC DMACB EI, INTEN bit(8+k), HDMAC DMACR DE, DMACA EB, DMACk bit0.
  Model: for each packet received while DMACk runs, copy it to the destination (DMACDA + DMATC), DMATCk += len,
  release the buffer. Stop when DMATCk == DMATCIk, or on a packet shorter than mps (set DMASk bit1, and bit0 [?]).
  Then DMACk bit0 = 0, HDMAC EB = 0, SS = 5, **INTS bit(8+k) = 1**.
  A ZLP ends the transfer with DMATC unchanged, and its buffer must read EPCOUNT hi = 0.
  The firmware (`on_end_out_transfer` @ 0x401cd594 → `end_out_transfer_dma`) either completes or programs the next
  chunk.
* If more data arrives than requested (overflow, -75), the firmware copies the excess with `memcpy` from the CPU-side
  buffer and BUFRD.

### 4.5 Bulk/interrupt IN
* PIO: per packet `memcpy` → EPCOUNT lo → BUFWR → IE12 (`set_in_transfer_pio` @ 0x401ce140 first clears a stale
  status26). The model sends it on the next host IN and latches status26. After the last packet:
  * if the length is a multiple of mps and `req->zero`, the firmware writes a ZLP (EPCOUNT lo = 0, BUFWR);
  * because E+40 = 1 and N = 2, the firmware then enables IE13 and completes on status27 (all buffers empty).
* DMA (`set_in_transfer_dma` @ 0x401cdc7c): src = memory, dst = FIFO port (FD = 1), DMATCIk = L (chunks ≤ 4 MiB rounded
  to mps; the last chunk carries DMACk bit3 = ZLP needed and bit4 = 1). Model: pull bytes from DMACSA + DMATC into free
  buffers in mps packets (the last one short), DMATCk += len. Send them on host IN tokens, then a ZLP if bit3 is set.
  Set INTS bit(8+k) when DMATC == DMATCI **and** the buffers have been transmitted (safe for bit4 = 1). The firmware
  (`end_in_transfer_dma` @ 0x401d8480) starts the next chunk or completes.
* Interrupt IN (PTP event EP3, 32 bytes) takes the same paths. Bulk/interrupt ZLP requests always use PIO.

### 4.6 Disconnect, re-enumeration, mode switch
* Cable pull: otg+0x10 bit0 = 1, pulse GIC 232 → `fusb_signal_event` @ 0x401d8e3c → (state 3) gadget callback →
  `f_usb20hdc_on_detect_bus_connect` @ 0x401d325c → `enable_host_connect(0)`: INTEN &= ~0x0A, DEVC &= ~0x3B000000,
  clear INTS/DEVS, stop all EPs, reset DMA, `initialize_controller(0)`, dequeue all (-108), and, if the pull-up flag was
  set, **CONF bit2 soft reset** (which must set DEVC bit16 = 1 again). Model: report a detach to the transport and drop
  pending URBs.
* Gadget switch without unplugging (PMCA `switchToAppInstaller`, updater switch): pull-up off (`gadget_pullup(0)` →
  `enable_host_connect(0)` → soft reset), then pull-up on with a new gadget. Model: DEVC bit16 0 → 1 (or a soft reset)
  = detach, 1 → 0 = attach. Each attach gets a new bus reset (§4.2). The host side must see a new device (new PID).
* Suspend/resume (DEVS 25/24) are not needed. A virtual host never suspends.

---------------------------------------------------------------------------------------------------------------------

## 5. HDMAC (0xF0204000) as used by this driver

Two channels: k = 0 at hdmac+0x10, k = 1 at hdmac+0x20 (`16*ch` + table offsets 0x10/0x14/0x18/0x1C). Global DMACR at
hdmac+0x00. The bit positions match the layout of the upstream milbeaut-hdmac driver (cross-check only). Names are
upstream's, meanings are from this driver's use.

| Reg | Bits | Driver use (`set_dma_controller`, `enable_dma_transfer`, `initialize_dma_controller` @ 0x401c9164, `on_end_dma_transfer` @ 0x401d8534) |
|---|---|---|
| DMACR +0x00 | 31 DE | set (global enable) on every start |
| DMACA +0x10 | 31 EB | set to start. On stop the driver clears it and **polls until it reads 0**. Model: the hardware clears it at the end of the transfer |
| | 30 PB, 29 ST | cleared at init (no pause, no software trigger: hardware request) |
| | 28:24 IS | = byte table 0x4029c6e4[E+43] = **0x0E** for every bulk EP (request from the USB HDC) [?: both channels use the same IS value] |
| | 23:20 BT | 0x8 when L % 64 ≠ 0 (TC counts 32-bit words), 0xF when L % 64 == 0 (TC counts 64-byte blocks) |
| | 19:16 BC | 0 |
| | 15:0 TC | words: `ceil(L/4) - 1`; 64-byte mode: `L/64 - 1`. So bytes = (TC+1)·4 or (TC+1)·64 (the driver's arithmetic; [?] general BT semantics) |
| DMACB +0x14 | 31:30 TT | 0 |
| | 29:28 MS | 2 (demand) in word mode, 0 (block) in 64-byte mode |
| | 27:26 TW | 2 (32-bit) |
| | 25 FS / 24 FD | FS = 1 for OUT (source = FIFO port), FD = 1 for IN (destination = FIFO port) |
| | 23 RC, 22 RS, 21 RD | 0 (no reload) |
| | 20 EI | set in `enable_dma_transfer(1)`, cleared on stop |
| | 19 CI | cleared at init and never set → **no completion IRQ** [?: name] |
| | 18:16 SS | stop status. The ISR treats 1-4 as an error (abort EP, dequeue -1) and anything else as normal; it is cleared by writing 0 to the field. Model: write 5 (normal end) on completion, or leave 0 |
| | 15:8 | cleared at init (protection) |
| DMACSA +0x18 / DMACDA +0x1C | | physical addresses (memory side, or the FIFO port E+64) |

IRQs: channel 0 → GIC **228**, channel 1 → **229** (platform device +0x50/+0x54). With CI = 0 the model never raises
them in normal operation. Raise them only for error stops with EI set (optional). **Completion is the HDC INTS bit
8+k.**

Model options:
(a) Faithful: an HDMAC device that performs AHB reads/writes. The HDC asserts a per-channel demand while its EP has
data/space, and HDMAC accesses to the FIFO port move data.
(b) Pragmatic: the HDC model performs the copy itself, using HDMAC DMACSA/DMACDA (incrementing the memory side) when
DMACk bit0 and HDMAC EB, DMACR DE are all set. It then updates DMATCk and clears EB.
The firmware cannot tell the two apart: it only reads EB, SS, DMATCk and DMASk.

---------------------------------------------------------------------------------------------------------------------

## 6. VBUS / ID glue (otg = 0xF2920000)

(from RE_USB §1.5 and RE_STORAGE_USB §6.2, re-checked in `fusb_interrupt_usb_vbus` @ 0x401d9200, `fusb_notify_vbus`,
`fusb_otg_get_vbus` @ 0x401d8c08, `fusb_start_control`, `fusb_start_gadget` @ 0x401d88e8)
* otg+0x10: bit0 = VBUS **active-low** (0 = present; `fusb_otg_get_vbus` returns 3 when the stored bit is 0),
  bit1 = ID (1 = float/B-device). No cable = 3, PC cable = 2.
* GIC 232 (VBUS) / 233 (ID): the ISRs re-read +0x10 and post an event only on change; they never clear anything →
  **pulse** on every change. They are enabled only between `fusb_start_control` and `fusb_stop_control`.
* `fusb_start_gadget` also requires ho+0x54 bit12 = 0 (EHCI PORTSC; reads 0 today).
* Writes to store: +0x00 (bit4 cleared at idle), +0x04 = 16 (host), +0x08 = 16 (gadget start), +0x40/+0x50 |= 0x100/0x200 (IRQ enables).
* **PC host (SDP) vs charger:** the glue does not report the type. `usb_portMonitor` (`sub_D8C`, mode byte 1) polls
  `usb_otgcore_get_line_state` and requires D+ = D- = 1 for more than `word_2C8E` of `word_2C8C` samples for a DCP
  (type 3). Otherwise the type is SDP (4). In mode 2 it asks the charger-IC message server instead. So:
  PC host → hdc+0x104 = 0; wall charger → 0x60 and no pull-up/bus traffic.

---------------------------------------------------------------------------------------------------------------------

## 7. Minimal subsets

Endpoint lists from `libusb.so` (16-byte records {addr, attr, wMaxPacket, interval, nbuf, …}: HS @ +0, FS @ +0x30):

| Function (PID) | Endpoints |
|---|---|
| MSC `usbg_storage` (0x047D, also the updater 0x03E2) | EP1 IN bulk, EP2 OUT bulk (512 HS / 64 FS) |
| PTP/MTP `usbg_stillimage` (Auto 0x02D7, PictBridge 0x034A, PC-Remote 0x0641, app-download 0x06A8) | EP1 IN bulk, EP2 OUT bulk, EP3 IN interrupt 32 B (bInterval 7 HS / 10 FS) |
| SENSER `usbg_sen` (0x0336) | EP1 IN, EP2 OUT, EP3 IN, EP4 OUT, all bulk (vendor class 0xFF) |
| HID bus-power `usbg_buspower` (0x04EF) | EP1 IN interrupt 32 B |

Common core (needed by every function):
* OTG glue +0x10 with GIC 232 pulses; hdc+0x104 = 0.
* CONF (bit2 self-clear), MODE, INTEN/INTS (bits 8, 9, 16-27; level IRQ 227), DEVC/DEVS (bits 0, 16, 17, 24-29),
  FADDR store, EPCMD (strobes 0-6, 8, 18-22; fields), EPCTRL (bits 0-15, 18-22, 26, 27; status 28-30 never set),
  EPCONF, EPCOUNT, buffer RAM 0x8100-0xDFFF, the EP0 sequences of §4.3, bus reset §4.2, attach/detach §4.6.
* Not needed: TSTAMP (optional), TESTC, OTG* 0x3xx, suspend/resume, DEVS 26/30/31, INTS 3/4, EPCTRL status 28-30,
  HDMAC IRQs, iso.

Per function:
* **MSC (bulk-only):** core + EP1/EP2. Both channels are free (EP1 → ch1, EP2 → ch0), so with aligned buffers **all
  data and CBW/CSW traffic uses DMA**: HDMAC + HDC DMAC1/DMAC2 are mandatory. CBW (31 bytes) reaches the DMA word mode
  (TC = 7, MS = 2, DMATCI = 31). Zero-length requests use PIO. Sony-PMCA-RE's MSC ext-cmd (SCSI 0x7A) needs nothing
  more.
* **PTP/MTP (info/install over MTP 0x9280/0x9281, 0x9488…):** core + EP1/EP2 (DMA) + EP3 interrupt IN. EP3 shares DMA
  channel 1 with EP1, so event packets run over PIO while a bulk-IN DMA is active (`is_pio_transfer_auto_change_usable`
  → true), and the other way round. **PIO and DMA both have to work.**
* **SENSER (`usbg_sen`, PMCA "senser shell"):** core + EP1-EP4 bulk. EP1/EP3 compete for channel 1 and EP2/EP4 for
  channel 0, so concurrent transfers fall back to PIO. Full PIO + DMA again.
* **pmca-console:** `info` = MSC ext-cmd or MTP ext-cmd in the current mode; `install` switches to the app-install
  MTP device (re-enumeration, §4.6); `firmware`/`updatershell` switch to the updater MSC device (PID 0x033A/0x03E2),
  which reboots the camera into updater mode (RE_USB §2.4). This needs the emulator's updater boot path, which is outside
  this document. (Flow per the Sony-PMCA-RE sources, used as a cross-check only.)

---------------------------------------------------------------------------------------------------------------------

## 8. Open questions

1. Reset values of DEVC (bit5, bit16) and EPCTRL. These are inferred from driver writes and polls. Verify on a real
   camera with `devmem` reads only (0xF0210200/0204, 0xF0218000) with and without a cable, or accept them as **H** until
   enumeration works in the emulator.
2. INTS bits 1/2 (which one is the device-event summary) and bit3. The model in §2.2 works either way.
3. Exact hardware semantics of the READY/EMPTY latches with double buffering. §3 gives rules that keep the firmware away
   from its abort paths. They need confirmation by an emulator run that reaches bulk traffic.
4. HDC DMACk bits 2, 4, 5 and the [26:16] field; DMASk bit0. The model in §4.4/4.5 sets values the firmware accepts on
   all its branches.
5. EPCMD bit 7 and constant bit 11. EPCTRL status 28/29/30 (bit 29 has its IE enabled by default but needs no action).
6. HDMAC: BT/TC unit semantics beyond the two encodings this driver uses. IS = 0x0E on both channels.
7. Whether the hardware latches FADDR after the status stage (MODE bit3). Irrelevant to a virtual host.
8. Userland stall before 0xE305 (RE_USB §4.2/§6.1) blocks any of this from being exercised. Resolve it first, or force
   `start_gadget` from gdb to test the model.

---------------------------------------------------------------------------------------------------------------------

## 9. Implementation status

Model: `hw/usb/cxd90014_usbhdc.c` (QEMU type `cxd90014-usbhdc`), one device with three MMIO regions: hdc 0xF0210000
(0x20000, GIC 227), HDMAC 0xF0204000 (0x1000, GIC 228/229 wired but never raised) and the OTG glue 0xF2920000 (0x1000,
+0x10 VBUS/ID, GIC 232/233). It replaces the `otg` and `usb-hdc` register files of `sony_a5000.c`. The A5KU server of
`docs/RE_USB_HOST_PATH.md` §5 lives in the same file. Trace events: `cxd_usbhdc_*`.

| Spec item | Status | Notes |
|---|---|---|
| CONF bit2 soft reset (§2.1, §2.11) | done | self-clearing; resets DEVC = 0x00010020, DEVS events, INTS, INTEN, FADDR, TESTC, hdc+0x18, DMA and EP state |
| INTEN/INTS (§2.2) | done | bits 3/4/8/9 latched W0C; bit1 = OR(DEVS & DEVC)[31:24]; bits 16+n = OR(EPCTRL status & IE); IRQ 227 level over bits 1-4, 8, 9, 16-27 |
| DEVC/DEVS (§2.3) | done | DEVS bit0 = 1, bit17 = DEVC bit0 (FS only when forced), events 24-31 W0C. Events are only raised when their DEVC enable is set |
| FADDR, TSTAMP, TESTC, OTG* 0x3xx | done | store; TSTAMP = virtual ms & 0x7FF |
| HDC DMA registers (§2.5) | done | DMATC/DMAS cleared when DMACk bit0 goes 0 → 1; DMAS = 3 on a short OUT packet or ZLP |
| EPCMD (§2.6) | done | fields applied on every write, strobes one-shot, reads 0 (never busy). Strobe order: status clear, STOP, INIT, START, STALL set/clear, BUFWR, BUFRD. Bit 7 logged as LOG_UNIMP |
| EPCTRL (§2.7) | done | live bits 6-11 computed from the buffer state; a write restores the reset state (0x500 / 0x400) |
| EPCONF / EPCOUNT (§2.8) | done | EPCOUNT lo stored, hi owned by the model |
| Buffer RAM, DMA ports (§2.9) | done | RAM any width; DMA ports are never accessed (option (b) of §5: the hdc copies with the HDMAC addresses). CPU accesses to them are logged |
| Endpoint engine (§3) | done | one packet in flight per EP (double buffering not pipelined). OUT packets only when the EP is armed (DMA running for it, or IE18 for PIO). IN EPn: status26 per packet taken, status27 when all buffers are empty, and when IE19 is enabled while already empty |
| Plug-in / pull-up (§4.1) | done | attach = VBUS && MODE bit1 && DEVC bit16 = 0 |
| Bus reset (§4.2) | done | DEVS29 → wait for W0C → DEVS28 (bit17 = 0, HS) → wait for W0C |
| Control transfers (§4.3) | done | SETUP clears the EP0 stall and is written to the OUT buffer even if it is full; the pre-armed status ZLP is taken only after the OUT data stage; the IN status ZLP is injected after the IN data; the reply waits until the firmware has cleared the status-stage EP0 status (or its IE) |
| Bulk OUT/IN PIO and DMA (§4.4, §4.5) | done | IN DMA: packets of mps pulled from DMACSA + DMATC; a trailing ZLP when DMACk bit3 and the last packet was full; INTS bit(8+k) after the last packet |
| Disconnect / re-enumeration (§4.6) | done | VBUS off, DEVC bit16 0 → 1 or a soft reset = detach |
| HDMAC (§5) | done (b) | registers stored; EB cleared and SS = 5 at the end of a transfer |
| VBUS glue (§6) | done | +0x10 = 2 with a client connected, 3 without; GIC 232 pulse on every change; hdc+0x104 = 0 (SDP) |

Deviations from this spec, all **H**:
* EPCONF is kept across the CONF bit2 soft reset (§2.11 lists it as 0). `transition_to_idle` performs a soft reset after
  the probe wrote EPCONF0 = 0x80040, and the model must not end up with EP0 buffers at hdc+0x8000.
* A SETUP also empties the EP0 IN buffer, so a status ZLP left over after a stalled or cancelled transfer cannot be sent
  as the data of the next one.
* An OUT packet larger than the rest of a DMA chunk: the excess is dropped and logged (never observed).

Verified in the emulator (2026-11-15, firmware 1.10, pristine NAND clone, no SD card):
* No client: boot, shooting screen, MENU, PLAY and MENU from PLAY give screendumps identical to the previous binary
  (`158bf21`).
* Client attached: VBUS → `ModelUsbMtp` → gadget start → pull-up within about 1 s of a running camera; bus reset;
  GET_DESCRIPTOR returns the firmware's descriptors: device `12 01 00 02 00 00 00 40 4c 05 d3 08 00 01 01 02 03 01`
  (**054C:08D3**, bcdDevice 1.00, strings "Sony" / "ILCE-5000" / serial), configuration 39 bytes: interface 0 class 6/1/1,
  EP 0x81 bulk 512, EP 0x02 bulk 512, EP 0x83 interrupt 32. The PID differs from the 0x034A of `pictbridge_desc_tbl`
  (RE_USB_HOST_PATH §1.2). It is what the unmodified firmware reports, so `DescriptorMaintainer::Overwrite` very likely
  patches it from Backup preset data (RE_USB_HOST_PATH open question 2).
* MTP traffic used both DMA channels (bulk OUT EP2 on channel 0 with 32768-byte chunks ended by short packets, bulk IN
  EP1 on channel 1 with chunks up to 8192 bytes, OUT transfers up to 65548 bytes) and PIO.
* Detach: the camera leaves the USB screen and MENU works. Five plug/unplug cycles in a row re-enumerated each time.
* Not exercised: STALL on bulk endpoints (MSC), FS operation, EP3 interrupt IN, SENSER.

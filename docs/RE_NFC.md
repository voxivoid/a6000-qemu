# RE: NFC ("One-touch") — chip, firmware path, and emulator model spec

Scope: what the a5000 (firmware 1.10) NFC hardware is, how the stock firmware drives it, what a phone reads from it,
what the emulator does today, and a spec for a model plus a host-side "tap". RE only; no code changed.

Evidence base: `nfc.ko` (Hex-Rays output in `re/ida/kmod_i2cgrp/nfc.c` + objdump of the module), `libInfraNfc.so`
(Hex-Rays output in `re/ida/ui/ui_libInfraNfc.so.c` + Thumb objdump), `libObj.so` (`ObjHandover`, `M_Handover`),
`libIMDB` module table, kernel `gpiopin_to_irq`, Backup props decoded from the owner's `Backup.bin`
(`re/backup/whitelist/backup_props_all.tsv`), av-cam SIO driver notes (`docs/RE_AVCAM_IRQ_BOOT.md` §4), and emulator
runs (section 3). Function addresses: `nfc.ko` = section offsets (.text at 0), `libInfraNfc.so` = file VAs.
`[?]` = not proven. Wi-Fi credentials found in the owner's Backup are **not** reproduced here (only their location).

---

## 0. Summary

* **Chip: a Sony FeliCa Plug–class "NFC Dynamic Tag" (NFC Forum Type 3 / FeliCa) [? part number not in the firmware;
  RC-S801/802 family behaviour].** The pin set the driver uses — power switch (SW), select (SEL), a 3-wire half-duplex
  serial link at 1 MHz, a data-ready IRQ and an RF-detect (RFDET) line — is exactly the FeliCa Plug host interface
  (SW, SEL, DATA, SPICLK, IRQ, RFDET; max 1 Mbps, "set parameter" with a 2-byte Data Format Code + 32-bit user
  parameter) [Sony RC-S801/802 brief; Switch Science FeliCa Plug notes].
* **Bus:** SIO **channel 2** (0xF2405400, GIC 203), driven by **av-cam's SIO server** (OSAL queue 0x740017) on behalf
  of `nfc.ko`; `nfc.ko` itself touches no SIO MMIO. GPIOs: **SW = P7.10**, **SEL = P7.14**, **IRQ = P0.4 → GIC 125**,
  **RFDET = P0.8 → GIC 129** (active low: 0 = field present).
* **Dynamic tag, host-answered.** The camera does not program static tag memory. Each reader command (FeliCa Read /
  Write Without Encryption) is delivered to the camera (IRQ → 230-byte read), `libInfraNfc` (in `im.elf`) builds the
  answer and sends it back. The answered "memory" is a read-only Type 3 NDEF image: Attribute Information Block +
  NDEF message `[MIME application/x-sony-pmm {SSID, passphrase}] [AAR com.sony.playmemories.mobile]`.
* **Tap flow:** RFDET falling edge → chip powered on + set-parameter → phone reads all NDEF blocks → InfraNfc
  notifies 0x104 "handover" → `ObjHandover` → model event `0x3A4D2001` "StartCommunicationEvt" → the UI runs the
  One-touch (NFC) app / smartphone connection (Wi-Fi Direct AP with the tag's SSID/passphrase) [? last step].
* **Emulator today:** `nfc.ko` probes and InfraNfc turns the driver on, but the GPIO model idles **P0.8 low**, so the
  firmware believes a phone is permanently on the antenna (one RFDET IRQ at boot, chip init frames on SIO2, a 60 s
  keep-alive alarm forever). A pulse on P0.4 causes an **IRQ storm** (49 IRQs, 49×230-byte reads for one edge)
  because GIC 125 is level-sensitive and the line is held 1 ms. Nothing answers on SIO2.

---

## 1. Hardware and bus (Q1)

### 1.1 Pin/parameter table (Backup subsystem 0xF9, board config; owner's values)
`nfc.ko` reads every pin through `Backup_read_4Byte(id)` pairs (port, bit).

| Backup id | value | use in nfc.ko | meaning |
|---|---|---|---|
| 0xF904D2 | 2 | `sub_0` +0 of the SIO request | **SIO channel 2** |
| 0xF904D3 | 1 | — (not read by nfc.ko) | [?] |
| 0xF904D4/D5 | 5 / 11 | — | P5.11 [? unused by nfc.ko] |
| 0xF904D6/D7 | 7 / 14 | `sub_0` +2/+3 (normal set), `sub_394` after transfers | **SEL = P7.14** (also passed to av-cam as the request's CS pin) |
| 0xF904D8/D9 | 7 / 10 | `nfc_rsrc_dev_power_off` (0), `nfc_rsrc_dev_init_data` (1) | **SW (chip power) = P7.10**, 1 = on |
| 0xF904DA/DB | 0 / 4 | `nfc_rsrc_request_irq` "nfc_rsrc_irq" | **IRQ = ext-int group 0 pin 4 → GIC 125** (= P0.4) |
| 0xF904DC/DD | 0 / 8 | "nfc_rsrc_rfdet" IRQ, `nfc_rsrc_get_approach` (`gpio_get_data_bit(0, 8)`) | **RFDET = P0.8 → GIC 129**, level read as "approach" |
| 0xF9054B/4C | — | alternate SEL when Backup 0x1820000 bit4 = 1 | not used (0x1820000 = 0x01) |

`gpiopin_to_irq(group, pin)` (vmlinux 0x4004FB58): group 0 pin 0–15 → 121+pin, pin 16 → 144; group 1 pin 0–6 →
137+pin. Group 0 pin n is GPIO P0.n (cf. P0.7 → 128 for the sub-micro), so P0.4 → **125**, P0.8 → **129**.

`nfc_rsrc_set_irq_rfdet` (0xD7C) writes the GIC distributor directly: `0xF8001200 + 4·(129>>5) = 1<<(129&31)`
(**ISPENDR**: software-raises the RFDET IRQ). `nfc_rsrc_enable_irq` (0xDF4) / `nfc_rsrc_request_irq` (0xF98) first
write `0xF8001280 + …` (**ICPENDR**, clear stale pending) for 125 and 129.

### 1.2 SIO transfers (av-cam SIO server, queue 0x740017)
`sub_0` (0x0) fills the 64-byte request (layout per `RE_AVCAM_IRQ_BOOT.md` §4):

| off | value | note |
|---|---|---|
| +0 | 2 (0xF904D2) | channel |
| +2/+3 | 7/14 (0xF904D6/D7) | CS/SEL pin |
| +4 | number of segments (1 or 2) | |
| +16, +20, +48 | 1 | |
| +40 | 2 | SPI mode word (alt set: 0) |
| +44 | **6 = transmit, 5 = receive** | (alt set: 1, or 2 for TX) |
| +52 | **1 000 000** | 1 MHz clock (FeliCa Plug link max 1 Mbps) |
| +56 | → segment list, 16 B each `{u32 tx_ptr, u32 rx_ptr, u32 0, u32 len}` | |
| +60 | `sio_comm_cb` | completion → wakes the caller; 3000-jiffy timeout (−110 on expiry) |

Observed register programming in the emulator (run 3/4, av-cam driver → SIO2):

| transfer | +0x04 | +0x0C (len−1)<<24 | +0x08 | FIFO |
|---|---|---|---|---|
| TX (mode 6) | **0x20000000** (bit 29) | 0x08000000 (9 B) | 0 | `1B FF FF 00 00 00 00 00 00` |
| RX (mode 5) | **0x10000000** (bit 28) | 0x7F000000 then 0x65000000 (128 + 102 B) | 0 | — |

So **+0x04 bit29 = transmit, bit28 = receive** on this channel [H, from these two modes only]. A 230-byte read is two
SIO transfers (128, then 102).

### 1.3 Who uses what
* Kernel: `nfc.ko` (UDIF "nfc_rsrc", ver 01.00.04, source `Nfc-RsrcCommon_132H`), loaded by `/sbin/init`. Real camera
  log: `UDIF: suspend skipped: nfc_rsrc`.
* av-cam (LIRO RTOS): SIO server channel task for ch2.
* Userland: `libInfraNfc.so` (IMDB entry 79, `me=0x181`, `infra_nfc_init/sus/res`, inside `im.elf`); `libObj.so`
  `ObjHandover` + model `M_Handover` (`modelHandover.so`), `LkmNFC` (per-UI-layer NFC enable lock), view icons
  `ACT_IconNFC*`; menu item "One-touch(NFC)" (`OneTouchNFC`, CautionConfig `cmnViewSettingNodeRootOneTouchNFC`).
* Android `libnfc_ndef.so` (nflasha16) is the stock AOSP/NXP NDEF helper, unrelated to this path.

---

## 2. Protocol (Q2)

### 2.1 OSAL queues
| queue | owner | direction / content |
|---|---|---|
| 0x18204BC | nfc.ko event thread `nfc_rsrc_event_thread` (0x15B0) | 248-byte msgs `{u32 code, u32 sender, u32, u32 len, u8 data[]}`. From userland: **1 start, 2 stop, 3 send data (len ≤ 0xE6)**. From ISRs/timer: **128 RFDET, 129 IRQ, 130 alarm**, 131 thread exit |
| 0x18104BB | InfraNfc (main thread + kernel input) | from nfc.ko: code **4 = received packet** (len 230, data at +16). Internal commands 0x201..0x209 |
| dynamic (`osal_dynamic_id(0x181)`) | InfraNfc sync API `sync_callback_api` (0x3D08) | op 1/2 register/unregister notify queue, 3 force on (→0x207), 4 force off (→0x208), 5 get boot-handover state |
| registered notify queues | e.g. ObjHandover | **0x101 state on, 0x102 state off, 0x103 hw error, 0x104 handover done** |
| 0x910040/41/42 | utimer.ko | alarm / delay / µs counter |

### 2.2 nfc.ko state machine (`nfc_rsrc_event_thread`)
`get/set_nfc_state`: 0 = stopped, 1 = armed (chip off, waiting RFDET), 2 = chip on / idle, 3 = packet delivered,
awaiting reply.

| event | condition | action |
|---|---|---|
| 1 start | state 0 | enable IRQs 125/129 (ICPENDR clear + enable), state ← 1, read RFDET level; **if 0 (field present) software-pend IRQ 129** |
| 128 RFDET (ISR `nfc_rsrc_rfdet` 0xCCC also `disable_irq_nosync(129)`) | state 1 | arm alarm (Backup **0x1820004 = 60 s**, ≤ 0x10C6), `nfc_rsrc_dev_init_data`, state ← 2 |
| 129 IRQ (ISR `nfc_rsrc_irq` 0xBF8) | state > 1 | re-arm alarm, `nfc_rsrc_dev_recv_data` (230 B), post code 4 to 0x18104BB, state ← 3 |
| 3 send | state 3 | `nfc_rsrc_dev_send_data(len, data)`, state ← 2 |
| 130 alarm | state > 1 | read RFDET: **0 → re-arm alarm (phone still there); 1 → SW off, state ← 1, re-enable IRQs** |
| 2 stop | state ≠ 0 | disable IRQs, cancel alarm, SW off, state ← 0 |

`nfc_rsrc_dev_init_data` (0x688): SEL ← 0; **first time only** (`g_nfc_first_comm_flag`) send set-parameter while
still unpowered; delay(1); **SW ← 1**; delay(50); send set-parameter; delay(1); SEL ← 1.
`nfc_rsrc_dev_send_data` (0x4D4): TX (1 or 2 segments of ≤128 B), delay(1), SEL ← 1.
`nfc_rsrc_dev_recv_data` (0x5A0): RX 128 + 102 B, delay(2), SEL ← 0, delay(1).
Delay units are those of utimer 0x910041 (ms [?]).
SEL thus reads as a link-direction/handshake line: 1 after the host has sent (chip may talk), 0 after the host has
read (host will talk) [H].

### 2.3 Host ↔ chip frames (as the firmware sees them)
* **Set parameter** (host → chip, 9 B, at every chip power-on), built by `sub_21C` (0x21C):
  `[(Backup 0x1820005 & 7) << 3 | 3] [Backup 0x1820002, 2 B] [Backup 0x1820001, 2 B] [Backup 0x1820003, 4 B]`.
  Owner's values (0x1820005 = 3, 0x1820002 = FF FF, 0x1820001 = 00 00, 0x1820003 = 0) → **`1B FF FF 00 00 00 00 00 00`**
  (verified on SIO2 in the emulator). Plausibly FeliCa Plug "mode + Data Format Code + user parameter" [?: which 2-byte
  field is the DFC is not proven].
* **Reader command** (chip → host, read as 230 B): parsed by `RecvPacket::RecvPacket` (0x3E3C):
  `[cmd] [n_blocks 1..12] [block list…]`; each block-list element is FeliCa format: first byte bit7 = 1 → 2-byte
  element, block = next byte; bit7 = 0 → 3-byte element, block = next 2 bytes little-endian. IDm and the service list
  are not present — the chip strips/handles them [?]. Trailing bytes ignored.
* **Reply** (host → chip, `nfc` code 3):
  * cmd **0x06** (Read Without Encryption): `[00 00] [16 B × n]`, len = 2 + 16n.
  * cmd **0x08** (Write Without Encryption): `[01 A5]` (status flags = error; the tag is read-only).
  * anything else, or invalid packet: no reply (logged).
  The chip wraps the reply into the FeliCa response frame [?].

### 2.4 Tag content (`InfraNfc::create_ndef_data` 0x413C, `read_block` 0x4234)
Built lazily on the first read, and **rebuilt whenever a command requests block 0**.

SSID/passphrase source (`read_ssid_passphrase` 0x3EC0), first match wins:
1. Backup 0x13E0010 == 1 and 0x13E000E == 1 → SSID 0x13E000F (33 B), pass 0x13E000D (9 B);
2. else 0x13E0015 == 1 → SSID 0x13E0018, pass 0x13E0016;
3. else 0x13E0007 == 1 → 200-byte record 0x13E0006: **SSID at +0 (33 B), passphrase at +97 (9 B)** (the owner's camera
   uses this one: 0x13E0007 = 1, 0x13E0010/0x13E0015 = 0).
The SSID is the camera's Wi-Fi Direct name (`DIRECT-xxxx:<model>` per `RE_WIFI.md`); the passphrase is 8 characters.

NDEF message (Ls = SSID length ≤ 32; total Ln = 92 + Ls bytes, must be ≤ 255):
```
Record 1  92 16 <PL=21+Ls> "application/x-sony-pmm"
          payload: 00 01 00 01 10 | 10 00 00 <Ls> <SSID> | 10 01 00 08 <8-byte passphrase>
Record 2  54 0F 1C "android.com:pkg" "com.sony.playmemories.mobile"     (Android Application Record, 46 B)
```
(0x92 = MB|SR|TNF 2 MIME; 0x54 = ME|SR|TNF 4 external.) The meaning of the leading `00 01 00 01 10` is not decoded
[?]; `10 00`/`10 01` act as SSID / passphrase tags with 16-bit big-endian lengths.

Type 3 Attribute Information Block (block 0):
```
10 Nbr Nbw 00 Nmaxb 00 00 00 00 00(WriteF) 00(RWFlag = read-only) 00 00 Ln  cks_hi cks_lo
Nmaxb = ceil(Ln/16); Nbr = Nbw = min(Nmaxb, 12); checksum = sum of bytes 0..13
```
Blocks 1..16 = 16-byte slices of the 256-byte NDEF buffer; higher blocks read as zeros.

### 2.5 When the tag is live, and what a tap does
* InfraNfc (`state_machine_main` 0x455C, entry 0x44EC) states: 0 init, 1 suspended, **2 off**, **3 on** (entry runs
  `action_start_driver` → nfc code 1, then notify 0x101), 4 off by airplane mode, 5 hardware error, 6 exit.
  At init (0x201) it goes to 2 (off) unless auto-start; boot mode 0x51000F == 5 disables auto-start. It is switched to
  **3 only by a force-on (0x207) and only if airplane mode is off**; force-off 0x208 → 2 (exit action =
  `action_stop_driver` → code 2). Force on/off come from the UI via `ObjHandover`/`LkmNFC` (NFC enabled only on
  screens whose layer allows it — in practice the shooting/playback screens with the N-mark icon [?]).
  Received packets are processed only in state 3.
* When every NDEF block 1..Nmaxb has been read at least once since the last block-0 read: `action_notify_handover`
  → notify **0x104** → `ObjHandover` (libObj 0x2D9388) → MWF message category **0x3A4D id 0x2001** →
  `M_Handover` "Gen StartCommunicationEvt" (libObj 0x48C9FC) → model manager. InfraNfc then sleeps Backup
  **0x1810001 × 10 ms** (owner: 10 → 100 ms). The UI then starts the function assigned in **One-touch(NFC)** (the
  smartphone connection / Smart Remote app) which brings up the Wi-Fi Direct group owner with that SSID/passphrase
  [? view-side dispatch of 0x3A4D2001 not traced].
* 0x101/0x102/0x103 → 0x3A4D/0x2000 with param 0x1001 = 1/0/2 (state on/off/hw error) for the N-mark icon [?].
* A handover-wait timer (`action_handover_timeout` 0x36A8, "ho tmo poff") exists for the "boot by NFC" case
  (sync API op 5) [? not traced].

---

## 3. Emulator today (Q3)

Runs with the unmodified firmware (`-trace cxd_sio_*`, guest shell, `gpio-poke`):

1. `nfc` module loaded; `/proc/interrupts` has `125 nfc_rsrc_irq` and `129 nfc_rsrc_rfdet`. No probe error.
2. After the UI comes up, **IRQ 129 count = 1** and SIO2 shows **two 9-byte TX frames `1B FF FF 00 00 00 00 00 00`**
   (first-comm frame + post-power-on frame). Cause: the GPIO model resets P0.8 to 0, so InfraNfc's start (code 1)
   reads "field present" and software-pends IRQ 129. This also proves InfraNfc reached state 3 (driver started) in
   the emulator.
3. With `gpio-poke 0.8=1` the 60 s alarm takes the chip-off path (no SIO traffic); `gpio-poke 0.8=0` then raises
   IRQ 129 (count 2) and **one** more 9-byte frame (first-comm flag already set) — the RFDET path works end to end.
4. `gpio-poke 0.4=1` (one edge on the IRQ pin): **IRQ 125 fired 49 times**, 49 × (128 + 102)-byte RX transfers.
   The GPIO model holds the GIC input high for 1 ms and GIC 125 is level-triggered (the board stub only makes
   98/99/101/102 edge-triggered); the ISR does not touch any device, so it re-enters until the line drops.
   RX data were the stale FIFO bytes (`1B FF …` → cmd 0x1B, n_blocks 0xFF), which InfraNfc rejects, so no reply.
5. No `-d unimp` accesses come from the NFC path (no NFC MMIO exists).

---

## 4. Emulation spec (Q4)

### 4.1 Device: board-level `a5k-nfc` (in `hw/arm/sony_a5000.c` or `hw/misc/a5k_nfc.c`)
Not an MMIO device: a SIO slave plus GPIO lines, like the sub-micro model.

| connection | how | behaviour |
|---|---|---|
| SIO2 | `cxd90014_sio_attach(sio, 2, a5k_nfc_xfer, s)` | full transfer hook (below) |
| SW P7.10 | `qdev_connect_gpio_out_named(gpio, "out", 7*32+10, …)` | 0 → chip unpowered: drop pending command, ignore TX; 1 → powered |
| SEL P7.14 | `"out"` 7*32+14 | traced; direction hint only (not required for correctness) |
| RFDET P0.8 | `qdev_get_gpio_in(gpio, 0*32+8)` | **idle 1**; 0 while the virtual phone's field is on. Works while SW = 0 (RF-powered) |
| IRQ P0.4 | `qdev_get_gpio_in(gpio, 0*32+4)` or a direct pulse | **exactly one GIC 125 assertion per reader command** (see 4.3) |

Reset: RFDET = 1, IRQ idle, no field, no pending command. (Fixes the "phone always present" state at boot; matches
real-camera behaviour where nothing happens on SIO2 until a tap — first-comm frame on the first tap only.)

### 4.2 SIO transfer hook
The current `CXDSioSlaveXfer` gets `(buf, len)` only. Direction is needed (TX and RX have the same length range and the
FIFO keeps stale bytes). Options:
* **Preferred:** extend the SIO model to pass the +0x04 word (bit29 TX, bit28 RX — section 1.2) to the slave
  (`CXDSioSlaveXfer2(opaque, ch, mode, buf, len)`), keeping the old callback for ch0/ch3.
* Fallback: infer from state (a read is expected only after the model raised IRQ and before the reply).

Model logic:
* **TX while powered**:
  * 9 bytes with no command pending → set-parameter: store `{mode byte, 2 B, 2 B, 4 B}`, log it. (A 9-byte TX at
    SW = 0 is the first-comm frame: ignore.)
  * otherwise, while a command is outstanding → reply. Concatenate segments (≤128 B each) until the reply is complete
    (Read: 2 + 16·n with n from the outstanding command; Write: 2). Hand it to the host interface, clear outstanding.
* **RX**: serve the 230-byte command buffer sequentially (first transfer bytes 0..127, second 128..229); zero-fill after
  the packet. Reset the read offset when a new command is latched.
* Timeouts: the guest waits ≤ 3000 jiffies per SIO transfer (immediate in the model) and has no reply deadline; the
  host side should wait up to ~2 s virtual time for a reply and report a timeout otherwise.

### 4.3 IRQ delivery
One reader command must cause one ISR run (the ISR posts code 129; a second run while state is 3 triggers a second
230-byte read). With the current GPIO model a pin edge holds a level-sensitive GIC input for 1 ms → storm (section 3).
Choose one:
* make GIC 125 (and 129) **edge-triggered** in the board stub's ICFGR programming (ICFGR7: IRQ 125 = bits 27:26 → set
  bit 27; IRQ 129 = ICFGR8 bits 3:2 → set bit 3) — the real bootloader/kernel value is not recoverable (the snapshot
  saves only ISENABLER) [H]; this probably applies to all GPIO pin IRQs 121–144 [?];
* or drive GIC 125 directly with a short pulse from the NFC model instead of through the P0.4 pin.
Also drive the P0.4 level (assert = low [? polarity unknown], release after the RX read) for fidelity of any pin read.

### 4.4 Host interface ("virtual phone")
Machine option `nfc=<chardev-id>` (e.g. `-chardev socket,id=nfc,host=127.0.0.1,port=46514,server=on,wait=off
-M sony-a5000,nfc=nfc`), line-based text protocol:

| host → model | effect / reply |
|---|---|
| `field on` / `field off` | RFDET 0 / 1. Reply `ok` |
| `cmd <hex>` | latch raw packet `[cmd][n][block list]`, raise IRQ; reply `resp <hex>` or `timeout` |
| `read` | phone emulation: `cmd 06 01 80 00` (block 0), validate AIB checksum, then read blocks 1..Nmaxb in commands of ≤ Nbr blocks; reply `aib <hex>`, `ndef <hex>`, then decoded records (`mime application/x-sony-pmm ssid=<..> pass=<..>`, `aar <pkg>`) |
| `tap [ms]` | `field on`, wait for the set-parameter frame (or 200 ms), `read`, hold `ms` (default 500), `field off` |
| `status` | power, SEL, last set-parameter bytes, state |
| model → host (async) | `ev power on/off`, `ev setparam <hex>` |

Optional HMP/QOM convenience: `qom-set /machine nfc-tap 1`. Trace events: `a5k_nfc_power`, `a5k_nfc_setparam`,
`a5k_nfc_cmd`, `a5k_nfc_resp`, `a5k_nfc_rfdet`.

### 4.5 Acceptance tests
1. **Clean probe**: boot to the shooting screen; `lsmod` shows `nfc`; `/proc/interrupts` 125/129 present with
   **0 counts** and no SIO2 transfers (RFDET idle high).
2. **Field detect**: `field on` → IRQ 129 +1; model logs first-comm frame (ignored, SW = 0), SW P7.10 → 1, then
   `setparam 1BFFFF0000000000 00` (owner's Backup); SEL ends 1.
3. **Read**: `read` returns a valid AIB (version 0x10, RWFlag 0, checksum OK, Ln = 92 + Ls) and an NDEF message with
   the two records of 2.4; SSID/passphrase equal Backup 0x13E0006 (+0 / +97) of the NAND image. Exactly one IRQ 125
   per command; each command = one 128 + 102 B RX pair and one TX reply.
4. **Write rejected**: `cmd 08 01 80 01 <16 B>` → `resp 01A5`.
5. **Handover**: after the last NDEF block is read, the UI leaves the shooting screen for the One-touch function
   (screendump); guest log/`klog` shows InfraNfc notify 0x104 [? exact screen]. With the Wi-Fi model of
   `RE_WIFI.md` (ath6kl SDIO on mmc1, P2P GO `DIRECT-…`, 10.0.0.1) the group owner comes up with the same SSID and
   passphrase and a `wifi_client_join <ssid> <pass>` from the host completes the connection. Without it the flow
   stops at the Wi-Fi start (error/abort screen expected [?]).
6. **Release**: `field off` → within 60 s (Backup 0x1820004) SW P7.10 → 0 and IRQs re-armed; a second `tap` works and
   sends only the post-power-on set-parameter frame.
7. **Gating**: with Airplane Mode on, or in the MENU, `tap` produces RFDET handling in the driver only if InfraNfc is
   on; no reply/handover when InfraNfc is off (driver stopped → no IRQs enabled).

### 4.6 Dependencies and open points
* Wi-Fi Direct start after handover depends on the Wi-Fi model (`docs/RE_WIFI.md`).
* View-side dispatch of 0x3A4D2001 and which One-touch app runs (`OneTouchNFC` setting) [?].
* Chip identity/part number, set-parameter field meanings, exact FeliCa framing the chip adds, IRQ/RFDET electrical
  polarity of IRQ [?], delay units of 0x910041 [?], GIC trigger type of 125/129 [H].
* `nfc.ko` alt SIO parameter set (Backup 0x1820000 bit4) unused on this camera.

---

## 5. Notes for the project's own NFC feature (Q5)

* The NDEF content is generated by stock `libInfraNfc` inside `im.elf`; `nfc.ko` posts every reader command to the
  **fixed** queue 0x18104BB that InfraNfc owns, so an outside process cannot answer reader commands without replacing
  InfraNfc (= `/usr` change, brick-risk tier — not an option before the replacement phase).
* What our UI can do safely with the stock path: switch NFC on/off and subscribe to state/handover through InfraNfc's
  sync API (ops 1–4, like `ObjHandover`/`LkmNFC`), show the N-mark state (0x101/0x102/0x103), and on **0x104
  handover** start our own Wi-Fi Direct/remote service. The tag then always advertises the camera's Wi-Fi Direct
  SSID/passphrase (Backup 0x13E record) plus the PlayMemories AAR — compatible with Sony's phone apps; a phone
  without that app still gets the Play Store redirect.
* Changing what the tag says (e.g. a generic Wi-Fi WSC record or a URL to our own app) requires either new Backup
  values (only the SSID/passphrase fields, via the menu's SSID/PW reset — the same values the Wi-Fi AP uses) or a
  replacement InfraNfc. Limits for any custom content: read-only Type 3 image, ≤ 255 bytes NDEF, ≤ 12 blocks per
  read command.

---

## Sources (external)
* Sony RC-S801/802 (FeliCa Plug / NFC Dynamic Tag) product brief — SW and RF-detect signals, 3-wire half-duplex host
  link up to 1 Mbps: https://sony.net/Products/felica/business/data/RC-S801_S802_E.pdf
* Switch Science FeliCa Plug notes (Data Format Code + 32-bit user parameter in setParam):
  https://trac.switch-science.com/wiki/FeliCaPlug-en
* Sony FeliCa Plug Arduino sample (pins SW, SEL, DATA, SPICLK, IRQ, RFDET; `setParam(MODE_TYPE3, dfc, userParam)`):
  https://gist.github.com/mugifly/8689329

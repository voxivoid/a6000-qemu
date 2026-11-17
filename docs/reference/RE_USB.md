# USB (device side): controller, connection sequence, emulator behaviour and modelling plan

Scope: the a5000 multi/micro-USB port as a **USB device**: mass storage, MTP/PTP, PC remote, the updater link,
and VBUS/charger detection. This document extends `docs/RE_STORAGE_USB.md` §6 and does not repeat it.
Firmware 1.10, unmodified.

Evidence comes from five sources:
* Hex-Rays output: kernel `emulator/re/ida_stor/stor_vmlinux_usb.c`, modules `stor_usb*.c`, userland
  `re/ida/ui/ui_libusb.so.c`, `ui_libUsbControl.so.c`, `ui_libUsbBusPower.so.c`, `re/ida/upm.ko.c`.
* `objdump` of `emulator/re/vmlinux.elf`.
* Strings and data tables of `/usr/lib/*.so`.
* The WBI snapshot (`tools/wbi.py`).
* Three dynamic runs of the frozen QEMU binary with `-d unimp`, the `a5ktrace` plugin and gdb (§4).

Claims I could not prove are marked **[?]**. Addresses are kernel VAs unless noted. Peripheral VAs are identity-mapped:
the snapshot page walk gives VA 0xF0210000 → PA 0xF0210000, and the same for 0xF2920000 and 0xF0202000.

---------------------------------------------------------------------------------------------------------------------

## 0. Summary

| Item | Result |
|---|---|
| Device controller | **Fujitsu/Socionext "F_USB20HDC"** USB 2.0 HS dual-role device controller, kernel built-in (`f_usb20hdc_udc`, boot log "F_USB20HDC UDC driver (version 1.0.5)"). **Not Synopsys DWC2.** At 0xF0210000 (0x20000), GIC **227**. 1 control endpoint and 11 configurable endpoints (`ep1-bulk` … `ep11-bulk`). |
| DMA | **Fujitsu "HDMAC"** at 0xF0204000. Two channels are used, IRQ **228/229**. The per-channel registers are at +0x10/+0x14/+0x18/+0x1C + 0x10·ch, which is the same layout as the upstream Linux `milbeaut-hdmac` driver (DMACA/DMACB/DMACSA/DMACDA). |
| Host side | EHCI/OHCI at 0xF0200000 (`ho`) plus `h2xbb` bridge 0xF0203000. These are used only for the camera-as-host path (`libusb` CUsbHost, `usbcore/ehci-hcd` insmod on demand). Not needed for device mode. |
| PHY | No separate PHY driver. The PHY tuning word is at **0xF020200C** (RMW of 3-bit fields from otgcore config bytes +0x4C/+0x4D, §1.3). A second tuning word is at hdc+0x18. Line state (D+/D-) is read at **hdc+0x104** (§1.4; this corrects RE_STORAGE_USB §6.4, which says DEVS 0x204). |
| VBUS / ID detect | **SoC OTG glue 0xF2920000 +0x10**: bit0 = VBUS (active-low), bit1 = ID. GIC 232/233. On this camera otgcore config byte +44 = **1** (snapshot), so the alternative "VBUS/ID on GPIO pins" path in `transition_to_idle` (byte == 2 → `gpiopin_to_irq`) is **not** used. No charger/detector IC: `libUsbBusPower` can load `usb_max14646/bd91407gw/max8934/smb232/…`.ko, but none of those files exist on this firmware. |
| Charger type | `usb_portMonitor` samples line state (hdc+0x104 bit5/6) after VBUS. DP = DM = 1 for >15 of 20 samples means DCP (type 3), otherwise SDP (type 4). |
| Gadget functions (VID 0x054C) | MSC BOT `usbg_storage` (PID 0x047D) · PTP `usbg_stillimage` (PictBridge 0x034A, Auto 0x02D7, PC-Remote 0x0641) · vendor bulk `usbg_sen` (0x0336, 4 bulk EPs, Sony "SENSER") · HID dummy `usbg_buspower` (0x04EF, 500 mA) · updater 0x03E2, app-download 0x06A8 (layouts not decoded). Descriptors live in `libusb.so` `.data` (`*_desc_tbl`), **not** in the kernel modules. |
| Emulator today (no cable) | Boot is clean. The probe and `transition_to_idle` run once per kernel stage, userland's otgcore ioctl 0xE304 runs `fusb_start_control`, and the state ends at **2** (control started, VBUS absent). **Nothing polls afterwards.** No change is needed for "unplugged". |
| Emulator with VBUS forced present | The stock UI switches to **"USB mode – connecting to MTP…"** (`viewUsbMTP.uxc`). Userland never issues the start-gadget ioctl 0xE305 within 120 s, so the UDC is never enabled (§4.3, open). |

---------------------------------------------------------------------------------------------------------------------

## 1. Hardware detail beyond RE_STORAGE_USB §6

### 1.1 Register-access table (`f_usb20hdc_register` @ 0x4029c0e0)
Entries are 8 bytes, `{u32 offset, u8 readable, u8 writable}`. The driver accesses registers only through this table
(`off_401D656C[2n]`). The table was dumped from the snapshot.

| idx | offset | name (from RE_STORAGE_USB) | note |
|---|---|---|---|
| 0–3 | 0x000 / 0x004 / 0x008 / 0x00C | CONF / MODE / INTEN / INTS | |
| 4–19 | 0x040–0x07C | EPCMD0–15 | bit31 = busy, polled `>= 0` ≤ 0xFFFF times |
| 20–23 | 0x200 / 0x204 / 0x208 / 0x20C | DEVC / DEVS / FADDR / TSTAMP | TSTAMP is read-only |
| 24–30 | 0x300 / 0x310–0x324 | OTGC / OTGSTS(ro) / OTGSTSC / FALL / RISE / OTGTC / OTGT | |
| 31–38 | 0x400–0x40C, 0x420–0x42C | DMAC1/DMAS1(ro)/DMATCI1/DMATC1(ro), same for ch2 | HDC side of the DMA handshake |
| 39 | 0x500 | TESTC | |
| 40–55 | 0x8000–0x803C | EPCTRL0–15 | |
| 56–71 | 0x8040–0x807C | EPCONF0–15 | |
| 72–104 | 0x8080–0x8100 | EPCOUNT (2 per EP) | EP0 OUT count = hi-half of entry 73 (0x8084) & 0x7FF |
| 105 / 106 | 0x10000 / 0x18000 | EPBUF (FIFO RAM windows) | |
| 108–111 | 0x10 / 0x14 / 0x18 / 0x1C (+0x10·ch) | **HDMAC** DMACA / DMACB / DMACSA / DMACDA | used with the HDMAC base (`f_usb20hdc_set_dma_controller` @ 0x401ca9d8: DMACA bits 28:24 = request source, 19:16 cleared; DMACB bits 31:30) |

### 1.2 Endpoint layout (`off_401CA234` → 0x4029c468, 16-byte entries)
* EP0: max packet 64, no DMA channel (bytes ff/ff).
* EP1–EP11: FIFO size 0x400, HS max packet 0x400 / FS 0x40 [? field order]. Odd EPs carry DMA channel byte 1 and even EPs channel 0 [?].
  `f_usb20hdc_is_dma_transfer_usable` @ 0x401c9afc refuses DMA when another active EP holds the same channel.
  In that case the driver falls back to PIO through EPBUF (`f_usb20hdc_is_pio_transfer_auto_change_usable`).
* The FIFO layout is computed in `f_usb20hdc_initialize_endpoint_configure` @ 0x401c9c0c, which writes EPCONF0 = buffer
  address >> 2 | maxpacket << 13.

### 1.3 Idle bring-up: `transition_to_idle` @ 0x401db020 (otgcore ioctl 0xE304, state 1 → 2)
The sequence below was observed in the emulator and is the same in every run:
```
otg+0x00 &= ~0x10                      ; 0xF2920000
CONF |= 4 ; poll CONF bit2 clear       ; soft reset (model: autoclear)
MODE |= 2 ; poll DEVS bit0 set         ; model presets DEVS = 1
0xF020200C = (old & 0xFFFFF888) | (cfg[0x4C] & 0x77) | ((cfg[0x4D] << 8) & 0x700)   ; PHY tuning [?]
hdc+0x18   = (old & 0xFFFFF888) | (cfg[0x4E] & 0x77) | ((cfg[0x4F] << 8) & 0x700)   ; second tuning word [?]
```
The `cfg` bytes are at VA 0x403b1af8. In the snapshot the word is 0x00040000, so the writes are 0 and 4, which matches the
emulator log. The firmware sets them with otgcore ioctl `0x4010E313` (libusb `set_phy_parameter`), sourced from Backup.

### 1.4 Line state: `usb_otgcore_get_line_state` @ 0x401da85c
`ldr r3,=0xF0210000; ldr r3,[r3,#0x104]`. Bit5 = D+, bit6 = D-. The offset 0x104 is not in the access table.
The current emulator returns 0 (unimp region inside `usb-hdc`), which `usb_portMonitor` reads as "SDP/data host". That
value is harmless.

### 1.5 VBUS / ID interrupt semantics
* `fusb_interrupt_usb_vbus` @ 0x401d9200 and `_id` @ 0x401d9284 read otg+0x10 and call `fusb_notify_vbus(bit0)` /
  `fusb_notify_cid(bit1)`. Each one only posts an event if the value changed (state fields +0x88 / +0x84 of the fusb struct).
  The ISRs never clear anything, so **GIC 232/233 must be pulses**, as RE_STORAGE_USB already notes.
* The probe sets otg+0x40 |= 0x100, then 0x300, and otg+0x50 likewise (both seen in the log). These are the VBUS/ID
  interrupt enable and edge select [?].
* `fusb_start_control` @ 0x401d9918 does not need the IRQ, because it samples +0x10 directly. A VBUS level that is already
  present when userland starts USB control is therefore picked up without any interrupt. §4.2 relies on this.
* `fusb_start_gadget` @ 0x401d88e8 has four preconditions: state == 2, stored ID == 1 (B-device), stored VBUS == 0
  (present), and **ho+0x54 bit12 == 0** (EHCI PORTSC owner/power; reads 0 in the emulator). It then writes **otg+0x08 = 16**
  and sets state 3.
* Snapshot struct (0x4bd1d87c): state +0x80 = **1**, ID = 1, VBUS = 1 (absent). The WBI image was taken before userland
  started USB control. In the emulator after boot (0x4b1e4da0 / 0x4c0859a0 depending on the run), state = **2**, ID = 1, VBUS = 1.

### 1.6 Device-event bits (from `f_usb20hdc_on_usb_function` @ 0x401d55d4, IRQ 227)
* DEVS (0x204) and DEVC (0x200) share bit positions: DEVS is the status and DEVC the enable. A status bit is cleared by
  writing 0 to it and 1 to the other bits of the top byte (`(v & 0xFFFFFF) | 0xFD000000` clears bit25).

| DEVS bit | Handler action | Meaning [?] |
|---|---|---|
| 25 | device state 8, gadget `suspend` | suspend |
| 24 | restore state, gadget `resume` | resume |
| 29 | abort every EP, reset DMA, dequeue all (-103), gadget `disconnect` | bus reset start / disconnect |
| 28 | speed = DEVS bit17 ? FULL : HIGH, EP0 maxpacket 64, EPCONF0 maxpkt 0x40, state DEFAULT | bus reset end / speed enumerated |
| 27 | EPCMD0 bits 0x40000 / 0x80000, read 8 bytes from EP0 FIFO (count must be 8), clear EPCMD0 0x1000 / 0x2000, dispatch std / class / vendor | **SETUP received** |
| 26 | clear only | ? (SOF?) |

* INTS bit3 is cleared by writing `& ~8`, and bits 4/8/9 are written as 1. INTS bits 8/9 and bit16+n route to DMA end and
  per-EP handlers (`f_usb20hdc_on_end_in/out_transfer`). The exact bit map needs a second RE pass [?].
* EPCTRLn status bits 0x4000000/0x40000, 0x8000000/0x80000 and 0x20000000/0x200000 are each read three times for
  debounce in the ISR. Their meanings are IN/OUT buffer-ready/empty and stall [?].

---------------------------------------------------------------------------------------------------------------------

## 2. Software stack and connection sequence

### 2.1 Layers
```
kernel built-in: fusb_otg (glue) ─ usb_otgcore (/dev/usb/otgcore) ─ usb_gadgetcore (/dev/usb/gadgetcore)
                 f_usb20hdc_udc     usb_event (/dev/usb/event, ring buffer of {handle, cb, id, len, payload})
modules (boot):  usb_portMonitor (/dev/usb/portmon), usbg_sen, usb_extcmd
modules (later): usbg_storage, usbg_stillimage, usbg_buspower, usbg_cmn (CUsbSvc::ko_insmod in libusb.so)
userland:        libusb.so (CUsbSvc / CUsbGadget* / CUsbHost, descriptor tables)
                 libUsbControl.so (FSM, thread "UsbControl", i2 row of bootin)
                 libUsbBusPower.so (charger IC selector, /dev/usb/buspwr)
                 servers: libInfraMscServer (MSC), libInfraMtpServer / libUsbAppDlSvr (MTP, app download),
                          libInfraPtpControl (/dev/usb/sicd_*), libupdaterufp (updater), Android ScalarAUsbDlApp
```
`/usr/bin/usb_insmod.sh` also names `usb_event/usb_scd/ups/usb_otgcore/usb_gadgetcore.ko`. On this firmware those are built into
the kernel (`__boottime_sony_usb_init` @ 0x4034b254 calls `f_usb20hdc_udc_init`, `usb_event_module_init`,
`usb_otgcore_module_init` and `usb_gcore_module_init`, and prints the fixed string "sony_usb_init: CXD4132"), so the script is a leftover.

### 2.2 Boot (no cable). Observed with `a5ktrace` on `usb_otgcore_ioctl` @ 0x401dbd60
1. Kernel stage: `fusb_otg_probe` writes clkrst4 +0xA4 = 7 and sets otg+0x40/0x50 enables. `f_usb20hdc_udc_probe` runs
   EP0/EPCONF setup (EPCMD0 = 0x800, EPCTRL0 polls, EPCONF0 = 0x80040). This is a register access at probe, contrary to
   RE_STORAGE_USB §6.4. It happens in both stage-1 and stage-2 kernels.
2. Stage i2: thread **UsbControl** (libUsbControl → libusb `CUsbSvc::openSvc` @ 0x9f78) opens `/dev/usb/otgcore` and issues
   ioctls `0x4024E301` (register 9 callbacks), `0x4010E313` (PHY parameters, §1.3) and **`0xE304`**.
   `0xE304` runs `transition_to_idle` and then `fusb_start_control` from the same ioctl (lr 0x401db1d4). That function
   samples otg+0x10 and posts the CID/VBUS events. The state becomes 2.
3. Nothing else touches USB hardware afterwards (unimp log over ~5 M lines). The usb_event reader thread and the
   portMonitor thread block in their own threads. **No boot-blocking wait exists on the USB path.**

### 2.3 Cable plugged (device mode). Static analysis plus the dynamic run of §4.2
1. VBUS goes low, and IRQ 232 (or `start_control`) runs `fusb_notify_vbus(0)`. This queues an event, which reaches libusb
   `proc_kevent_otg` and then libUsbControl `sub_12B10` as msg 0x10000/0x10001. The UsbControl FSM handles it
   (`MsgId2EventId` @ 0x1020c, `actionGVbusON`).
2. The upper layer (app framework) switches the UI to USB mode. Observed: `Appframework` loads `viewUsbMTP.uxc`, and Android
   `UsbDeviceManager` reads `/sys/class/switch/usb_connected/state`. The menu setting **USB Connection** (Auto / Mass
   Storage / MTP / PC Remote, settings `SetupUSBConnection*`) and **USB LUN Setting** decide the function. "Auto" starts with
   MTP (the observed screen). The function bitmask is passed in message param 0x1005 (`UsbIfWrapper::StartGadget` @ 0x129ec:
   1→did 0, 2→11, 4→55, 8→58, 16→59). Which Backup ID holds the setting is unknown [?].
3. libusb `start_gadget`:
   1. insmod the function module.
   2. Speed ioctl `0x4001E30C`.
   3. **`0xE305`** → `fusb_start_gadget` (otg+0x08 = 16, state 3).
   4. Open `/dev/usb/gadgetcore` and issue `0x401CE201` (callbacks) and `0x400CE203` (descriptor table + serial from Backup 0x3E0005).
   5. Open `/dev/usb/portmon` and issue `0xF000` (charger detection, §0).
   6. `usb_gadget_probe_driver` → `f_usb20hdc_enable_communicate` → `f_usb20hdc_udc_otg_probe` @ 0x401cfb44: controller init,
      IRQ 227/228/229, pull-up via `f_usb20hdc_udc_gadget_pullup`.
4. The host then enumerates over IRQ 227 (bus reset → DEVS bit28, SETUP → DEVS bit27, …).
5. Stop path: gadgetcore `0xE204`/`0xE202`, then otgcore `0xE303` (`transition_to_stop` → `fusb_stop_control`) and `0xE302`.

### 2.4 Updater, app install, charging
* **Updater (UFP):** `nflasha7/root/common.src` uses `DATSRC_SENSER=0` (USB) and `DATSRC_MS=1`. `/tmp_updater/UFP` marks a running
  USB updater. `libupdaterufp` receives scrambled packets (`Dec_Scramble*`) into DMM or a ramdisk. The transport is the
  Sony **SENSER** vendor link: `usbg_sen.ko` is a tty line discipline (`usbg_sen_ldisc_*`, alias `tty-ldisc-17`), and
  `usb_extcmd.ko` hooks vendor commands into the MSC/PTP functions. Whether the updater uses the `sen` (0x0336) or the
  `updater` (0x03E2) descriptor table, and the tty node name, are unresolved [?]. Updater boot = loader mode 3.
* **PMCA / app download:** `libUsbAppDlSvr` (MtpAppDlSvr) insmods `usbg_stillimage.ko`. The transport is PTP/MTP plus Sony
  extension commands (`libInfraPtpExtCmd`, `mtpw_*_SonyDiExtCmd`) and Android `ScalarAUsbDlApp`.
* **Charging and USB power:**
  * When the camera is off, USB power is a power-on factor (`upm.ko bfkey2str`: `BF_KEY_USB_CONNECT`, `BF_KEY_USB_APO`) and
    loader mode 6 `LDR_BOOT_MODE_USB_CHARGE`.
  * `upm` maps Backup 0x51000F == 6 to the power-on string **"ON:USBJ"**, and bootin target `usbj` (0x20) is the only one
    that runs `libUsbChargingApp.so` (RE_USERLAND_SUBCPU). So **IMDB profile USBJ is the USB-charge boot profile**, not a jig [?].
  * `upm.ko sub_4A0` opens `/dev/usb/buspwric` to switch charging off during battery measurement. The node does not exist on
    this model, so it returns -1 and is harmless.
  * The menu "USB Power Supply" (On/Off) selects the `usbg_buspower` HID configuration (500 mA) [?].
  * The charger itself, and how VBUS reaches the sub-micro when the camera is off, are not modelled. That side belongs to
    RE_SUBMICRO_LINK / RE_INPUT_I2C_POWER [?].

---------------------------------------------------------------------------------------------------------------------

## 3. Firmware-facing register subset for a device-mode model

| Block | Must model | Can stay RAM / unimp |
|---|---|---|
| OTG 0xF2920000 | +0x10 VBUS/ID (machine state), pulses on GIC 232/233 when it changes, +0x08 / +0x04 mode strobes (store) | +0x00, +0x40, +0x50 |
| HDC 0xF0210000 | CONF bit2 autoclear (done), MODE, DEVS bit0 = 1 (done), DEVC/DEVS events (§1.6), INTEN/INTS, EPCMDn (busy bit31 = 0, command bits act on the EP state), EPCTRLn status bits, EPCONFn, EPCOUNTn, EPBUF windows 0x10000/0x18000 (FIFO RAM), FADDR, DMAC1/2 handshake, +0x104 line state | +0x18, OTGC… 0x3xx, TESTC |
| HDMAC 0xF0204000 | 2 channels: DMACA/DMACB/DMACSA/DMACDA at 0x10 + 0x10·ch. Execute the FIFO↔RAM copy on enable and raise GIC 228/229 | DMACR global [?] |
| PHY 0xF0202000 | — | 0xF020200C tuning (store) |

The DMA path is likely to be used for bulk endpoints because the EP table assigns channels (§1.2). A PIO-only model would
make `f_usb20hdc_set_in/out_transfer` take the DMA branch and stall. The HDMAC therefore has to be part of the model [?:
confirm with a run that reaches `f_usb20hdc_set_dma_controller`].

---------------------------------------------------------------------------------------------------------------------

## 4. What happens in the emulator today (frozen binary, pristine NAND)

### 4.1 No cable (current board: `otg 0x10=3`, `usb-hdc 0x204=1`, CONF autoclear)
* The unimp log shows exactly the §2.2 sequence: probe twice (stage 1 + 2), then one `0xE304` bring-up.
  `otg+0x10` is read 4 times in total. There are no errors and no further USB accesses.
* fusb state after boot: state 2, ID 1, VBUS 1. **"Unplugged" is already correct; plan item (a) is done.**
* Unrelated observation, not USB: in the first run a `liro-kliro_100` thread oopsed at t ≈ 28.7 s
  (NULL PC, `tsk_thread_helper` in liro). The other two runs did not show it in the inspected window.

### 4.2 VBUS forced present (gdb: hbreak `fusb_start_control`, write `*(int*)0xF2920010 = 2`)
* `start_control` posts "VBUS on, B-device". The event reaches userland, and the **stock UI shows "USB mode / connecting to
  MTP…"** (USB Connection = Auto on this NAND). Files opened after the event: `viewUsbMTP.uxc`, `ScalarAUsbDlApp.apk`,
  `/sys/class/switch/usb_connected/state`, `/sys/class/usb_composite/accessory/enable`.
* Within the next 120 s, none of the following happen: otgcore/gadgetcore ioctl, `sys_init_module`, open of `/usr/kmod/usbg_*.ko`,
  `/dev/usb/portmon` or `/dev/usb/buspwr`, or any HDC access. **The gadget is never started.**
* Candidate causes [?]: (1) the MTP server is loaded on demand (msg 0x940021 → `InfraOnDmdLdr`), and `libInfraMtpServer.so`
  was never opened; (2) it waits for a media resource (this run had no SD card); (3) it waits for an Android-side handshake
  (`ScalarAUsbDlApp`, `SyncAndroid`). A run with `A5K_SD` and with USB Connection = Mass Storage would separate these.

### 4.3 Tooling note
A gdb memory access to the GIC distributor (`0xF800121C`, an attempt to set IRQ 232 pending) made the QEMU process exit.
Inject VBUS through the `start_control` sampling path, or add a proper board hook (§5 step 1).

---------------------------------------------------------------------------------------------------------------------

## 5. Modelling plan

**(a) Unplugged: done.** Keep `otg+0x10 = 3`, `usb-hdc` with `0x204=1` and CONF bit2 autoclear. Optionally add an unimp/RAM
window for 0xF0202000 so the PHY tuning write is logged under its own name.

**(b) Virtual host. QEMU's `hcd-dwc2` cannot be reused:** it is a Synopsys DWC2 *host* controller, while this is a
Fujitsu F_USB20HDC *device* controller. QEMU has no UDC framework, so the device side must be a new model plus a
transport that presents the camera's gadget to a host. Steps:

1. **`cxd90014-usbotg` cable state (≈0.5 day).** Add machine property `usb=off|host|charger` and an HMP/QMP command to plug and
   unplug. Plug sets +0x10 bit0 = 0 and keeps bit1 = 1, then pulses GIC 232. A charger additionally makes hdc+0x104 read
   0x60 (DP = DM = 1) so `usb_portMonitor` reports DCP. This also gives the UI test of §4.2 without gdb.
2. **`cxd90014-usbd` (F_USB20HDC subset, ≈1 week):**
   * register file of §1.1 with W0C DEVS/INTS semantics;
   * bus reset (DEVS 29 → 28, HS);
   * SETUP injection (8 bytes into the EP0 FIFO, EPCOUNT, DEVS 27, IRQ 227);
   * EP0 IN/OUT data stages through EPBUF/EPCOUNT;
   * EPCMD busy = 0, with commands applied immediately;
   * FADDR;
   * bulk EP FIFOs with ready/empty bits.

   Milestone: the gadget enumerates and descriptors match §0.
3. **HDMAC 2-channel model (≈2 days):** copy on enable, terminal count, IRQ 228/229. Cross-check bit names against Linux
   `drivers/dma/milbeaut-hdmac.c`.
4. **Transport (≈3–5 days), one of:**
   * **USB/IP server** (TCP 3240, OP_REQ_DEVLIST/IMPORT, CMD_SUBMIT/RET_SUBMIT/UNLINK). A Linux host attaches it natively with
     `usbip attach`, after which gphoto2/MTP/mass-storage tools, `fwtool`/Sony-PMCA-RE work unchanged. There is no stock client
     on macOS.
   * **usbredir, device-holder side** (libusbredirparser through a chardev). Another QEMU VM (`-device usb-redir`) or a SPICE
     client sees the camera. Works on any host OS, but needs a second VM.

   Both map onto the same internal API: `submit(ep, setup/data) → complete(status, data)`, with NAKs held until the guest
   arms the endpoint.
5. Unblock the user-space stall of §4.2 first, or enumeration never starts. This is RE in the USB app and MTP server load path,
   not hardware work.

Total ≈ 2.5–3.5 weeks of focused work for MSC + PTP/MTP [?]. Tags: everything in §1.1–1.5 is S/V; §1.6 bit meanings are H.

---------------------------------------------------------------------------------------------------------------------

## 6. Open questions
1. Why userland stops at "connecting to MTP" without issuing `0xE305` (§4.2). Repeat with an SD card and with USB Connection =
   Mass Storage / MTP. Trace `osal_snd_msg` 0x940021 and the libUsbControl FSM.
2. Full INTS / EPCTRL / EPCMD bit map (ISR second pass, plus `f_usb20hdc_set_in_transfer_pio` @ 0x401ce140 and
   `_set_out_transfer_pio` @ 0x401cb56c). Sony's published GPL kernel source for this model would name them directly
   (`f_usb20hdc_udc.c`, `fusb_otg.c`) if it is available [?].
3. The Backup ID of the USB Connection / LUN / USB Power Supply settings, and of the charger-IC selector bytes in
   libUsbBusPower (`sub_29F0` / `sub_2978`; Hex-Rays dropped the `Backup_read` arguments).
4. Updater transport: the tty node of `usbg_sen`, whether SENSER rides on 0x0336 or 0x03E2, and how `extcmdsvc_hndl` binds
   to `usb_extcmd`.
5. Real-camera confirmation of otg+0x10 polarity (read 0xF2920010 with and without a cable) and of hdc+0x104 on a charger.
6. How VBUS presence reaches the sub-micro for power-off charging (`BF_KEY_USB_CONNECT`).

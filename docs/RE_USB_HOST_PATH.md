# USB host path: firmware userland, Sony-PMCA-RE tools, and a TCP cable for macOS

Scope: what has to happen above the USB device controller so that the host tools from Sony-PMCA-RE
(`pmca-console info / install / firmware / updatershell`) can talk to the emulated a5000 (firmware 1.10, unmodified).
It covers the firmware userland side (why the gadget did not start, what the updater core waits for), what each tool
expects on the wire, and a host-side connection design that works on macOS. The register-level controller model is in
`docs/RE_USB_DEVICE_CONTROLLER.md`. The software stack and VBUS glue are in `docs/RE_USB.md`. Neither is repeated here.

Evidence:
* One dynamic run (r1) of the repository binary at HEAD `71e9fc0` on a pristine NAND clone, no SD card. VBUS was forced
  present at the firmware's first sample (gdb `hbreak fusb_start_control`, `set *0xF2920010 = 2`, as in RE_USB §4.2).
  Probes used the `a5ktrace` plugin (`libobj=1`, `fn=` user-library probes, kernel `pc=` probes on `usb_otgcore_ioctl`,
  `usb_gadgetcore_ioctl`, `fusb_start_gadget`, `f_usb20hdc_udc_otg_probe`, `sys_init_module`, `do_sys_open`). Controller
  registers were read afterwards with gdb (`0xF021xxxx`, `0xF292xxxx`, never the GIC).
* Local Hex-Rays output: `re/ida/ui/ui_libUsbControl.so.c`, `ui_libusb.so.c`, `ui_libUsbAppDlSvr.so.c`, `re/ida/libObj.so.c`,
  `re/ida/upd/libupdaterufp.so.c`, `re/ida/libIMDB.imdb_raw.txt`.
* `arm-none-eabi-objdump` (code, `.data`, dynamic relocations) of `/usr/lib/libusb.so`, `libInfraMtpServer.so`,
  `libInfraMscServer.so`, `libUsbAppDlSvr.so`, `libInfraRemote.so`, and of the updater core's `/usr/bin/sauu` and
  `/usr/lib/libusb.so` (cramfs initrd of nflasha1, version 41.01.28).
* Backup values from `re/backup/whitelist/backup_props_all.tsv`.
* Sony-PMCA-RE source, local checkout `tools/Sony-PMCA-RE` at `a82f5ba` (2022-08-18), read only.

No IDA session was needed. Facts carry their evidence. Guesses are marked **[?]**. User-library addresses are offsets in the
library file (IDA base 0). Kernel addresses are VAs of the normal 1.10 kernel (`re/vmlinux.elf`).

---------------------------------------------------------------------------------------------------------------------

## 0. Summary

| Question | Answer |
|---|---|
| Why userland never issued 0xE305 (RE_USB §4.2) | **That stall is gone in the current build.** In run r1, with VBUS forced present, the firmware went through the whole chain within one boot: `ModelUsbMtp` created → MTP/MSC servers loaded → `CONFIGURE_FUNC_CMD` → `START_GADGET_CMD` → `START_GADGET_ALL_RECEIVED` → `StartGadget(did 11)` → otgcore `0x4001E30F`, `0x4001E30C`, **`0xE305`** → `fusb_start_gadget` → `f_usb20hdc_udc_otg_probe` → gadgetcore `0x401CE201` / `0x400CE203` → `/dev/usb/portmon` → `START_GADGET_CMP(ok)`. Afterwards the controller is pulled up: MODE = 0xE, DEVC = 0x3B000000 (bit16 clear), otg+0x08 = 16, fusb state 3. RE_USB's run used build `bab067e`, which predates the camera-engine fixes. The most likely cause of the old stall is the model-tree switch hang of RE_SD_MEDIA_SERVICE / RE_PLAYBACK_SWITCH (leaving the shooting tree waits for `EE_STOP_CMP`) **[?: inferred from the timing of the fixes; the USB path itself was not traced in the old build]**. |
| What the emulator must provide for the gadget to start | Only the VBUS level: otg+0x10 = 2 when `fusb_start_control` samples it, or a GIC 232 pulse when it changes later. **No SD card, no Android handshake and no setting change are needed.** Line state hdc+0x104 = 0 (SDP). |
| What the gadget then waits for | A host: bus reset and SETUPs (RE_USB_DEVICE_CONTROLLER §4.2/§4.3). libusb arms an **enumeration timer of 60 s** (Backup 0xE70030 = 0x003C) when the pull-up event arrives (`CUsbSvc::mes_pullup_event` @ 0xaef4). |
| Updater core `sauu` | It waits for libusb's `CONNECTION` callback with `connection.val == 1`. That value comes from the same otgcore VBUS event (event 1, "on"). On it, `sauu` calls `usbif_hndl::req_start(did 56)`, which is the **updater MSC gadget 054C:03E2**. The emulator stimulus is identical: VBUS present at `fusb_start_control`, or a GIC 232 pulse. |
| Transports used by the tools | `info`: PTP/MTP vendor ops 0x9280/0x9281 (MTP mode) or SCSI 0x7A (mass-storage mode). `install`: same, then re-enumeration as the MTP app-install device 054C:06A8 (ops 0x9488/0x9489/0x948C/0x948D). `firmware` / `updatershell`: the same ext-cmd channel, then **camera reboot** into the updater core and re-enumeration as MSC 054C:03E2 (SCSI 0x7A). SENSER (vendor bulk, 054C:0336) is only used by `serviceshell`. |
| Host connection | **Recommended: a small custom framing ("A5KU") over TCP**, served by the emulator's USB device model through a QEMU chardev, plus a new Sony-PMCA-RE backend `pmca/usb/driver/generic/socket.py` selected with `-d socket`. Pure Python, no kernel driver, no root, so it works on macOS. The existing `-d qemu` driver cannot carry MTP (§5.1). USB/IP is the right second transport for a Linux host (real kernel USB stack). macOS has no client, so it would need a Linux VM. usbredir puts the wrong side on the emulator. |
| First milestone | `pmca-console info -d socket` against an emulator booted with the cable plugged: 15 steps in §6. Everything on the firmware side up to the pull-up already works today. The work left is the controller model (RE_USB_DEVICE_CONTROLLER), the A5KU server in that model, and the Python backend. |

---------------------------------------------------------------------------------------------------------------------

## 1. Userland: from VBUS to the pull-up (run r1)

### 1.1 Observed sequence

From the trace (`A5KF` = user-library probe, `A5KT` = kernel probe; task names in brackets):

| # | Where | What (evidence) |
|---|---|---|
| 1 | kernel, `[UsbControl]` | ioctls on `/dev/usb/otgcore` at boot: `0x4024E301`, `0x4010E313`, `0xE304` (RE_USB §2.2). gdb then sets otg+0x10 = 2 at `fusb_start_control` @ 0x401d9918. |
| 2 | libObj (`Appframework`) | `ModelUsbAppDownloadToInstance` (libObj 0x5cb448) and **`ModelUsbMtpToInstance`** (0x5cfd48) are called. `dlsym` resolves `ViewUsbMTPToInstance`, and `viewUsbMTP.uxc` + `master_other.uxc` are opened. `ModelUsbAutoToInstance` and `ModelUsbMassToInstance` are **not** called, so this NAND's USB Connection setting selects MTP **[?: setting Backup ID still unknown, RE_USB §6.3]**. |
| 3 | `[ObjMediaServer]` | opens `libInfraMscServer.so`, then `sys_init_module` of a 0xFCF0-byte module (= `usbg_storage.ko`, 64752 B). Opens `libInfraMtpServer.so`, then insmod of 0x6B08 bytes (= `usbg_stillimage.ko`, 27400 B). Opens `libInfraPtpControl.so`, then reads `/mnt/dat2`, `/mnt/dat3`, `/setting/updater/dat4`. |
| 4 | libUsbControl | `MsgId2EventId(0x83003)` → `actionGConfigureFunc` @ 0x11b3c (FSM event 11). This is `MSGID_CONFIGURE_FUNC_CMD`, sent by `MtpServer::ConfigureFuncCmd` @ 0xaf68 with PARAMID_FUNCTION (0x1005) = **2** (`movs r1,#2` @ 0xaf98). |
| 5 | `[ObjMediaServer]` | loads the ext-cmd plug-ins: `libInfraExtCmdSvc`, `libusbcmd`, `libInfraPtpExtCmd`, `libInfraDevInfoSender`, `libInfraKikiLogSender`, `libInfraExtBackupCommunicator`, `libInfraScalarExtCmdPlugIn`, `libInfraLensCommunicator`. Each opens `/dev/usb/usb_extcmd`. |
| 6 | libUsbControl | `MsgId2EventId(0x83001)` (`MSGID_START_GADGET_CMD`, from `MtpServer::jobStartFuncCmd` @ 0xc158, function 2) → `actionGStartGadget` @ 0x11e10 → `chkStartGadgetOK` @ 0x10924 → `MsgId2EventId(0xA7001)` (`MSGID_START_GADGET_ALL_RECEIVED`) → `actionGGadgetStartEntry` @ 0x11cb8 → `UsbIfWrapper::StartGadget(mask 2)` @ 0x129ec. |
| 7 | kernel, `[UsbControl]` | otgcore `0x4001E30F`, open `/var/usbport`, `0x4001E30C`, **`0xE305`** → `fusb_start_gadget` → `f_usb20hdc_udc_otg_probe`, `0x4001E30C`, open `/dev/usb/sicd_core` and `/dev/usb/gadgetcore`, gadgetcore **`0x401CE201`, `0x400CE203`**, open `/dev/usb/portmon`. |
| 8 | libUsbControl | `sndStartGadgetCmp(0)` @ 0x112a0 (success). |
| 9 | gdb afterwards | hdc CONF = 0x2, MODE = 0xE, INTEN = 0xE, **DEVC = 0x3B000000** (bit16 = 0: pulled up), otg+0x08 = 0x10, otg+0x10 = 2, fusb struct 0x4bd1d900 = 3 (state "gadget started"). |
| 10 | screen | A black dialog: "연결된 장치를 확인하여 주십시오." ("Check the connected device."). It was unchanged a few minutes later, and no stop ioctl followed **[?: whether this is the normal "waiting for host" text or the reaction to the 60 s enumeration timer]**. |

So the firmware userland is ready. Nothing in it waits for media, for Android, or for any other handshake before the
pull-up. `MtpServer` also has a `jobPTPInitwithoutMedia` path.

### 1.2 How UsbControl decides to start the gadget

`UsbControl` (libUsbControl.so, thread `UsbControl`) is a gatekeeper with two bitmasks in its object:
* word 37 = functions **configured** (`actionGConfigureFunc`: OR of PARAMID_FUNCTION from each `CONFIGURE_FUNC_CMD`, 0x83003);
* word 38 = functions **started** (`actionGStartGadget`: OR from each `START_GADGET_CMD`, 0x83001).

`chkStartGadgetOK` posts `0xA7001` only when word 38 == word 37. The FSM then enters the gadget-start state, whose entry
calls `UsbIfWrapper::StartGadget(word 38, word 55)`. That function maps the mask to a libusb descriptor id (did):

| PARAMID_FUNCTION | Server that sends it (evidence: the `movs r1,#N` before PARAMID 0x1005) | did | descriptor table (relocation at did_info+12) | VID:PID |
|---|---|---|---|---|
| 1 | `MassStorageServer` (libInfraMscServer 0x4c5c, 0x56a0) | 0 | `storage_desc_tbl` | 054C:047D |
| 2 | `MtpServer` (libInfraMtpServer 0xaf98, 0xc108, 0xc172) | 11 | `pictbridge_desc_tbl` | 054C:034A |
| 3 (1\|2) | both | 12 | `auto_desc_tbl` | 054C:02D7 |
| 4 | **[?]** (bus-power HID) | 55 | `buspower_desc_tbl` | 054C:04EF |
| 8 | `MtpAppDlSvr` (libUsbAppDlSvr 0xabf2, 0xace2, 0xade2) | 58 | `appdl_desc_tbl` | 054C:06A8 |
| 16 | `InfraRemote` (libInfraRemote 0xa524…) | 59 | `remotepc_desc_tbl` | 054C:0641 |
| — | updater core (`sauu`, §2) | 56 | `updater_desc_tbl` | 054C:03E2 |
| — | not started by these servers | 13 | `sen_desc_tbl` | 054C:0336 |
| — | — | 57 | `adb_desc_tbl` | 18D1:0D02 |

* The did → table mapping comes from `ptbl_did` (libusb .data 0x1ade4, 60 pointers to did_info) and the dynamic relocations
  `R_ARM_ABS32 *_desc_tbl` at did_info+12 (`CUsbSvc::InitDescriptors` @ 0xb5ec reads +12). VID/PID are the u16 pair at
  table+32. The same layout holds in the updater core's libusb.so (different build, same tables and PIDs).
* `DescriptorMaintainer::Overwrite` (libusb 0xb89c) can patch descriptors from Backup preset data **[?: whether it
  changes VID/PID on a retail camera]**. The descriptor block also takes the serial from Backup 0x3E0005 (RE_USB §2.3).
  On this NAND, 0x3E0005 = "ILCE-5000" padded to 16 bytes.
* Function 4 is the only bit with no identified sender. Its did 55 is excluded from `InitDescriptors`' loop and is the HID
  bus-power configuration.

### 1.3 After the pull-up

* `CUsbSvc::proc_kevent_otg` @ 0xa870, case 3 (pull-up event) → `mes_pullup_event` @ 0xaef4: on pull-up it queues a timer
  of `1000 * get_enumeration_timeout_value()` ms with `proc_host_enum_timeout_cb`. `get_enumeration_timeout_value` @
  0xaec0 reads Backup **0xE70030** (2 bytes), default 60. This NAND holds **0x003C = 60 s**. The timer is cancelled when the
  pull-up goes away. What the timeout triggers in the upper layers is not traced **[?]**.
* Consequence for the emulator: a host must reset and enumerate the device within **60 s of guest time** after the
  pull-up. Otherwise the user has to re-plug (VBUS off/on).
* Enumeration events go back up as `MSGID_BUS_RESET_EVT` 0xA4007, `MSGID_SETCONFIG_EVT` 0xA4001 (with per-function variants
  0xA4002 MASSSTORAGE, 0xA4003 MTP), `MSGID_SETINTERFACE_EVT` 0xA4005, suspend/resume 0xA4008/0xA4009, `MSGID_DEVICE_INFO_EVT`
  0xA6002, `MSGID_PULLUP_TIMEOUT_EVT` 0xA6004, `MSGID_ENUM_TIMEOUT_EVT` 0xA6005 (names: `UsbControl::getMsgIdString`
  @ 0x10428 and libObj's DefUsbControl table). The SETCONFIG broadcast is how the app learns that a host configured the
  device.

### 1.4 What the emulator must provide (userland side)

| Item | Value | Status |
|---|---|---|
| VBUS level | otg+0x10 = 2 (bit0 = 0 present, bit1 = 1 B-device). If set before `fusb_start_control` runs (boot), no IRQ is needed. If changed later, pulse GIC 232. | V (r1) for the boot case. The later-plug case is RE_USB_DEVICE_CONTROLLER §6 |
| Line state hdc+0x104 | 0 → `usb_portMonitor` classifies SDP (PC host) | S (RE_USB §1.4) |
| ho+0x54 bit12 | 0 (precondition of `fusb_start_gadget`) | V (r1 reached state 3) |
| Anything in userland | nothing: no SD, no setting change, no Android action | V (r1) |
| Host within 60 s of pull-up | bus reset + standard enumeration | S (§1.3) |

The board needs a cable control (RE_USB §5 step 1). Proposal: machine property `usb=off|pc|charger` (initial state) and an
HMP/QMP command for plug/unplug. The TCP client connection should **not** imply VBUS by default (a cable is a physical
event). An opt-in `usb-plug-on-connect=on` is a convenience.

---------------------------------------------------------------------------------------------------------------------

## 2. Updater core: what `sauu` waits for

From `/usr/bin/sauu` (updater initrd 41.01.28) disassembly:
* `execute_ufp.sh` → `prepare-usb.sh` (insmod `usb_portMonitor`, `usbg_sen`, `usb_extcmd`, `usbg_storage`) → `sauu`.
* `USBHandler` calls `usbif_initialize`, then `usbif_hndl::open`. libusb then reports the current connection state through
  `USBHandler::ConnectionCallback` @ 0xc0a8. The log line `connection.val = 0` in RE_UPDATER_BOOT §5 is that callback
  (no cable).
* `ConnectionCallback` maps `connection.val == 1` → EventID **5**, anything else → EventID 6 (`cmp r3,#1; moveq r8,#5;
  movne r8,#6` @ 0xc0ea).
* `WaitingConnectionState::Execute` @ 0xb9c0 acts only on EventID 5. It calls `USBHandler::Start(param)` @ 0xc2ac, which
  calls `set_select_port(param.port)` and then **`req_start(0x38 = did 56, StartStopCallback)`** (`movs r6,#56` @ 0xc30c).
* did 56 = `updater_desc_tbl` = **054C:03E2** (updater libusb `ptbl_did[56]`+12 → `R_ARM_ABS32 updater_desc_tbl`). The
  function is mass storage (`usbgmscif_init`, `usbgmscif_setReadOnly`, `usbgmscif_setMediaStatus`), and commands arrive
  through `usbcmd_t::Receive/Send` (libusbcmd, i.e. `usb_extcmd` vendor SCSI). This matches Sony-PMCA-RE
  `SONY_ID_PRODUCT_UPDATER = [0x033a, 0x03e2]` and `SonyMscUpdaterDevice`.
* In libusb, `CUsbSvc::proc_kevent_otg` case 1 (VBUS) turns kernel state 1 into "on" and 2 into "off". So
  `connection.val == 1` means **the otgcore VBUS-on event** **[?: the updater libusb is a different build; the case table
  was read in the main build only]**.

Emulator requirement: the same as §1.4. VBUS present when the updater kernel's `fusb_start_control` samples otg+0x10, or
a GIC 232 pulse later. The updater kernel is a separate build **[?: its `f_usb20hdc_udc` was not compared with the normal
kernel's; the register usage is assumed identical]**. The emulator's VBUS state must survive the board reset used for the
updater boot (the cable stays plugged while the camera reboots).

`ud_timeout_poweroff.elf` runs next to `sauu` and presumably powers the camera off after an idle timeout
**[?: duration not read]**. If it fires, the host sees a detach.

---------------------------------------------------------------------------------------------------------------------

## 3. Tools, camera modes and transports

### 3.1 Device discovery in Sony-PMCA-RE

`pmca/commands/usb.py listDevices()` goes through every driver context and asks for devices with **VID 0x054C** whose
**first interface** has class 8 (MSC), 6 (PTP/still image) or 0xFF (vendor). Then:

| Class | Probe | Accepted as | Condition |
|---|---|---|---|
| 8 MSC | SCSI INQUIRY (5 bytes, then full) | `SonyMscUpdaterDevice` | vendor "Sony" and product "DSC"/"Camcorder", **and** PID ∈ {0x033A, 0x03E2} |
| | | `SonyMscExtCmdDevice` | vendor/product as above, any other PID |
| 6 PTP | OpenSession(1), GetDeviceInfo | `SonyMtpExtCmdDevice` | Manufacturer "Sony Corporation", VendorExtensionDesc "", ops ⊇ {0x9280, 0x9281, 0x9282} |
| | | `SonyMtpAppInstallDevice` | Manufacturer "Sony Corporation", VendorExtensionDesc contains "sony.net/SEN_PRXY_MSG:", ops ⊇ {0x9488, 0x9489, 0x948C, 0x948D} |
| 0xFF | — | `SonySenserDevice` | PID ∈ {0x02A9, 0x0336} |

Firmware side: "Sony Corporation" is in `libptp.so` and `libUsbAppDlSvr.so`, and "sony.net/SEN_PRXY_MSG: 1.0;" is in
`libUsbAppDlSvr.so`. `MtpServer` implements `SonyDiExtCmd` and `SonyReqReconnect` (`mtpw_cmd_SonyDiExtCmd`, …). The MSC
INQUIRY strings for the 1.10 gadgets were not located **[?]**.

Exactly one device must be found (`getDevice`). PMCA uses the **first** IN and OUT bulk endpoints of interface 0
(`GenericUsbDriver._findEndpoints`, `bmAttributes == 2`).

### 3.2 The ext-cmd channel (shared by every tool)

`sendSonyExtCommand(cmd, data, bufferSize)`:
* **MTP:** op 0x9280 `SonyDiExtCmd_write` [cmd] with data phase = `data`, then op 0x9281 `SonyDiExtCmd_read` [cmd] with
  a read data phase. Retried while the response is 0x2019 (DeviceBusy).
* **MSC:** CDB `7A <cmd LE32> 00×7` (12 bytes). First a write data phase of `data`, then the same CDB with a read data
  phase of `bufferSize`. Retried while the sense is (9, 0x81, 0x81).

| cmd | User (camera library) | Used by |
|---|---|---|
| 0 | updater protocol (`libupdaterufp.so` in normal mode, `sauu` in the updater core): 32-byte `PacketHeader` + body, commands INIT 0x1, CHK_GUARD 0x10, QUERY_VERSION 0x20, SWITCH_MODE 0x30, WRITE_FIRM 0x40, COMPLETE 0x100, GET_STATE 0x200; 512-byte response buffer | `info` (INIT + QUERY_VERSION), `firmware`, `updatershell` |
| 1 | DevInfoSender: (1,1) GetModelInfo | `info`, `updatershell` (model name) |
| 3 | GpsAssist (3,1) | `info` (optional, errors ignored) |
| 5 | ScalarExtCmdPlugIn: (5,2) NotifyScalarDlmode | `install` |
| 6 | LensCommunicator (6,2) | `info` (optional) |
| 0x23 | updater shell (code in Sony-PMCA-RE `updatershell/usbshell.cpp`, runs inside the uploaded FDAT) | `updatershell` |

The sub-command channels use a 16-byte `ExtCmdHeader` and 0x2000-byte buffers (`SonyExtCmdCamera._sendCommand`). All of
these plug-ins were loaded in run r1 (§1.1 step 5). `libupdaterufp.so` is an on-demand module (IMDB row 120) that was not
loaded during r1 **[?: loaded on the first cmd-0 packet]**.

### 3.3 Per tool

| Tool | Starting mode (camera) | Device seen first | Steps | Re-enumeration / reboot | Final device |
|---|---|---|---|---|---|
| `info` | USB Connection = MTP (this NAND) or Mass Storage | MTP 054C:034A, EP1 IN bulk, EP2 OUT bulk, EP3 IN interrupt (MSC: 054C:047D, EP1 IN, EP2 OUT) | GetModelInfo (1,1); updater INIT + QUERY_VERSION (cmd 0); lens (6,2); GPS (3,1) | none | same |
| `install` | MTP or MSC | as `info` | (5,2) NotifyScalarDlmode, then poll `listDevices` every 0.5 s ×10 (5 s) | camera stops the MTP gadget and starts did 58 (pull-up off → on) | MTP app-install 054C:06A8: GetProxyMessageInfo/GetProxyMessage/SendProxyMessageInfo/SendProxyMessage (0x9488/0x9489/0x948C/0x948D); a TCP/SSL proxy to Sony-PMCA-RE's local HTTPS "market" server; the Android app `ScalarAUsbDlApp` downloads and installs the APK |
| `firmware` | MTP or MSC | as `info` | INIT, CHK_GUARD (the .dat header windowed), QUERY_VERSION, **SWITCH_MODE 0x30** | camera: `libupdaterufp` `sub_17C0` → `system("up.sh user")` (creates `/setting/updater/mode`) + an osal message to request a restart **[?: target 0x920141 = power/reboot]** → reboot into the updater core (RE_UPDATER_BOOT) → `sauu` → did 56. Host polls 60 × 0.5 s = **30 s** | MSC updater 054C:03E2: INIT, CHK_GUARD, QUERY_VERSION, WRITE_FIRM (windowed), COMPLETE; then the camera writes NAND and reboots |
| `updatershell` | MTP or MSC | as `info` | GetModelInfo → model "ILCE-5000" → FDAT = `updatershell/fdat/CXD90014/ILCE-5000.hdr` + `CXD90014.dat`; then the `firmware` flow with that FDAT | as `firmware` | after WRITE_FIRM: cmd 0x23 shell (`TEST`, `SHEL`, `PUSH`, …) served by the FDAT code running in the updater |

Notes:
* `firmware` with a real `.dat` really reflashes the emulated NAND. Use NAND clones only.
* The 30 s host poll after SWITCH_MODE covers main-firmware shutdown + reset + updater boot. RE_UPDATER_BOOT measured
  ~36 s of emulated time from reset to the `sauu` wait, so the first attempt will likely time out. Sony-PMCA-RE then says
  "run this command again". The second run finds the `SonyMscUpdaterDevice` directly. This is acceptable, but slow
  emulation shows up here.
* `install` additionally needs the camera's Android side (`ScalarAUsbDlApp.apk`, opened in RE_USB §4.2) and HTTPS, which
  means a sane RTC/date for certificate checks **[?]**. It belongs in a later milestone.
* `serviceshell` (not one of the four tools): MSC device → vendor control request `bmRequestType 0x43, bRequest 1, wValue
  0x37FF, wIndex 0xD7AA` → SENSER 054C:0336, 4 bulk EPs, authentication with fixed keys. It needs the libusb-style backend
  (`isinstance(driver, GenericUsbDriver)`), which the socket backend provides.

---------------------------------------------------------------------------------------------------------------------

## 4. Host-side connection options (macOS)

### 4.1 Requirements from the tools
* Control OUT/IN with any bmRequestType (standard, class `0x21`, vendor `0x43`), bulk IN/OUT on two pairs (SENSER uses EP3/EP4
  as a terminal: `read(0x40, ep=1, timeout=100 ms)`), and interrupt IN (PTP events, never read by PMCA).
* **Short-packet semantics:** a bulk read of N bytes must return early on a short packet or ZLP. `MtpDriver._readPtp` reads 512
  and gets 12-byte responses; it loops over ZLPs (`while data == b''`).
* STALL reported as an error (`GenericUsbException`), plus clear-halt (MSC phase errors).
* Read timeouts (default 5 s in the libusb backend, 100 ms for the SENSER terminal).
* Detach and re-attach with a different PID while the tool keeps running (install, firmware).

### 4.2 Comparison

| Option | Emulator side | macOS host side | Verdict |
|---|---|---|---|
| **Existing `-d qemu` driver** (`pmca/usb/driver/generic/qemu.py`, TCP localhost:7642, 8-byte header `{u8 flags, u8 ep, u16 pad, u32 length}`, FLAG_SETUP 1 / FLAG_RESET 2, reply length 0xFFFFFFFD = STALL, bit31 = NAK → client re-polls 100 × 10 ms) | simple | none needed | **Not usable for MTP.** The IN loop subtracts each reply from the requested length and loops until it reaches 0, so a short packet never completes a read (an MTP response then times out). No control requests other than its own enumeration (`classInterfaceRequestOut` / `vendorRequestOut` raise), no vendor-class context, `recv` without a full-read loop, a 4 s sleep + full re-enumeration in every `reset()`, endpoints of interface 0 only, no timeouts. It was written for another emulator **[?: origin]**. It is good enough for an MSC-only smoke test at most. |
| **USB/IP** (TCP 3240: OP_REQ_DEVLIST/OP_REQ_IMPORT, then USBIP_CMD_SUBMIT/RET_SUBMIT/CMD_UNLINK/RET_UNLINK) | a server inside the device model: devlist from cached descriptors, URB queueing, unlink | **no client on macOS** (no `vhci-hcd`). Needs a Linux VM with `usbip attach`, and then pmca with libusb inside the VM | High fidelity on Linux: the real kernel USB stack, gphoto2/libmtp work. But two machines and a VM for every macOS run, and the device side must also emulate vhci quirks (SET_ADDRESS is never forwarded, port reset arrives as a `SET_FEATURE(PORT_RESET)` URB). Good **second** transport, for Linux. |
| **usbredir** | QEMU implements the *host-controller* side (`-device usb-redir` imports a remote device into a guest bus). The device-holder side (`usbredirhost`) wraps a real libusb handle. So the emulator would need its own implementation of the usbredirparser device side. | another QEMU VM with a Linux guest, or a SPICE client. There is no Python client | Wrong side for us, and the heaviest option. |
| **Custom "A5KU" framing over TCP + pmca `socket` backend** | a small chardev protocol inside the device model: control, bulk/interrupt IN/OUT, reset, cancel, attach/detach events | pure Python inside Sony-PMCA-RE. No driver, no root, no ImageCapture/`ptpcamerad` interference, same on every OS | **Recommended.** Smallest code on both sides. Semantics map 1:1 onto the controller model (a held URB = NAK). Detach and re-attach are explicit events with a generation counter. A Python bridge A5KU ↔ USB/IP can be added later for Linux users without touching the emulator. |

**Recommendation:** implement A5KU (§5) in the device model as the only transport in the emulator, and add the
Sony-PMCA-RE backend `socket`. Keep USB/IP as an optional host-side bridge script **[suggestion]**.

---------------------------------------------------------------------------------------------------------------------

## 5. The A5KU protocol ("a5000 USB over TCP")

### 5.1 Transport and lifecycle
* TCP stream. The **emulator is the server**: a QEMU chardev, e.g.
  `-chardev socket,id=usb0,host=127.0.0.1,port=7643,server=on,wait=off` plus a device/machine property `usb-chardev=usb0`
  (names are suggestions). One client at a time. Port 7643 avoids confusion with the old `qemu` driver's 7642.
* All integers little-endian. Every message = 16-byte header + `length` bytes of payload.
* The client sends `HELLO` first. The device replies `HELLO_ACK` and from then on pushes `EVENT`s and completions.
* The connection is not the cable. VBUS is controlled on the emulator side (§1.4). Closing the TCP connection cancels all
  pending requests and leaves the camera plugged in. If the gadget is attached, a new client must RESET before using it.

### 5.2 Header

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 2 | magic | 0x5541 ("AU") |
| 2 | 1 | type | §5.3 |
| 3 | 1 | ep | USB endpoint address with the direction bit (0x00 = EP0, 0x81 = EP1 IN, 0x02 = EP2 OUT …), 0 when unused |
| 4 | 4 | id | request id chosen by the client (unique among outstanding requests). Completions echo it. Device events use 0 |
| 8 | 4 | length | payload bytes following the header |
| 12 | 4 | arg | per type: requested IN length, flags, completion status, event code |

### 5.3 Messages

Client → device:

| type | Name | ep | payload | arg | Device action |
|---|---|---|---|---|---|
| 0x01 | HELLO | 0 | u32 version = 1 | 0 | reply HELLO_ACK |
| 0x02 | RESET | 0 | — | 0 | bus reset (DEVS bit29 → W0C → DEVS bit28 with bit17 = 0 for HS). Reply RESET_DONE. Fails with NODEV if detached |
| 0x03 | CONTROL | 0 | 8-byte SETUP + (if bmRequestType bit7 = 0) wLength bytes of OUT data | 0 | full control transfer through EP0 (§5.5). Reply CONTROL_DONE |
| 0x04 | OUT | OUT ep | data (any length, may be 0) | bit0 = append a ZLP when `length % wMaxPacketSize == 0` and length > 0; bit1 = send a ZLP for length 0 | split into max-packet packets and deliver as the firmware arms the EP. Reply OUT_DONE |
| 0x05 | IN | IN ep | — | max bytes N | collect packets until a short packet/ZLP or N bytes. Reply IN_DONE |
| 0x06 | CANCEL | 0 | — | id to cancel | the named request completes now with CANCELLED (if it already completed, nothing). No own reply |

Device → client:

| type | Name | payload | arg |
|---|---|---|---|
| 0x81 | HELLO_ACK | u32 version, u32 attached (0/1), u32 generation, u32 speed (2 = FS, 3 = HS) | 0 |
| 0x82 | RESET_DONE | u32 speed | status |
| 0x83 | CONTROL_DONE | IN data (≤ wLength) or empty | status |
| 0x84 | OUT_DONE | u32 bytes accepted | status |
| 0x85 | IN_DONE | data (may be empty = ZLP) | status |
| 0x90 | EVENT | u32 generation, u32 speed | event: 1 = ATTACHED (pull-up on with VBUS), 2 = DETACHED (pull-up off, soft reset, VBUS off, or emulator reset) |

Status: 0 OK, 1 STALL, 2 NODEV (detached, or the request belongs to an older generation), 3 CANCELLED, 4 OVERFLOW (a
packet did not fit the IN request), 5 PROTO (malformed or unsupported request).

Rules:
* Requests on **different** endpoints may be outstanding at the same time and complete in any order. On one endpoint,
  requests are served FIFO. A client may keep one IN request pending on an interrupt EP indefinitely.
* `generation` increments on every ATTACHED. Every request issued before a DETACHED completes with NODEV.
* The device never invents transfers. "NAK" is simply a request that stays pending.

### 5.4 Device side: mapping to the controller model

References are to `docs/RE_USB_DEVICE_CONTROLLER.md`.

| Event / message | Model behaviour |
|---|---|
| VBUS on/off (board) | otg+0x10 bit0, GIC 232 pulse (§6). Not part of the protocol |
| DEVC bit16 1 → 0 with MODE bit1 = 1 and VBUS present | send `EVENT ATTACHED(gen+1, speed 3)` (§4.1 step 4). Remember "attached" for HELLO_ACK of later clients |
| DEVC bit16 0 → 1, CONF bit2 soft reset, VBUS off, machine reset | send `EVENT DETACHED`, complete all pending requests with NODEV, drop buffered packets (§4.6) |
| RESET | §4.2: set DEVS bit29 (if DEVC bit29), wait until the firmware clears it, set DEVS bit17 = 0 and bit28. Reply when DEVS bit28 has been cleared by the firmware |
| CONTROL | §4.3: clear EP0 stall, place the SETUP at hdc+0x8140, EPCOUNT0B = 8, DEVS bit27 → IRQ 227. IN: collect EP0 IN packets until short or wLength, then inject the 0-byte status OUT. OUT: feed ≤ 64-byte packets into the EP0 OUT buffer as BUFRD frees it, then take the pre-armed status ZLP. No data: take the pre-armed ZLP. EPCTRL0 bit12 at a data or status point → STALL |
| OUT (bulk/int) | §4.4: one packet at a time into the EP's OUT buffer when it is enabled, not NAK-all, not stalled and empty; PIO or the HDMAC DMA path. Reply OUT_DONE when the last packet (and optional ZLP) has been accepted. EP stalled → STALL |
| IN (bulk/int) | §4.5: take packets that the firmware made ready (BUFWR or DMA), latching status26 per packet. Keep leftover packets of a larger firmware transfer for the next IN request. Complete on a short packet, on a ZLP (returns what was collected so far, possibly empty), or at N bytes. A packet that would exceed N → OVERFLOW |
| CANCEL | remove the request. Packets already taken from the firmware stay buffered for the next request on that EP |
| SET_ADDRESS / SET_CONFIGURATION / CLEAR_FEATURE(HALT) | ordinary CONTROLs. The firmware's standard-request table handles them (FADDR write, function enable, EP reset) |

The model needs no knowledge of MTP or MSC. Packet boundaries carry the protocol semantics.

### 5.5 Notes for a later USB/IP bridge **[suggestion]**
A host-side Python process that speaks A5KU to the emulator and USB/IP (`OP_REP_DEVLIST` from cached descriptors) to
Linux `vhci-hcd`. It must inject `SET_ADDRESS` itself after RESET (vhci never forwards it), turn the
`SET_FEATURE(PORT_RESET)` URB into RESET, and map `CMD_UNLINK` to CANCEL. The emulator does not change.

---------------------------------------------------------------------------------------------------------------------

## 6. Sony-PMCA-RE backend `socket`

### 6.1 Interface it must implement (unchanged commands)

New file `pmca/usb/driver/generic/socket.py`, modelled on `generic/libusb.py`:

```
class _UsbContext(BaseUsbContext)          # pmca/usb/driver/__init__.py
    __init__(name, classType, driverClass)  # name 'socket-MSC' etc.
    __enter__/__exit__                      # share one module-level A5KU connection
    listDevices(vendor) -> iter(UsbDeviceHandle(handle=UsbBackend, idVendor, idProduct))
    openDevice(device) -> driverClass(device.handle)
class MscContext(_UsbContext)              # classType USB_CLASS_MSC (8)
    openDevice: bInterfaceProtocol 0x50 -> MscBbbDriver, 0/1 -> MscCbiDriver   # same rule as libusb.py
class MtpContext(_UsbContext)              # USB_CLASS_PTP (6), MtpDriver
class VendorSpecificContext(_UsbContext)   # USB_CLASS_VENDOR_SPECIFIC (0xFF), GenericUsbDriver

class UsbBackend(BaseUsbBackend)            # pmca/usb/driver/generic/__init__.py
    reset()                                 # NO bus reset: ensure the current generation is enumerated and configured
    clearHalt(ep)                           # CONTROL 02 01 0000 <ep> 0000 (CLEAR_FEATURE ENDPOINT_HALT)
    getId() -> (idVendor, idProduct)        # cached device descriptor
    getEndpoints() -> [obj(bEndpointAddress, bmAttributes, wMaxPacketSize)]   # interface 0, alt 0
    read(ep, length, timeout=None) -> bytes # IN; raise GenericUsbException on STALL/NODEV/timeout(CANCEL)
    write(ep, data)                         # OUT, no ZLP flag; raise GenericUsbException on error
    classInterfaceRequestOut(request, value, index, data=b'')   # bmRequestType 0x21
    vendorRequestOut(request, value, index, data=b'')           # bmRequestType 0x43 (vendor, recipient other)
```

`GenericUsbDriver` picks endpoints with `bmAttributes == 2` exactly, so the endpoint objects must carry the raw
descriptor byte. Errors must be `GenericUsbException`. `MscBbbDriver` relies on it to detect stalls.

Integration (the only edits outside the new file):
* `pmca/commands/usb.py importDriver()`: add
  `elif driverName == 'socket': from ..usb.driver.generic.socket import MscContext, MtpContext, VendorSpecificContext`.
  The libusb fallback lines are then skipped because all three contexts are set.
* `pmca-console.py`: add `'socket'` to `drivers = ['native', 'libusb', 'qemu']`.
* Address: env `A5K_USB=host:port`, default `127.0.0.1:7643`. Timeout: env `A5K_USB_TIMEOUT` ms, default **30000**.
  The libusb backend uses 5000 ms. Emulated MTP operations under TCG can be slower **[?: measure]**.

### 6.2 Backend behaviour
* **Connection:** at the first `__enter__`, connect and send HELLO. If nothing listens, `listDevices` yields nothing (like
  the qemu driver). A reader thread demultiplexes completions by id and records EVENTs.
* **Enumeration (once per generation):** when attached and the generation is new: RESET → `GET_DESCRIPTOR(device, 18)` →
  `SET_ADDRESS(1)` → `GET_DESCRIPTOR(config 0, 9)` → `GET_DESCRIPTOR(config 0, wTotalLength)` → `SET_CONFIGURATION(bConfigurationValue)`.
  Parse interface 0 / alt 0 and its endpoints. This replaces the work of the host OS. It must finish within the firmware's
  60 s window (§1.3).
* **`listDevices(vendor)`:** if attached and enumerated, yield one handle when `idVendor == vendor` and interface 0's class
  equals the context's class. While detached, yield nothing. The wait loops in `installCommand` and
  `firmwareUpdateCommandInternal` call this every 0.5 s and survive exceptions.
* **`read`:** send IN(ep, N). On timeout send CANCEL and raise. A 0-byte result is returned as `b''` (MTP ZLP skipping
  depends on it).
* **Detach during a transfer:** NODEV → `GenericUsbException`. Driver objects of the old generation stay dead. The
  commands create new ones after re-discovery.
* `reset()` is called by every `UsbDevice` constructor. Doing a bus reset there would reset the firmware's function state
  (MTP session) for nothing. Only RESET when the generation changes.

---------------------------------------------------------------------------------------------------------------------

## 7. First milestone end to end: `pmca-console info -d socket` prints the model

| # | Step | Component | State today |
|---|---|---|---|
| 1 | Start the emulator with the cable plugged (`usb=pc`) and the A5KU chardev | board (`sony_a5000.c`): otg+0x10 = 2 at reset, hdc+0x104 = 0 | to do (RE_USB §5 step 1). r1 used gdb |
| 2 | Boot. UsbControl issues otgcore 0x4024E301 / 0x4010E313 / 0xE304; `fusb_start_control` samples VBUS on and posts CID/VBUS events | kernel `fusb_otg`, libusb, libUsbControl | **works** (r1) |
| 3 | `MSGID_VBUS_ON_EVT` 0xA400A broadcast → the app creates `ModelUsbMtp` and `ViewUsbMTP` | libObj (Appframework) | **works** (r1) |
| 4 | `ObjMediaServer` loads the MSC/MTP/PTP servers (insmod `usbg_storage`, `usbg_stillimage`) and the ext-cmd plug-ins | im.elf on-demand loader | **works** (r1) |
| 5 | CONFIGURE_FUNC(2) → START_GADGET(2) → ALL_RECEIVED → `StartGadget(did 11)` → 0xE305 → UDC probe → gadgetcore descriptors → portmon (SDP) | libUsbControl, libusb, kernel | **works** (r1) |
| 6 | Pull-up: DEVC bit16 → 0. libusb starts the 60 s enumeration timer | `f_usb20hdc_udc` + libusb | **works** (r1: DEVC = 0x3B000000) |
| 7 | Device model sees the pull-up edge → `EVENT ATTACHED(gen 1, HS)` (queued until a client says HELLO) | `cxd90014-usbd` + A5KU server | to do (RE_USB_DEVICE_CONTROLLER §4.1, this doc §5.4) |
| 8 | `pmca-console info -d socket`: `importDriver('socket')` → contexts → connect, HELLO / HELLO_ACK(attached) | new backend | to do (§6) |
| 9 | Backend enumerates: RESET (DEVS 29/28), GET_DESCRIPTOR device/config, SET_ADDRESS(1), SET_CONFIGURATION | backend → model EP0 path → firmware standard requests and `usbg_stillimage` | to do. Firmware side expected to work **[?]**. Expect `MSGID_SETCONFIG_EVT` / 0xA4003 in libUsbControl |
| 10 | `listDevices`: interface 0 class 6 → `MtpContext` yields 054C:034A | backend | to do |
| 11 | `MtpDevice`: OpenSession(1), then GetDeviceInfo (bulk OUT EP2 command, bulk IN EP1 data + response; the DMA path, PIO for short packets) | model bulk + HDMAC; firmware `libInfraMtpServer` / `libptp` | to do (model). Firmware **[?]** |
| 12 | `isSonyMtpCamera`: "Sony Corporation", empty extension, ops 0x9280-0x9282 present → `SonyMtpExtCmdDevice` | PMCA | — |
| 13 | `getCameraInfo`: 0x9280 [cmd 1] with an 0x2000-byte write data phase, 0x9281 [cmd 1] read → DevInfoSender returns plist, model name, model code, serial | `libInfraPtpExtCmd` → `libInfraExtCmdSvc` → `libInfraDevInfoSender` (loaded in r1) | firmware **[?]** |
| 14 | `SonyUpdaterCamera.init()` + `getFirmwareVersion()`: cmd 0 packets INIT / QUERY_VERSION → `libupdaterufp` (on-demand load) | camera updater client | **[?]** |
| 15 | Lens (6,2) and GPS (3,1) are optional (errors caught). PMCA prints `Model: ILCE-5000 …` **[?: exact strings]** | PMCA | — |

Exit criterion: step 15 prints the model, product code, serial and firmware version "1.10".
Fallback check if MTP misbehaves: set USB Connection = Mass Storage on the camera. The path becomes MSC 054C:047D +
SCSI 0x7A, which uses only EP1/EP2 bulk and no interrupt EP (RE_USB_DEVICE_CONTROLLER §7).

Later milestones and their extra needs:
* `install`: function 8 / did 58 re-enumeration (pull-up off/on → DETACHED/ATTACHED), Android `ScalarAUsbDlApp`, the HTTPS proxy, a
  valid clock.
* `firmware` / `updatershell`: a software-initiated reboot that the emulator turns into a board reset with the cable kept
  plugged, the updater boot path (RE_UPDATER_BOOT, branch `updater-boot`), and the updater kernel's UDC on the same model.
  The host sees DETACHED, then ATTACHED with 054C:03E2 **[?: how the main firmware's restart request reaches the board]**.

---------------------------------------------------------------------------------------------------------------------

## 8. Open questions

1. What the "Check the connected device." screen in r1 means: the plain waiting view, or a reaction to the 60 s enumeration
   timeout (`proc_host_enum_timeout_cb` → `MSGID_ENUM_TIMEOUT_EVT` 0xA6005 → app). It will resolve itself once a host
   enumerates.
2. Backup ID of the USB Connection setting (this NAND creates `ModelUsbMtp`) and whether `DescriptorMaintainer` can
   override VID/PID from Backup preset data.
3. Firmware behaviour of the MTP function under real traffic (steps 9–14 in §7): only the static and pre-enumeration paths
   are verified.
4. The sender of PARAMID_FUNCTION 4 (did 55, bus-power HID) and when the menu "USB Power Supply" uses it.
5. INQUIRY vendor/product strings of `usbg_storage` (normal MSC and the updater gadget) for `isSonyMscCamera`.
6. How `libupdaterufp`'s restart request after `up.sh user` (osal message 0x920141 {13, 28, 32}) reaches the power
   controller, and what the emulator must do so that it becomes a board reset.
7. The updater kernel's `f_usb20hdc_udc` / `fusb_otg` build: same register usage as the normal kernel? (Not compared.)
8. `ud_timeout_poweroff.elf` timeout in the updater core.
9. Emulated speed of MTP/ext-cmd round trips. It decides the backend's default timeout and whether the 30 s updater
   re-discovery window in `firmwareUpdateCommandInternal` is enough.
10. Why the old run (RE_USB §4.2, build `bab067e`) stalled. The likely cause is the model-switch hang fixed later. A
    bisect run with VBUS forced would confirm it, but it is not needed for the plan.

---------------------------------------------------------------------------------------------------------------------

## 9. Implementation status

* **Emulator:** the A5KU server of §5 is part of `cxd90014-usbhdc` (`hw/usb/cxd90014_usbhdc.c`, controller status in
  `docs/RE_USB_DEVICE_CONTROLLER.md` §9). Chardev property `chardev`, or the machine option `usb-socket=HOST:PORT`
  (creates `tcp:HOST:PORT,server=on,wait=off`). All messages of §5.3 are implemented: HELLO/HELLO_ACK, RESET, CONTROL,
  OUT (ZLP flags), IN (short packet / ZLP / N bytes, OVERFLOW), CANCEL (IN packets already taken are kept for the next
  request on that endpoint), EVENT ATTACHED/DETACHED with the generation counter, NODEV for requests of a detached
  device. EVENTs are sent only after HELLO.
* **Host:** `tools/pmca-socket/socket.py` (the backend of §6) plus `sony-pmca-re-socket.patch` (`importDriver` +
  `'socket'` in the `-d` choices), MIT like Sony-PMCA-RE.

Deviations from §1.4 / §5.1 (by request of the project owner): **the TCP connection is the cable.** A client connecting
= VBUS on (otg+0x10 = 2, GIC 232 pulse); the client going away = VBUS off. There is no `usb=off|pc|charger` property and
no HMP plug command. Without a client the camera sees no cable, exactly as before. The VBUS state survives a machine
reset, so the cable stays plugged across a guest reboot. Backend additions beyond §6.1: `A5K_USB_ATTACH_WAIT` (default
180 s, the first lookup waits for the gadget's pull-up after plugging) and `A5K_USB_REATTACH_WAIT` (default 300 s: after a
DETACHED the next lookup waits for the device to come back, because PMCA's own loops wait only 5 s for `install` and
30 s for `firmware`/`updatershell`).

Results (2026-11-15, firmware 1.10, pristine NAND clone, no SD card, host macOS, Python 3.15):

| Step (§7) | Result |
|---|---|
| 1-6 cable → gadget start → pull-up | **works.** With the camera at the shooting screen, a connecting client gets ATTACHED in about 0.3-3 s (UI shows "USB mode / MTP") |
| 7-10 attach, HELLO, enumeration | **works.** Device descriptor 054C:**08D3** (not 0x034A: the firmware patches it, open question 2), interface 0 class 6, EP 0x81/0x02 bulk 512, EP 0x83 interrupt 32 |
| 11-15 `pmca-console info -d socket` | **works:** "Sony Corporation ILCE-5000 is a camera in MTP mode", Model ILCE-5000, product code, serial, **Firmware version 1.10** |
| `install` (no file) | **works:** MTP → (5,2) → DETACHED / ATTACHED as the app-install device → proxy messages → "Task completed successfully" |
| `updatershell`, main firmware side | INIT, CHK_GUARD, QUERY_VERSION, SWITCH_MODE accepted. The camera shows "장치를 리셋합니다" (the device will be reset) with an OK button (PMCA: "follow the instructions on the camera screen"). After OK the gadget detaches and the UI goes black, but **no board reset follows** (open question 6: the restart request does not reach any modelled reset path; the kernel log shows no restart). An HMP `system_reset` at that point does not help either: the stage-1 loader kernel then panics in `mount_block_root` (warm reset after stage 2 is not supported by the board yet, outside the USB model) |
| `updatershell`, updater side | Cold boot from the same NAND (now holding `/setting/updater/mode`) with a client connected: the updater core's `sauu` starts the gadget **054C:03E2** (MSC, class 8), PMCA finds "Sony DSC ... in updater mode" (INQUIRY over bulk DMA). Started with `-m ILCE-5000` (no DevInfoSender in the updater, PMCA's normal behaviour): INIT, WRITE_FIRM of the FDAT (100 %) and **the updater shell prompt runs**; `help` answered. After `exit` PMCA polls WRITE_FIRM status and the camera keeps answering busy (MSC STALL → CLEAR_FEATURE → sense 9/0x81/0x81, retried); stopped after about 3 min, not investigated |

Open (new):
* Restart request after SWITCH_MODE → board reset (open question 6) and a warm reset from stage 2 back into the stage-1
  loader. Both are needed for `firmware`/`updatershell` without a manual cold boot. The cable model itself already keeps
  VBUS across a reset.
* The WRITE_FIRM busy loop after leaving the updater shell.
* Once in r1 (a run with `-d unimp` and full register tracing), after two plug cycles and a MENU open/close in playback,
  the third plug never produced a pull-up and the screen stayed black. Not reproduced in seven further plug cycles,
  including the same key sequence, without tracing.

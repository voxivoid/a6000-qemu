# Wi-Fi (Atheros AR6003 on SDIO): RE and emulation spec

Scope: what the a5000 (firmware 1.10) Wi-Fi hardware is, how the stock firmware brings it up (kernel modules,
userspace, power/pin sequence, SDIO enumeration, BMI firmware download, HTC/WMI protocol), what the emulator does today
when Wi-Fi is switched on, and a spec for a model that lets the **unmodified** firmware run station mode and AP
(smartphone / Wi-Fi Direct GO) mode with the host as the network. RE only; no emulator code was changed.

Related: `docs/RE_STORAGE_USB.md` §3 (F_SDH30 = SDHCI v3, register usage), §4 (first look at mmcioWrapper),
`docs/HARDWARE.md` (mmc rows), `docs/RE_GUEST_SHELL.md` (shell used for the dynamic run).
`[?]` = not proven. "upstream" = mainline Linux `drivers/net/wireless/ath/ath6kl` of the same era, used only as a
naming aid; every protocol fact used by the spec was checked against the stock driver decompile unless marked `[?]`.

Evidence:
* Static: `/usr/kmod/{ath6kl_sdio,mmcioWrapper,mmc_core,mmc_drv,cfg80211}.ko` (IDA/Hex-Rays decompiles
  `re/ida_stor/stor_*.c`, data dumps `stor_*.data.txt`), kernel `gpio_ioctl` / `hw_gpio_set_mode_port`
  (`re/vmlinux.elf`, IDA), `/usr/lib/libWlanResource.so` (Hex-Rays + objdump for the literal Backup IDs),
  `/usr/bin/{WS_SubProcess.elf, wpa_supplicant, GPIO2SDIO.elf, SDIO2GPIO.elf, compat_firmware.sh}`,
  `/usr/lib/libNetService.so` strings, `/usr/lib/netconf/*.conf`, `/usr/wlan/ath/*`, real-camera Backup values
  (`re/backup/whitelist/backup_props_all.tsv`, parent project, local only).
* Real camera: `re/dmesg_live.txt` (parent project, local only) shows `cfg80211` loaded and the regulatory domain set
  to KR while Wi-Fi was on, i.e. the stack below does come up on the real device.
* Dynamic (emulator, pristine NAND clone, no SD, `-d unimp`, UART2 shell): MENU → Wireless → "WPS Push" (§4).

---------------------------------------------------------------------------------------------------------------------

## 0. Summary

| Question | Answer |
|---|---|
| Chip | **Atheros (Qualcomm) AR6003, target version 0x30000582 = "ar6003 hw 2.1.1"**, 26 MHz reference clock. FullMAC: scan / association / AP beaconing run in the chip's own firmware; the host talks BMI (boot) then HTC/WMI. Module vendor most likely Murata (`MurataTypeTP.bin`, `MurataTypeWN.bin` names in libWlanResource) `[?]`. |
| Bus | **SDIO, 4-bit, on mmc1 = F_SDH30 (SDHCI v3) at 0xF0301000, GIC 221.** Channel from Backup `0x013E000C & 0xF` = 1 (real camera). Card interrupt = SDHCI NORINTSTS bit 8 (CARDINT) on the same IRQ line. SDIO IDs accepted by the driver: manf 0x0271, device 0x0300/0x0301 (AR6003) or 0x0400/0x0401 (AR6004). |
| Power / pins | Board variant "Stamina", Wi-Fi power mode 5 (Backup `0x00A50002` = 5). GPIO p6.3 → high, 1 tick, p6.2 → high, 10 ticks (power rails `[?]`); p3.0–p3.5 switched to the SDIO function; 3 ticks later **p1.4 → high (chip enable / PWD_L `[?]`)**. Pin numbers come from Backup IDs (`0x00F90137/0108`, `0x00F90138/0109`, `0x00F90365/0366`). Verified in the emulator: after the attempt, p6 out = 0x0C, p1 out bit4 = 1, p3 mode bits 0–5 cleared. |
| Driver stack | Userspace (`libWlanResource`) insmods `cfg80211.ko`, `mmc_core.ko`, `mmcioWrapper.ko` (bridges Linux mmc_core to Sony `mmc_drv`), sets `/proc/sys/kernel/hotplug` = `/usr/bin/compat_firmware.sh`, insmods `ath6kl_sdio.ko ath6kl_p2p=1` (QCA "olca 3.4" ath6kl, driver 3.4.0.189), then `iw dev ath0 interface add ath1 type managed` and `iw ath{0,1} set power_save off`. |
| Chip firmware | Loaded by the host over BMI from `/usr/wlan/ath/`: `fw-4.bin` (QCA-ATH6KL IE container: FW_VERSION `3.4.0.189.12.SMARTPHONE`, OTP image 3998 B, firmware image 111944 B, patch 312 B @ 0x57FEC8, VIF_MAX 2, capability bits incl. bit 16) and `bdata.bin` (board data, 1792 B). |
| Network userspace | `WS_SubProcess.elf` starts `wpa_supplicant` (QCA `2.0-devel-3.4.0.171`, `-Dnl80211`) with `/etc/wpa_supplicant_bss.conf` (station, `ath0`) or `/etc/wpa_supplicant_p2p.conf` (P2P GO, SSID postfix `-WCM`, `max_num_sta=1`). Station IP: `pump` DHCP client (`/usr/lib/netconf/pump.conf`, device `ath0`). AP side: `dhcpd` on `ath1`, 10.0.0.1/16, leases 10.0.1.1–10.0.1.254. |
| Today in the emulator | Wi-Fi start powers the GPIOs correctly, loads `cfg80211` and `mmc_core`, then `mmcioWrapper` fails: `mmcio_mount failed!` / `mmcio_wrapper_init failed!` (mmc1 is unimplemented: `mmc_hw_open` polls 0xF030102C for internal-clock-stable ~7900 times and times out). The UI shows **"System Error"** (§4). Nothing else is affected. |
| Recommended model | (B) below: SDHCI at mmc1 + a new **AR6003 SDIO function-1 device** on its sd-bus, with an HLE of the chip firmware at the BMI/HTC/WMI boundary (the chip's Xtensa ROM is not available, so the internal CPU cannot be emulated), backed by a "virtual air" that maps to one QEMU `-netdev`. Station mode: virtual AP(s) bridged to e.g. `-netdev user` (DHCP for `pump`, hostfwd). AP mode: virtual client station that joins the camera's BSS, also bridged to a netdev. WPA2-PSK 4-way handshake done inside the model (keys are only bookkeeping: the data path stays plaintext). |

---------------------------------------------------------------------------------------------------------------------

## 1. Hardware

### 1.1 Chip identification
* `ath6kl_sdio.ko` hw table (`stor_ath6kl_sdio.data.txt` 0x1870C, 21 words per entry) has 0x30000384 "ar6003 hw 2.0",
  **0x30000582 "ar6003 hw 2.1.1"**, 0x30000623 / 0x30000001 (AR6004). The 2.1.1 entry is the only one whose firmware
  directory is `"/"` (Sony-modified; the other entries keep `ath6k/AR6003/hw2.0` etc.) and whose file names exist in
  `/usr/wlan/ath/`, so the camera's chip is the 2.1.1 part. Its fields: dataset patch addr 0x57FF74, app load addr
  0x1234, 0, 0x542330, 0x200, 0, refclk 26 000 000 Hz (0x018CBA80), UART-TX pin 8, testscript addr 0x57EF74, files
  `otp.bin`, `athwlan.bin`, `athtcmd_ram.bin`, `data.patch.bin`, `utf.bin`, `nullTestFlow.bin`, board `bdata.bin`.
* `ath6kl_core_init @ 0x9280` (decompile around line 9310): BMI GET_TARGET_INFO → `ar->target_type = info.type`,
  `ar->version.target_ver = info.version`; the version is looked up in the 4-entry table ("Unsupported hardware
  version: 0x%x" otherwise). **In this driver target_type 3 = AR6003** (host-interest base 0x540600) and 5 = AR6004
  (0x400800) (`ath6kl_configure_target`, lines 8437–8500); this differs from upstream's numbering.
* SDIO IDs (`MODULE_DEVICE_TABLE` aliases in the .ko): `sdio:c*v0271d0300*`, `d0301`, `d0400`, `d0401`.
  `sub_17814` (sdio config) treats `device & 0xFF00 >= 0x300` as "write CCCR 0xF0 = 0x01" (4-bit async IRQ mode).

### 1.2 Bus and interrupt
* mmc1 = F_SDH30 #1, 0xF0301000, GIC 221 (`docs/RE_STORAGE_USB.md` §0/§3). `mmcioWrapper init_module @ 0x4d8` reads
  Backup **0x013E000C** (1 byte); `& 0xF` = channel (0/1/2 = mmc0/1/2, ≥3 rejected). Real camera value 0x01 →
  mmc1. `libWlanResource SetWifiCh/SetWifiType` split the same byte: low nibble = channel 1, high nibble = type 0
  (0x30 would be the external "Wi-Fi accessory" path).
* The card interrupt reaches the host as SDHCI NORINTSTS bit 8: `mmc_hw_irq_handler @ 0x7d08` reads +0x30 and the vendor
  word +0x118, calls the registered SDIO handler when bit 8 is set, then W1C-writes both. The SDIO handler is masked
  and unmasked with NORINTSTSEN/NORINTSIGEN bit 8 (`mmc_hw_lock_card_int` / `mmc_hw_unlock_card_int @ 0x7dd4`,
  `sub_7420(…, 0x100)`), i.e. **level semantics**: the card holds the line until the host has drained the cause.
  `mmc_hw_register_mmcio_handler @ 0x7e04` additionally sets a bit in +0x2C (`sub_73CC(a1, 44, 4)`, likely "keep the
  SD clock on for async card interrupts" `[?]`).
* Card detect for mmc1 (sysctl 0xF2915010 bit 26) is **not** consulted on this path: `mmcio_wrapper_card_present`
  returns 1 and `mmc_card_open` probes by command. The emulator's sysctl value 0x0C010000 (bit 26 = 1) can stay.

### 1.3 GPIO / pin sequence
Kernel `/dev/gpio` ioctl `0xC0104702` (`gpio_ioctl @ 0x40190858`, nr 2 = set mode): argument
`{u32 whole_port; u8 port @+4; u32 bit @+8; u32 mode @+12}`; `whole_port = 0` → `hw_gpio_set_mode_bit`.
`hw_gpio_set_mode_port @ 0x4018fe70` decodes `mode`:

| mode bits | effect (register offsets of the cxd90014-gpio port block) |
|---|---|
| `& 3 == 1` | +0x34 set (the +0x30 "mode" bit = 1: GPIO function) |
| `& 3 == 2` | +0x38 clear (+0x30 bit = 0: peripheral function) |
| `& 0x1C == 4` | input: +0x24 set (input enable), +0x08 clear (direction) |
| `& 0x1C == 8` | output high: +0x28, +0x44 set (latch), +0x04 set (direction) |
| `& 0x1C == 16` | output low: +0x28, +0x48 clear (latch), +0x04 set |

So 17 = GPIO output low, 9 = GPIO output high, 5 = GPIO input, 6 = peripheral + input, 10 = peripheral + output high.
(`docs/HARDWARE.md` lists +0x30 as "mode/pull"; this is the evidence that bit = 1 means GPIO.)

Power mode = Backup `0x00A50002` (1 byte, real camera **5**) → `initializePort_Stamina` / `setPortToStartWlan_Stamina`
(libWlanResource @ 0x3728 / 0x5a24). Port/bit pairs are read with `Backup_read_4Byte` (real values in brackets):

| Step | Pin (Backup port ID / bit ID) | Mode | Then |
|---|---|---|---|
| init (wlanResource init, at Wi-Fi start) | p6.3 (`0x00F90137`=6 / `0x00F90108`=3) | 9 (high) | wait 1000 `[?: units of sub_3388, an osal timer call; likely µs]` |
| | p6.2 (`0x00F90138`=6 / `0x00F90109`=2) | 9 (high) | wait 10000 |
| | p3.4, p3.5, p3.0–p3.3 (WifiCh 1) | 5 (GPIO input, parked) | — |
| start (`setPortToStartWlan_Stamina`) | — | — | wait 10000 + 1000 |
| | p3.4 | 10 (SDIO function, out high) — CLK `[?]` | |
| | p3.5, p3.0, p3.1, p3.2, p3.3 | 6 (SDIO function) — CMD, D0–D3 `[?]` | wait 3000 |
| | **p1.4 (`0x00F90365`=1 / `0x00F90366`=4)** | 9 (high) | → insmod chain (§2.1) |

Power modes 0/1 (`initializePort`) first drive p6.3, p6.2, p1.4 low (mode 17) and then raise p6.3 and p6.2; WifiCh 2
would use p3.16–19, 24, 25. `setPortToStopWlan_Stamina` re-parks p3.0–5 and drops the enables `[?: order not
decoded]`. Interpretation `[?]`: p6.3/p6.2 = module supply rails, p1.4 = AR6003 `CHIP_PWD_L` (reset, active low) —
it is the last line raised before the SDIO bus is used, which matches the AR6003 power-up order (supplies, clocks, then
PWD_L release). `GPIO2SDIO.elf`/`SDIO2GPIO.elf` (p3.8–11, 16, 17 and a /dev/mem write at 0xF3010348/354) belong to
another board variant/slot and are not on this path `[?]`.

### 1.4 Board data / MAC
* `bdata.bin` (1792 B) is the generic board file; the per-unit MAC/calibration live in the chip's OTP (the OTP image
  from `fw-4.bin` is executed with BMI EXECUTE) `[?]`.
* `ath6kl_mangle_mac_address` (always called; `softmac_enable` param) requests `ath6k/AR6003/hw2.1.1/softmac`, which
  does not exist under `/usr/wlan/ath/` → "MAC address file not found" (harmless).
* The address the stack uses is the one reported in **WMI_READY** (§5.6). Sony also keeps a 6-byte MAC in Backup
  `0x00A50000` (read by other code, e.g. "Disp MAC Address" `[?]`). The model reports a configurable MAC (§7.7); never
  hard-code a real device's address.

---------------------------------------------------------------------------------------------------------------------

## 2. Software stack

### 2.1 Bring-up order (libWlanResource, `startWlan @ 0x5e08`)
1. `wifiPowerOn`/`startWlan` → `setPortToStartWlan_Stamina` (§1.3).
2. `loadWlanHostDrivers_olca3_4 @ 0x4668`: `insmod cfg80211.ko` → `mmc_core.ko` → `mmcioWrapper.ko` →
   `echo /usr/bin/compat_firmware.sh > /proc/sys/kernel/hotplug` → `insmod ath6kl_sdio.ko ath6kl_p2p=1`
   (`testmode=1` in the factory test path) → `echo "" > /proc/sys/kernel/hotplug`. Any failure → error → UI
   "System Error" (observed, §4).
3. `initializeWlanTargetBoard_olca3_4 @ 0x4ba8`: `iw dev ath0 interface add ath1 type managed`.
4. `WlanResourceSetMaxPerformanceMode`: `iw ath0 set power_save off`, `iw ath1 set power_save off`.
5. `GetMacAddress`: `SIOCGIFHWADDR` on `ath0`.
(Strings for an older `ar6000.ko`/`sdio_busdriver.ko` stack are dead code: those modules are absent.)

### 2.2 Firmware files (request_firmware → hotplug helper)
`compat_firmware.sh` serves `$FIRMWARE` from `FIRMWARE_DIRS="/usr/wlan/ath/"`. With fw dir `"/"` the driver
(`sub_A37C @ 0xa37c`, `sub_A11C`) requests `//fw-4.bin` (falls back to fw-3/fw-2, then single files `//otp.bin`,
`//athwlan.bin`, …) and the board file `bdata.bin` → `/usr/wlan/ath/bdata.bin`. `fw-4.bin` IEs (parsed here with a
script): FW_VERSION, TIMESTAMP, OTP_IMAGE (3998 B), FW_IMAGE (111944 B), PATCH_IMAGE (312 B), CAPABILITIES (3 bytes:
bits 1,2,4,7,8,9,10,12,15,16), PATCH_ADDR 0x0057FEC8, VIF_MAX 2. **Capability bit 16 changes the WMI CONNECT event
layout** (16-bit IE lengths, §5.6); the model must use that layout because the driver takes the capability from this
host file, not from the chip.

### 2.3 Network userspace
* `WS_SubProcess.elf` (started by `/usr/bin/network.sh`) owns `wpa_supplicant` (`… -D%s -i%s -c %s`, driver
  `nl80211`, control sockets `/tmp_network/run/wpa_supplicant`, `/tmp/wpa_ctrl_*`) and parses its events
  (`CTRL-EVENT-*`, `WPS-*`, `P2P-*`, `AP-STA-*`).
* Station (`bss.conf`): `ap_scan=0`, `disable_scan_offload=1`, WPS PBC/display methods. IP via `pump`
  (`/tmp_network/run/pump.sock`, `pump.conf`: `nogateway`, `ath0 { nodns autoip }`).
* AP / smartphone (`p2p.conf`): P2P GO (`p2p_go_intent=10`, oper class 81 = 2.4 GHz, dynamic channel,
  `max_num_sta=1`, SSID postfix `-WCM`). `libNetService` runs `dhcpd -lf … -pf … -cf /usr/lib/netconf/dhcpd.conf ath1`
  with the camera at **10.0.0.1** (`dhcpd.conf`: 10.0.0.0/16, range 10.0.1.1–254, lease 600 s).
* `ath6kl_p2p=1` + `iw … add ath1` = two vifs on one wiphy (VIF_MAX 2): `ath0` station, `ath1` P2P/AP.

---------------------------------------------------------------------------------------------------------------------

## 3. What the stock driver needs from the hardware

### 3.1 SDIO enumeration by Sony `mmc_drv` (mmcioWrapper → `mmcio_mount(1)` → `mmc_card_open @ 0x6220`)
`mmc_hw_open` (needs a working SDHCI: internal clock stable etc.), then on the card:
1. Clock on, `mmc_io_reset_mmcio_card` (CMD52 write CCCR 0x06 RES) `[?: detail]`, **CMD0**.
2. `mmc_mmcp_detect @ 0xef00`: **CMD8** (arg 0x1AA) and **CMD55/ACMD41** — an SDIO-only card must **not answer** (timeout).
   Any error other than 1030 continues with SDIO detect.
3. `mmc_io_detect @ 0x9e24`: **CMD5** arg 0 → R4 must have OCR bits 23:0 ≠ 0 and `nfunc` (bits 30:28) ≠ 0; then CMD5 with
   the selected voltage until bit 31 (ready) within 1000 ms.
4. **CMD3** (R6, retried until RCA ≠ 0 within 1 s), **CMD7** (select, R1b).
5. `mmc_io_simple_open @ 0x9fe0`: CMD52 reads CCCR 0x08 (card cap; bit 6 LSC → 400 kHz), 0x13 (bit 0 SHS), 0x09–0x0B
   (common CIS pointer).
6. `mmc_io_open @ 0xa0fc`: walks the common CIS with CMD52 until tuple **0x20 (MANFID)**, stores vendor/device, then
   CCCR 0x04 |= 1 (IENM).
7. `optimize_freq_and_width`: CCCR 0x07 bus width = 2 (4-bit) via RMW, CCCR 0x13 bit 1 (EHS) when > 25 MHz.
8. mmcioWrapper: `mmcio_control_func(1,1)` (CCCR 0x02 IOE1, poll 0x03 IOR1 `[?]`), `mmcio_control_irq(1,1)`
   (CCCR 0x04 IEN1), `mmcio_set_buswidth(4)`, `mmcio_request_irq`, `mmc_add_host`.

### 3.2 Linux `mmc_core` view (through mmcioWrapper)
`mmcio_wrapper_request @ 0x1a4` answers **CMD5 itself** with R4 = **0x90FF8000** (ready, 1 function, 2.7–3.6 V) and
passes everything else through: CMD52 → `mmcio_read/write_ioreg` (real CMD52), CMD53 → `mmcio_read/write_iomem` via a
32 KiB bounce buffer (host caps: max block 512, 128 blocks, 0x8000 bytes per request). Other opcodes (CMD3/CMD7 from
mmc_core's own SDIO attach) are completed without touching the card. `set_ios` is a no-op. So Linux re-reads CCCR,
CIS (common + function 1 via FBR1 0x109–0x10B), enables function 1, sets FBR1 block size 0x110/0x111 = **128**
(`sub_17814`), writes **CCCR 0xF0 = 0x01**, then claims the SDIO IRQ. When the IRQ fires, mmc_core reads **CCCR 0x05
(INTx pending)**; bit 1 must be set or the interrupt is treated as spurious.

### 3.3 AR6003 host-interface register space (SDIO function 1, CMD52/CMD53 addresses)
Taken from the stock driver (`ath6kl_sdio_set_mbox_info` values in `ath6kl_sdio_probe` @ ~0x18200,
`sub_16A68` (sync I/O), `ath6kl_sdio_bmi_*`, `ath6kl_sdio_diag_*`, `ath6kl_hif_poll_mboxmsg_rx @ 0x2e0`):

| Address | Width | Name (upstream) | Behaviour the model must provide |
|---|---|---|---|
| 0x400 | 1 | HOST_INT_STATUS | bit 0 = mailbox 0 has an RX message (also bits 1–3 for mbox 1–3, unused); bit 4 counter, bit 6 CPU, bit 7 error `[?]` |
| 0x401–0x403 | 1 each | CPU / ERROR / COUNTER_INT_STATUS | W1C causes; normally 0 |
| 0x404 | 1 | MBOX_FRAME | 0 |
| 0x405 | 1 | RX_LOOKAHEAD_VALID | bit 0 = a message is waiting in mbox 0 (used by BMI read and HTC polling) |
| 0x408–0x40B | 4 | RX_LOOKAHEAD0 | first 4 bytes (HTC header: eid, flags, len16) of the waiting message |
| 0x400–0x417 | 24 | IRQ register table | read in one CMD53 (24 bytes in poll mode, `ath6kl_hif_poll_mboxmsg_rx`; ISR reads the same block `[?: exact size]`) |
| 0x418–0x41B | 4 | INT_STATUS_ENABLE, CPU_/ERROR_/COUNTER_INT_STATUS_ENABLE | written by `ath6kl_hif_unmask_intrs`; mbox-0 data enable gates the card IRQ `[?: bit layout per upstream: 0x418 bit 0 mbox0]` |
| 0x420–0x43F | 4 each | COUNT[8] | credit counters (read) |
| 0x440–0x45F | 4 each | COUNT_DEC[8] | read returns the counter and decrements it. **BMI polls 0x450 (counter 4)** until non-zero (1 s timeout, "bmi communication timeout") |
| 0x474 | 4 | WINDOW_DATA | diag data |
| 0x478 | 4 | WINDOW_WRITE_ADDR | writing it (address in LSB-last order per upstream `[?]`) stores WINDOW_DATA to target memory |
| 0x47C | 4 | WINDOW_READ_ADDR | writing it loads target memory into WINDOW_DATA |
| 0x800–0x1FFF | — | MBOX0..3 (0x800 each) | **host writes are shifted so they end on the last byte of the mailbox** (`sub_16A68`: `addr += 0x800 - len` for 0x800 ≤ addr < 0x2000); a write that covers the mailbox's last byte ends the message (EOM). Reads (fixed or incrementing address, block or byte mode) pop bytes from the target→host message at the head of mbox 0 |
| 0x4000–0x6FFF | — | MBOX0 extended window (0x3000) | same as mbox 0; writes start at 0x4000 and are moved to end at 0x7000 (`addr == 0x4000 → 0x7000 - len`) |
| 0x7000–0xAFFF | — | GMBOX (0x4000) | unused (no Bluetooth over GMBOX on this camera `[?]`) |

Block size 128 (`mbox_info.block_size`); HTC messages are padded to 128 bytes; the host reads exactly the padded length.

### 3.4 BMI phase (before firmware runs)
Command words are little-endian u32, written to mailbox 0 after a credit check (0x450); replies are read from mailbox 0
after RX_LOOKAHEAD_VALID bit 0 (`ath6kl_sdio_bmi_read`, skipped for replies ≤ 3 bytes). IDs per upstream, confirmed
by the stock call sites:

| ID | Command | Payload → reply |
|---|---|---|
| 1 | DONE | — → none; the "firmware" starts (§3.5) |
| 2 | READ_MEMORY | addr, len → len bytes |
| 3 | WRITE_MEMORY | addr, len, data → none |
| 4 | EXECUTE | addr, param → u32 result |
| 5 | SET_APP_START | addr → none |
| 6 | READ_SOC_REGISTER | addr → u32 |
| 7 | WRITE_SOC_REGISTER | addr, value → none |
| 8 | GET_TARGET_INFO | — → **0xFFFFFFFF, 0x0000000C, version 0x30000582, type 3** (`ath6kl_bmi_get_target_info @ 0x404c`: sentinel path, byte count must be 12) |
| 13 | LZ_STREAM_START | addr → none |
| 14 | LZ_DATA | len, data → none |

Upload sequence for this chip (`sub_AC68 @ 0xac68` = init_upload + hw start), all addresses must just be accepted and
stored (sparse target RAM, `addr & 0x1FFFFF` aliasing, see below): `ath6kl_configure_target` writes host-interest items
(0x540600 hi_app_host_interest = 2, 0x540614 UART debug, option flags with read-modify-write, …); SOC register RMW at
0x40C4 (SYSTEM_SLEEP), 0x180C0, writes 0x4020 = 1, 0x40E0 = 0x100000, the "temporary war to avoid sdio crc error" block
0x1404C–0x1405C; read `hi_board_data` → board file write (1792 B), `hi_board_data_initialized`; OTP via fast download
(LZ stream) to 0x1234 + EXECUTE; firmware via fast download + SET_APP_START; patch (312 B) to the patch address +
`hi_dset_list_head`; restore 0x40C4 / 0x180C0; DONE. The model may discard LZ data and images (§6 explains why it
cannot run them).
Memory model: BMI uses target virtual addresses (0x54xxxx), the diag window uses physical ones: after DONE the driver
diag-reads **0x140600** and masks with 0x1FFFFF (`sub_AC68`, "unable to set the host app area"). Back target memory
with one sparse store indexed by `addr & 0x1FFFFF`. Initial content: zero, except `hi_board_data` (0x540654 per
upstream offset `[?]`) = a RAM address such as 0x542400 `[?: real ROM value unknown]`.

### 3.5 HTC phase
`ath6kl_htc_wait_target @ 0x382c` polls (no IRQ yet) for the first message on mbox 0. HTC frame header (6 bytes):
`eid u8, flags u8, payload_len le16, ctrl0 u8, ctrl1 u8`; flags bit 1 = trailer present, `ctrl0` = trailer length.

| Direction | EP | Message | Layout / requirement |
|---|---|---|---|
| T→H | 0 | READY (id 1) | `u16 id=1, u16 credit_count≠0, u16 credit_size≠0, u8 max_ep, u8 pad, u8 htc_version, u8 max_msg_per_bundle` (length > 7; > 9 reads byte 8). Suggest credits 24 × 1664 B `[?]`, bundle 0 (no bundling) |
| H→T | 0 | CONN_SVC (id 2) | `u16 id, u16 svc_id, u16 flags, u8 meta_len, u8 pad`. Services: 0x100 WMI control, 0x101 BE, 0x102 BK, 0x103 VI, 0x104 VO ("WMI CONTROL", "WMI DATA BE/BK/VI/VO" in `sub_AC68`) |
| T→H | 0 | CONN_SVC_RESP (id 3) | `u16 id=3, u16 svc_id, u8 status=0, u8 eid (1..5), u16 max_msg_size (≥1536+headers), u8 meta_len=0, u8 pad` |
| H→T | 0 | SETUP_COMPLETE(_EX) (id 4/5) | ack only |
| T→H | any | credit report | trailer record `{u8 rec_id=1 (CREDITS), u8 len, {u8 eid, u8 credits}…}`; return one credit per consumed host message, at least when the host sets flags bit 0 (NEED_CREDIT_UPDATE) `[?]` |

After that the host enables interrupts (§3.3) and all target→host traffic is IRQ driven: set HOST_INT_STATUS bit 0 /
lookahead, assert the card IRQ (CCCR 0x05 bit 1, SDHCI CARDINT) while a message is pending and enabled.

### 3.6 WMI
WMI control frames on the control EP: header `u16 cmd_id, u16 info1 (bits 3:0 = vif index), u16 reserved`
(`ath6kl_wmi_control_rx @ 0x14eec`). Data frames on the data EPs: 6-byte `wmi_data_hdr` (`s8 rssi, u8 info, u16 info2,
u16 info3`): info bits 7:6 = frame type (0 = 802.3, 1 = 802.11), bits 4:2 = user priority; info2 bits 15:13 = meta
version (tx meta present); info3 bits 3:0 = vif index (`ath6kl_rx`). RX type 0 frames are **802.3 + LLC/SNAP**
(`ath6kl_wmi_dot3_2_dix`); TX frames are converted DIX → 802.3/LLC by `ath6kl_wmi_dix_2_dot3` and carry an optional tx
meta block per info2 `[?: sizes, read ath6kl_wmi_data_hdr_add]`.

Events the model must generate (IDs from `ath6kl_wmi_control_rx` comparisons):

| ID | Event | Layout used by the stock driver |
|---|---|---|
| 0x1001 | READY | `u32 sw_version, u32 abi_version (**must be 1**, "abi version mismatch"), u8 mac[6], u8 phy_cap` (len > 14). Must arrive within 200 jiffies of HTC start ("wmi is not ready") |
| 0x1002 | CONNECT | 16-byte union + **`u16 beacon_ie_len, u16 assoc_req_len, u16 assoc_resp_len`** (capability bit 16 set) + IEs (len > 22). Station: `u16 ch(MHz), u8 bssid[6], u16 listen_intvl, u16 beacon_intvl, u32 nw_type`. AP own BSS: `u16 ch, u8 bssid[6] = vif MAC`. AP client: `u8 phymode, u8 aid, u8 mac[6], u8 auth, u8 keymgmt, u16 cipher, u8 apsd_info, u8 unused[3]` `[?: union per upstream]`; assoc_req IEs must contain the client's RSN IE |
| 0x1003 | DISCONNECT | `u16 proto_reason, u8 bssid[6], u8 disconn_reason, u8 assoc_resp_len, u8 assoc_info[]` (len > 10) |
| 0x1004 | BSSINFO | `u16 ch(MHz), u8 frame_type (1 beacon / 2 probe resp), u8 snr (RSSI = snr − 95 dBm), u8 bssid[6], u16 ie_mask` + frame body from the timestamp on (timestamp, beacon interval, capability, IEs) |
| 0x100A | SCAN_COMPLETE | `u32 status` (0) |
| 0x100B | REPORT_STATISTICS | optional (station info) |
| 0x1006 | REGDOMAIN | optional (`u32 reg code`) |
| 0x9008 | CANCEL_REMAIN_ON_CHNL | `u32 freq, u32 duration, u8 status` `[?]` → `cfg80211_remain_on_channel_expired` |
| 0x9009 | TX_STATUS | `u32 id, u8 ack` `[?]` → `cfg80211_mgmt_tx_status` |
| 0x900A | RX_PROBE_REQ | P2P probe request report |

Commands the model must act on (IDs from the `ath6kl_wmi_*_cmd` call sites); all others are accepted silently
(credits returned):

| ID | Command | Model action |
|---|---|---|
| 1 / 2 / 3 | CONNECT / RECONNECT / DISCONNECT | join a virtual AP → CONNECT (+ EAPOL msg 1 if WPA2) / DISCONNECT |
| 7, 0xF08B | START_SCAN, BEGIN_SCAN | one BSSINFO per virtual AP on the requested channels, then SCAN_COMPLETE |
| 8, 9, 10, 11, 13, 17, 18, 61, 0xF024 … | scan params, bss filter, probed SSID, listen interval, disc timeout, channel params, power mode, keepalive, rx frame format | store / ignore |
| 22 / 23 | ADD_CIPHER_KEY / DELETE_CIPHER_KEY | record keys (no crypto on the data path) |
| 63, 74 | SET_APPIE, SET_IE | store beacon / probe-response / assoc IEs (AP mode, P2P) |
| 0xF00F | AP_CONFIG_COMMIT | start the camera's BSS → CONNECT (own BSS); optionally schedule the virtual client (§7.5) |
| 0xF010 | AP_SET_MLME | deauth/disassoc a client → DISCONNECT |
| 0xF00B, 0xF011 | AP hidden SSID, set PVB | ignore |
| 0xF067 / 0xF068 `[?]` | REMAIN_ON_CHNL / cancel | reply with remain-on-channel start `[?: event ID]`, then 0x9008 after the duration |
| 0xF069, 0xF08A | SEND_ACTION, SEND_MGMT | TX_STATUS ack = 1 (no peer) |
| 0xF06A, 0xF06C, 0xF06D | probe report req, probe response, info req | ignore |
| 0xF08F | SET_REGDOMAIN | ignore (cfg80211 already sets KR, `re/dmesg_live.txt`) |

---------------------------------------------------------------------------------------------------------------------

## 4. What happens today (emulator)

Run: `qemu-system-arm` at the shared HEAD binary, pristine NAND, no SD, `-display none`, UART2 shell, `-d unimp`.
MENU → 무선 (Wireless) page 1: "Send to Smartphone" (enabled at first, refused with "This function is not available
now" without media), "Send to Computer" and "View on TV" greyed, One-touch (NFC), Airplane Mode (Off), **WPS Push**.
`WPS Push` + Enter (≈ 330 s uptime):
* GPIO state afterwards (`xp`): p6 dir/out = 0x0C (p6.2, p6.3 high), p1 out bit 4 = 1, p3 +0x30 bits 0–5 cleared
  (SDIO function) and p3.4 output — the whole §1.3 sequence ran.
* `/proc/modules`: `cfg80211`, `mmc_core` loaded; `mmcioWrapper` and `ath6kl_sdio` not.
* Kernel log (Sony klog ring, `tools/klog.py`): `mmcio_mount failed!`, `mmcio_wrapper_init failed!`.
* `-d unimp`: mmc1 vendor write 0xF03011FC, clock-gate RMW 0xF2A30070 = 0xB00 (slot 1), pad writes
  0xF2912348/354 = 0x2F, 0xF2912344 = 0x10, 0xF2912548/554 = 0x4000, then `wr2 0xF030102C = 1` followed by ~7950 reads
  of 0xF030102C (internal clock stable never set) → `mmc_hw_open` times out.
* Screen: full-screen **"시스템 에러" (System Error)**. The guest stays up (shell works).
With a plain `generic-sdhci` at mmc1 and no card the expected result is the same failure one step later (CMD5
timeout in `mmc_io_detect`) `[?: not run]`.

---------------------------------------------------------------------------------------------------------------------

## 5. Requirements in one list (what a model must satisfy, in order)
1. SDHCI v3 at 0xF0301000 / GIC 221 with the F_SDH30 vendor window 0x100–0x1FF (same as mmc0), clock-stable.
2. Card power follows GPIO p1.4 (and p6.2/p6.3): no response to any command while low; full reset on a rising edge.
3. SDIO card: no reply to CMD8/CMD55/ACMD41/CMD1; CMD5 R4; CMD3 R6 (RCA ≠ 0); CMD7; CMD52 to CCCR/FBR/CIS and to
   function-1 registers; CMD53 byte and block mode (block size 128 for function 1), incrementing and fixed address.
4. CCCR: 0x00 = 0x32 (SDIO 2.00, CCCR 2.00) `[?]`, 0x02/0x03 IOE/IOR (IOR follows IOE), 0x04 IEN, 0x05 INT pending,
   0x06 RES, 0x07 bus width, 0x08 card cap (no LSC, set SMB/SRW/S4MI as on AR6003 `[?]`), 0x09–0x0B CIS ptr, 0x10/0x11
   func-0 block size, 0x12 power control, 0x13 high speed (SHS = 1), 0xF0 vendor (accept). FBR1 0x100–0x111 with CIS
   ptr and block size. CIS: CISTPL_MANFID (0x20) `71 02 01 03` (0x0271 / 0x0301 `[?: 0x0300 or 0x0301 on the real part]`),
   CISTPL_FUNCID (0x21) 0x0C, CISTPL_FUNCE (0x22) for function 0 (max block 128 `[?]`, max speed 50 MHz) and function 1,
   CISTPL_END.
5. Card interrupt: level, = `(host_int_status & int_status_enable) != 0 && IEN1 && IENM`; visible in CCCR 0x05 bit 1 and
   as SDHCI CARDINT.
6. Function-1 register space §3.3, BMI §3.4, HTC §3.5, WMI §3.6.

---------------------------------------------------------------------------------------------------------------------

## 6. Emulation options (ranked by fidelity, then effort)

| # | Option | Fidelity | Effort | Verdict |
|---|---|---|---|---|
| A | Full chip LLE: model the AR6003 SoC (Xtensa CPU, ROM, MAC/PHY, radio) and run the downloaded `athwlan.bin`/OTP | highest | impossible today: the chip's mask ROM (BMI loader, ROM patches the RAM image calls into) is not in the firmware package; no Xtensa AR6003 platform model exists | rejected |
| **B** | **SDIO card + HLE of the chip firmware at the BMI/HTC/WMI boundary** (the stock kernel driver, mmc stack, wpa_supplicant, dhcpd/pump all run unmodified), backed by a virtual radio environment mapped onto a QEMU netdev | stock software end to end; only the chip's internal firmware is replaced | large but bounded: SDIO card device (~600 lines), sdhci card-IRQ hook (small upstream-style patch), HIF/mailbox + BMI (~400), HTC + credits (~300), WMI subset + air/netdev bridge (~800), WPA2-PSK 4-way both roles (~500, QEMU crypto) | **recommended** (AI_WORKFLOW §1.5: HLE allowed because the original chip code (ROM) is unavailable; the boundary is the SDIO register/mailbox protocol and is reproduced byte for byte) |
| C | Replace the SDIO bus with a paravirtual interface (virtio-net, or a fake `ath0` from a host-side module) | low: needs a new kernel driver or a modified NAND (forbidden by AI_WORKFLOW §1.2); the stock `startWlan` would still fail at `mmcioWrapper` | small | rejected |
| D | Model only up to SDIO enumeration (card answers CMD5/52/CIS but no BMI) | none for networking; turns "System Error" into an ath6kl probe failure | small | only as milestone 1 of B |

Within B there is one design choice: where the 802.11 "air" ends. B1 (recommended): the model is the radio *and* the
peers (virtual APs, virtual client); only Ethernet frames leave it → any QEMU netdev works on macOS and Linux
(`user`, `socket`, `stream`, `vmnet-*`, `tap`). B2 (later): export real 802.11 frames to a host 802.11 simulator
(Linux `mac80211_hwsim` + `wmediumd`) so a real host `wpa_supplicant`/`hostapd` is the peer — more faithful for WPS/P2P
negotiation but Linux-only and needs an 802.11 MAC layer in the model; out of scope for the first version.

---------------------------------------------------------------------------------------------------------------------

## 7. Spec for option B (code agent)

### 7.1 Files / QOM types
| File | Content |
|---|---|
| `hw/arm/sony_a5000.c` | a second `generic-sdhci` (same properties as mmc0: spec 3, no UHS, `capareg 0x017834b4`) at **0xF0301000**, IRQ GIC **221**, with the F_SDH30 vendor window 0x100–0x1FF handled like mmc0; create `ar6003-sdio` on its sd-bus when `-M …,wifi=on` (default **on** = stock hardware; `wifi=off` leaves the bus empty). Wire GPIO named outputs p1.4, p6.2, p6.3 (`"out"` lines 1*32+4, 6*32+2, 6*32+3) to the card's `pwd`/`vdd` inputs. Also map mmc2 0xF0302000/GIC 224 as an empty SDHCI (RE_STORAGE_USB §7 P2) |
| `hw/sd/sdhci.c` (+`sdhci-internal.h`) | card interrupt: new `SDBusClass` hook (e.g. `set_card_irq(bus, level)`) or a qdev GPIO input `"card-irq"`; while asserted, NORINTSTS bit 8 reads 1 and the IRQ is raised when NORINTSTSEN/NORINTSIGEN bit 8 are set; clearing the enable masks it (level, re-evaluated on every enable write). Keep upstream behaviour when nothing drives it |
| `hw/sd/ar6003_sdio.c` (new) | the card: SD protocol + CCCR/FBR/CIS + function-1 HIF registers + mailboxes + IRQ (§7.2–7.3) |
| `hw/net/ar6003_fw.c` (new, or same file) | firmware HLE: BMI, HTC, WMI, virtual air, netdev bridge, WPA2 (§7.4–7.6) |
| `hw/sd/trace-events`, `hw/net/trace-events` | `ar6003_cmd`, `ar6003_cmd52`, `ar6003_cmd53`, `ar6003_bmi`, `ar6003_htc_rx/tx`, `ar6003_wmi_cmd/evt`, `ar6003_irq` |
| `Kconfig`, `meson.build` | `AR6003_SDIO` selected by `SONY_A5000` |

The card should subclass `TYPE_SD_CARD`'s class interface (`SDCardClass`: `do_command`, `read_byte`, `write_byte`,
`data_ready`, `receive_ready`, `get_inserted`) the way `sd-card-spi` does, or be its own `TYPE_SD_CARD`-compatible type
that `sdbus_get_card()` accepts `[?: check sd.c/core.c casts in QEMU 10.1]`.

### 7.2 SD/SDIO protocol
* Power: `pwd` low or `vdd` low → card absent for commands (no response = SDHCI command timeout). Rising `pwd` (with vdd
  high) → reset all state: CCCR defaults, RCA 0, BMI state, mailboxes empty, credits = 1 on COUNT[4].
* CMD0: reset to idle (keep power). CMD5: R4 `0x80000000 | (1 << 28) | 0x00FF8000` once the arg's voltage is non-zero
  (arg 0 → same value with bit 31 set is acceptable; mmc_drv only loops until bit 31). CMD3: R6 with RCA 0x0001.
  CMD7: R1b. CMD8, CMD55, ACMD41, CMD1, CMD2, CMD9: **no response**. CMD52: R5 = `flags (0x10 = state CMD/0x20 TRN) << 8 |
  data`; RAW (bit 27) returns the new value. CMD53: R5, then data: byte mode count = arg[8:0] (0 = 512), block mode
  blocks of the function's block size; OP code bit 26 = incrementing address.
* Function 0 (CCCR/FBR/CIS) and function 1 (§7.3). Unknown addresses: read 0, log `LOG_UNIMP`.

### 7.3 Function 1 (HIF)
* Register file 0x400–0x4FF per §3.3 (COUNT_DEC reads decrement; window registers perform target-memory access when
  the address register's last byte is written `[?: upstream writes the address with a 4-byte CMD53, LSB at the lowest
  address; trigger on any write to 0x478/0x47C]`).
* Mailbox 0 TX (host→target): accumulate bytes written in 0x800–0xFFF or 0x4000–0x6FFF; a write touching 0xFFF or 0x6FFF
  completes the message → hand it to BMI (before DONE) or HTC (after DONE). Mailboxes 1–3: accept and log.
* Mailbox 0 RX (target→host): FIFO of messages; each HTC message is exposed padded to 128 bytes; reads anywhere in the
  mailbox ranges consume bytes; when a message is fully consumed, load the next one into RX_LOOKAHEAD0 and keep
  RX_LOOKAHEAD_VALID/HOST_INT_STATUS bit 0 set while the FIFO is non-empty. BMI replies are raw (no HTC header, no
  padding: the host reads the exact reply length).
* IRQ = `(HOST_INT_STATUS & INT_STATUS_ENABLE) != 0` (after BMI DONE only; BMI is polled), gated by CCCR IEN1/IENM,
  forwarded to the sdhci card-irq hook; CCCR 0x05 bit 1 mirrors it.

### 7.4 BMI and HTC (HLE of the boot ROM / firmware start)
* BMI as §3.4, sparse target memory (`GHashTable` of 4 KiB pages keyed by `(addr & 0x1FFFFF) >> 12` is enough;
  firmware images are ~110 KiB). LZ_DATA and EXECUTE just log size/address (EXECUTE returns 0). Keep a log line with
  the FW_VERSION-like identifiers seen (e.g. the image length) for tests.
* On DONE: after ~10 ms (virtual clock) enqueue HTC READY (§3.5). Answer CONN_SVC for 0x100..0x104 with EIDs 1..5.
  After SETUP_COMPLETE send WMI READY (abi 1, sw_version e.g. 0x03040000 + build `[?]`, MAC = machine property,
  phy_cap 0x00/11g `[?]`) and optionally REGDOMAIN.
* Credits: start with READY's credit_count; every host message consumes one; return them in a credit-report trailer
  on the next T→H message, or in an empty EP0 message when the host asks (flags bit 0) or after 1 ms idle.

### 7.5 Virtual air
Machine/device properties (string syntax illustrative):
* `wifi-mac=02:xx:…` (default: a locally administered address derived from a fixed seed; never a real device's MAC).
* `wifi-ap=ssid=<s>[,psk=<p>][,ch=<n>][,bssid=<mac>]` (repeatable or `;`-separated), default one open AP
  `ssid=QEMU-AP,ch=6`. Each virtual AP is reported in scans (BSSINFO with SSID, supported rates 1–54, DS param, RSN IE
  for WPA2-PSK/CCMP, beacon interval 100) and accepts CONNECT if SSID (and BSSID if given) match.
* `wifi-client=auto|off` and `wifi-client-psk=<p>` (or an HMP/QMP command `wifi_client_join <ssid> <psk>`): after
  AP_CONFIG_COMMIT, a virtual client with its own MAC associates (CONNECT ap_sta with an assoc request carrying an RSN
  IE) and runs the supplicant side of the 4-way handshake with the PSK. The camera generates its own P2P passphrase and
  shows it on screen, so the test passes it in by command after reading the screen or the guest's wpa_supplicant
  state `[?: whether Sony's GO is autonomous (p2p_group_add) — the model does not depend on it]`.
* `netdev=<id>`: one QEMU netdev for the whole air. Frames from the camera (WMI data, any vif) are converted
  802.3/LLC → Ethernet and sent; frames from the netdev are delivered to the vif that is connected (station: if the
  destination is the camera MAC, broadcast or multicast; AP: if the destination is the camera or broadcast — the
  virtual client's MAC is the source the netdev uses). EAPOL (ethertype 0x888E) is consumed by the model while a
  handshake is in progress and never forwarded.

### 7.6 WPA2-PSK
PMK = PBKDF2-SHA1(passphrase, SSID, 4096, 32); PTK = PRF-384(PMK, "Pairwise key expansion", min/max(AA,SPA),
min/max(ANonce,SNonce)); EAPOL-Key MIC = HMAC-SHA1-128 (key descriptor version 2, AES-CCMP); GTK in msg 3 wrapped with
AES key wrap (RFC 3394) using KEK. Use `crypto/` (`qcrypto_pbkdf2`, `qcrypto_hmac`, `qcrypto_cipher` AES-128-ECB for key
wrap). Station mode: the model is the authenticator (msgs 1 and 3); AP mode: the model is the supplicant (msgs 2 and 4).
Data frames stay plaintext on both sides of the model (the real chip encrypts in hardware after ADD_CIPHER_KEY).
Open networks skip this.

### 7.7 Defaults that keep the stock behaviour
`wifi=on` with no `netdev` and the default AP: the stock UI can enable Wi-Fi, scan and see `QEMU-AP`, but no traffic
leaves the VM (frames dropped, counted in a trace event). Nothing on the boot path touches mmc1 (§1.2,
RE_STORAGE_USB §4), so boot is unchanged.

---------------------------------------------------------------------------------------------------------------------

## 8. Acceptance tests
Each with a pristine NAND clone, UART2 shell, `-d unimp,guest_errors` clean for mmc1 except the vendor window.
1. **Bring-up**: MENU → Wireless → WPS Push (or Access Point Settings on page 2). No "System Error"; klog has no
   `mmcio_*failed`; `/proc/modules` lists `cfg80211, mmc_core, mmcioWrapper, ath6kl_sdio`; klog shows
   `ar6003 hw 2.1.1 sdio fw … api 4` (from the `"%s %s fw %s api %d%s"` print in `sub_AC68`); `ifconfig -a` / `ls
   /sys/class/net` shows `ath0` and `ath1`; `GetMacAddress` returns the `wifi-mac` value.
2. **Scan**: Wireless → Access Point Settings → the list contains the `wifi-ap` SSIDs with the configured security.
3. **Station + DHCP + host TCP**: `-netdev user,id=w0,net=192.168.77.0/24,hostfwd=tcp:127.0.0.1:46508-:<port>` +
   `wifi-ap=ssid=TestAP,psk=testpass123`, select TestAP, enter the password in the stock keyboard → wpa_supplicant
   `CTRL-EVENT-CONNECTED`; `pump` obtains 192.168.77.x (`ifconfig ath0`); from the guest shell
   `busybox wget http://192.168.77.2:<host port>/` (slirp gateway → host) succeeds; open variant without a password too.
4. **Send to Computer / PC Remote** (needs an SD card image with a JPEG, `A5K_SD`): with test 3's AP the camera reaches a
   host-side service — smoke test only `[?: which protocol; optional]`.
5. **AP / smartphone**: SD image with one JPEG; Wireless → Send to Smartphone → the screen shows SSID `DIRECT-xxxx:…`
   and a password; `ath1` up at 10.0.0.1, `dhcpd` running. Then `wifi_client_join <ssid> <password>` (or
   `wifi-client=auto` + the password command) → the camera screen changes to "connecting/transferring" (AP-STA-CONNECTED in
   WS_SubProcess logs), dhcpd hands out 10.0.1.x to the virtual client; with `-netdev user,id=w1,net=10.0.0.0/16,
   host=10.0.1.200,hostfwd=tcp:127.0.0.1:46509-10.0.0.1:<camera port>` the host connects to a camera TCP port (e.g. the
   PlayMemories HTTP server `[?: port]`) and receives an HTTP response.
6. **Off/on cycle**: Airplane mode on/off and leaving the Wi-Fi screen unload the modules (`rmmod ath6kl_sdio,
   mmcioWrapper, mmc_core, cfg80211`), drop p1.4, and a second start works (card reset on the p1.4 edge).
7. **No regression**: boot to the shooting screen, MENU/PLAY, SD media unchanged with `wifi=on` and with `wifi=off`.

---------------------------------------------------------------------------------------------------------------------

## 9. Open questions
* Exact AR6003 SDIO device ID (0x0300 vs 0x0301) and CCCR/CIS contents of the real module — readable on the real camera
  from `/sys/bus/sdio/devices/*/{vendor,device}` while Wi-Fi is on (read-only, owner's call).
* Units of `sub_3388` waits (osal sync message 40) — only matters for timing fidelity.
* Real `hi_board_data` value after the ROM boots; CONNECT union layouts for AP mode and the remain-on-channel event ID
  (verify in `ath6kl_connect_event`/`sub_111B4` and the 0x9000-range branches of `ath6kl_wmi_control_rx`).
* Whether Sony's smartphone mode uses an autonomous GO or full P2P group negotiation (`WS_SubProcess` P2P command
  sequence); the model only supports the GO-side association in v1.
* HIF interrupt-enable bit layout (0x418–0x41B) and the ISR read size — take from `ath6kl_hif_unmask_intrs` /
  `ath6kl_hif_intr_bh_handler` decompiles when implementing.

## 10. NFC notes (handed over)
NFC is owned by `docs/RE_NFC.md`. Only incidental observations: `nfc.ko` is loaded by `/sbin/init` at boot (unlike
Wi-Fi), the running system has threads `nfc_rsrc_event_` and IRQ threads `irq/125-nfc_rsr`, `irq/129-nfc_rsr` (GPIO pin
interrupts, p0.4 / p0.8 by the gpiopin_to_irq mapping), the real-camera log has `UDIF: suspend skipped: nfc_rsrc`, and
the Wireless menu's "One-touch (NFC)" item is selectable in the emulator. Not investigated further.

---------------------------------------------------------------------------------------------------------------------

## 11. Implementation notes (code agent, QEMU branch `wifi`)

Option B of §6 as specified in §7, with these files:

| File | Content |
|---|---|
| `hw/sd/ar6003_sdio.c` | `ar6003-sdio`: SD protocol (CMD0/3/5/7/52/53, silent for CMD8/55/41/1/2/9), CCCR, FBR1, common + function-1 CIS, function-1 HIF registers 0x400–0x47F, mailbox 0 TX/RX, card interrupt, power inputs `pwd` (p1.4) / `vdd[0..1]` (p6.2, p6.3) |
| `hw/net/ar6003_fw.c` | firmware HLE: BMI (sparse target RAM), HTC (READY, CONN_SVC, SETUP_COMPLETE, credit trailers), WMI READY + the commands below, virtual APs, virtual client, WPA2-PSK 4-way (both roles), netdev bridge |
| `include/hw/net/ar6003.h` | shared state |
| `hw/sd/core.c`, `include/hw/sd/sd.h` | `SDBusClass.set_card_irq` + `sdbus_set_card_irq()`; the bus looks up `SDCardClass` with `object_get_class()` so a card need not be a `TYPE_SDMMC_COMMON` (SDState is private to `sd.c`) |
| `hw/sd/sdhci.c`, `include/hw/sd/sdhci.h` | card interrupt: NORINTSTS bit 8 follows the card's line while NORINTSTSEN bit 8 is set (level, not W1C), re-evaluated in every `sdhci_update_irq` |
| `hw/arm/sony_a5000.c` | `a5k_wifi_init()`: second `generic-sdhci` at 0xF0301000 / GIC 221, `ar6003-sdio` child `/machine/ar6003` on its bus, GPIO p1.4 / p6.2 / p6.3 wired; machine option `wifi=on|off` (default on). mmc2 is still not mapped |

Values chosen where the spec left them open (all `[?]`, none read from a real module):
CCCR 0x00 = 0x32, 0x08 = 0x12 (SMB, S4MI), 0x13 SHS = 1; FBR1 0x100 = 0x07; CIS MANFID 0x0271/0x0301, function-1 FUNCE
42 bytes (max block 512, enable timeout 1 s); HTC READY 24 credits × 1664 B, max EP 8, HTC 2.1 without bundling;
CONN_SVC_RESP max message 1664; WMI READY sw_version 0x030400BD, phy_cap 0x03; `hi_board_data` (0x540654) = 0x542400.

Observed facts (stock driver, verified in the emulator):
* The driver uses the counter 4 credit at 0x450 and RX_LOOKAHEAD_VALID bit 0 for BMI (§3.3 confirmed), the diag window as
  §3.3 (address bytes 1–3 written with fixed-address byte writes, the 4-byte incrementing write at the base triggers).
* Upload: `fw-4.bin` OTP and firmware go through LZ_STREAM_START/LZ_DATA (about 116 KB), board data and patches through
  WRITE_MEMORY (about 2 KB); then DONE. `ath6kl: ar6003 hw 2.1.1 sdio fw 3.4.0.189.12.SMARTPHONE api 4` follows HTC READY,
  five CONN_SVC (0x100..0x104) and SETUP_COMPLETE_EX.
* Capability bit 2 in `fw-4.bin` → the host scans with BEGIN_SCAN (0xF08B; channel list at +53/+54), not START_SCAN.
* BSSINFO (`ath6kl_wmi_control_rx`, 0x1004): `u16 freq, u8 frame type (1 beacon), u8 snr, bssid[6], u16 ie_mask`, then
  the beacon body from the timestamp; RSSI = snr − 95 dBm; the body must be ≥ 12 bytes.
* CONNECT (0x1002) station: union `u16 freq, bssid[6], u16 listen, u16 beacon interval, u32 nw_type` + 16-bit lengths;
  assoc request = capability + listen interval + IEs, assoc response = capability + status + AID + IEs
  (`ath6kl_cfg80211_connect_event` @0x7588 skips 4 and 6 bytes). AP own BSS: `u16 freq, bssid = the vif's netdev MAC`.
  AP client (`ath6kl_connect_ap_mode_sta` @0xd028): `phymode, aid, mac[6], auth, keymgmt (WPA2_PSK = 0x10), u16 cipher
  (AES = 8), apsd`; **the assoc request is the whole 802.11 management frame** (IEs taken at +28, +34 for a reassociation).
* vif MACs (`ath6kl_interface_add` @0x9a34): vif n > 0 = `mac[0] ^ (1 << n) | 2`, and with fw capability bit 9
  (set in `fw-4.bin`) `mac[4] ^= 0x80`.
* RX data (`ath6kl_rx` @0x10278): `wmi_data_hdr` info3 bits 3:0 = vif, meta version in info2 bits 15:13 (1 = 6 bytes,
  2 = 3 bytes, 0 = none); 802.3 + LLC/SNAP; total 28..3868 bytes. TX: meta version ≠ 0 adds 12 bytes after the header
  (`ath6kl_wmi_data_hdr_add` @0x11674).
* In AP mode the driver drops unicast frames whose source is not an associated station, so the bridge rewrites the netdev
  peer's MAC to the virtual client's (Ethernet source / destination and ARP hardware addresses).
* Station mode: Wireless → Access Point Settings scans (BEGIN_SCAN on ath0), lists the virtual APs with the lock icon
  for WPA2, and "registering" an AP connects, runs DHCP through `pump`, then disconnects (WMI DISCONNECT by the host)
  and shows "Registered".

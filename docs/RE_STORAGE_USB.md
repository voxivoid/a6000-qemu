# Removable storage (SD / Memory Stick / SDIO Wi-Fi) and USB: static RE for the emulator

Scope: `mcmn_drv.ko mmc_drv.ko mmcioWrapper.ko mmc_core.ko ms_drv.ko ath6kl_sdio.ko usb*.ko usbg_*.ko
ehci/ohci/sd_mod` from `rootfs_usr/kmod/`, plus the kernel's built-in F_USB20HDC UDC and `fusb_otg` code in
`emulator/re/vmlinux.elf`. Firmware 1.10. Everything here is static analysis. Claims I could not prove are marked **[?]**.

Sources: IDA 9.4/Hex-Rays decompiles of every function in every module, plus the kernel USB/OTG/GPIO functions, in
`emulator/re/ida_stor/stor_<module>.c`. Data segments are dumped in `stor_<module>.data.txt`. `<fn> @ <off>` gives the
IDA offset inside the `.ko` (`.text` starts at 0), or the kernel VA. Backup values come from the real camera's
`Backup.bin` (`re/backup/whitelist/backup_props_all.tsv`). The emulator boots the real NAND, so it reads the same values.

---------------------------------------------------------------------------------------------------------------------

## 0. Summary

| Item | Result |
|---|---|
| SD host IP | **Fujitsu/Socionext F_SDH30 (SD Host 3.0) = standard SDHCI v3 register file at 0x000-0x0FF**, plus F_SDH30 vendor registers at 0x100-0x12F and a word at 0x1FC. |
| Slots | mmc0 0xF0300000 IRQ 218 = **SD card** (combo MS/SD slot). mmc1 0xF0301000 IRQ 221 = **SDIO Wi-Fi (AR6003)**. mmc2 0xF0302000 IRQ 224 is unused on this model **[?]**. |
| DMA | **ADMA2, 32-bit descriptors** (Host Ctrl 0x28 DMA-select = 2). Up to 1024 descriptors, 8 KiB table, attr 0x21 (+END 0x02), length 0 = 64 KiB. No SDMA, no PIO for data. |
| Card detect / WP | **Not SDHCI.** Read from the system-controller word **0xF2915010**: bit16 = MSIN#, bit24 = mmc0 CD#, bit25 = mmc0 WP (and the MS-present qualifier), bit26/27 = mmc1 CD/WP. Card power = GPIO p2 bit5. |
| Probe needs | `mmc_drv` probe only registers char devices: no register access. The hardware is touched on the first open/mount (`mmc_hw_open`). `ms_drv` probe only requests IRQ 206. |
| Wi-Fi | `mmc_core`/`mmcioWrapper`/`ath6kl_sdio`/`cfg80211` are **insmod'ed by userspace (libWlanResource) only when Wi-Fi is enabled**. They are **not needed for boot**. With no SDIO card, Wi-Fi fails to start; the camera should otherwise be unaffected **[?]**. |
| Memory Stick | Sony "MSPro-AL" stack. The MSIF registers are at 0xF0303030. Nothing is touched at probe. IRQ 206 must stay low. MS can be absent. |
| USB | OTG glue at 0xF2920000. **OTG+0x10 bit0 = VBUS (active-low), bit1 = ID.** With no cable it must read **0x3**. A reading of 0 means "VBUS present + A-device", which would trigger USB-connect / host logic. UDC = F_USB20HDC at 0xF0210000, IRQ 227. HDMAC 0xF0204000, IRQ 228/229. VBUS/ID IRQ 232/233 have no clear register, so they must be pulsed. |

---------------------------------------------------------------------------------------------------------------------

## 1. Load order and who uses what

`/sbin/init` (nflasha7) runs insmod in this order (from its string table):
`... usb_portMonitor, usbg_sen, usb_extcmd, kikilog, dmm, upm, nfc, grm_*, hdmi, cec, accel, mag, compass,
**mcmn_drv, ms_drv, mmc_drv**, sata_drv(absent), msen_drv(absent), usbg_adb(absent), sprop`.

Dynamic loads by userspace:
* Wi-Fi: `libWlanResource.so` / `libWlanControlCommon.so` insmod `cfg80211, mmc_core, mmcioWrapper, ath6kl_sdio`.
  The same lists also name `sdio_lib/sdio_busdriver/ar6000.ko`, which are absent and belong to an alternative chip.
* USB host (test mode / A-device): `libusb.so`, `libObj.so` insmod `usbcore, ehci-hcd, ohci-hcd, usb_current, scsi_mod,
  sd_mod, sr_mod, cdrom, usb-storage, htmd`.
* USB mass-storage gadget: `libInfraMscServer.so` insmods `usbg_storage.ko`. `usbg_stillimage` (PTP) and
  `usbg_buspower` are loaded by the USB service as well **[?]**.

Consequence: a cold boot needs only `mcmn_drv`, `ms_drv`, `mmc_drv`, `usbg_sen`, `usb_portMonitor`, `usb_extcmd` and
the built-in `fusb_otg`/F_USB20HDC.

---------------------------------------------------------------------------------------------------------------------

## 2. mcmn_drv: media common driver (power, clocks, CD/WP, config)

### 2.1 Targets
`mcmnd_util_target2str`: 0x10 = MS0, 0x20 = MMC0, 0x21 = MMC1, 0x22 = MMC2, 0x30 = SATA, 0x40/0x41 = MSEN.
In `mmc_drv`, `mcmn_target_name[] = {0x20,0x21,0x22}` (data 0x11eb4), so slot n maps to MMCn.
Inserted-media enum (`mcmnd_util_insertedmedia2str`): 0 NON, 1 UNKNOWN, 2 MS, 3 MMC, 4 SATA.

### 2.2 Card detect and write protect: system controller +0x10 (PA 0xF2915010)
Each access is an `ioremap(0xF2915010, 4)` followed by one read:
* `mcmnd_core_getmsindata @ 0x54c`: `MSIN = (reg >> 16) & 1`
* `mcmnd_core_getmmccddata @ 0x5c4`: MMC0 (0x20/0x101) bit **24**, MMC1 (0x21/0x102) bit **26**
* `mcmnd_core_getmmcwpdata @ 0x470`: MMC0 bit **25**, MMC1 bit **27**

Combo-slot decision, `mcmnd_core_checkinsertedmedia @ 0x8c4`, normal branch (`rmtype = byte_AD06 = Backup 0xD9009C = 0`):

| MSIN (b16) | CD0 (b24) | WP0 (b25) | Result |
|---|---|---|---|
| 1 | 1 | x | NON (no media) |
| 1 | 0 | x | **MMC (SD card)** |
| 0 | 0 | x | UNKNOWN |
| 0 | 1 | 1 | MS |
| 0 | 1 | 0 | UNKNOWN |

So the pins are active-low. **SD inserted and writable: bit16 = 1, bit24 = 0, bit25 = 0.** Empty slot: bit16 = 1, bit24 = 1.
The SD write-protect switch is read only through this bit (`mcmnd_core_getwpsw @ 0x1548` is reached from `mmc_hwconf_chk_protect_sw @ 0x589c`).
1 = protected: writes and erases then fail with -1032. SDHCI PRESENT_STATE bit19 is consulted only on the eMMC/MMC path
(`mmc_hw_get_protect_state @ 0x7eac`). Recommended value for 0xF2915010: **0x0C010000** (SD present, RW; mmc1 CD/WP
"high"). In the current `cxd90014-sysctl` model, offset 0x10 is plain storage that reads 0. That reads as UNKNOWN, so it must
be preset.

Media-change events are not interrupt driven here. mcmn registers an OSAL queue callback,
`mcmnd_rm_callbackpoint_frontmedianotify @ 0x2fcc` (queue 0x28017C, 7-byte payload with "hold" bits). Another agent sends
to it (front cover / sub-micro, **[?] origin not found**). Userspace (`libInfraMediaDetect.so`) then calls the
checkinsertedmedia ioctl, which reads 0xF2915010 again. Hot-plug emulation therefore means "flip the bits, then (ideally)
deliver the notify message". For boot, the initial ioctl poll should be enough **[?]**.

### 2.3 Power and clock (all fire-and-forget, no polling)
* `mcmnd_core_setmediapower @ 0x287c`: GPIO port/bit from Backup. For MMC0, port = 0xF9043D = **2**, bit = 0xF9043E = **5**;
  `gpio_set_mode_bit(2,5, on?9:17)`. For MS0, port = 0xF9009A = 0, bit = 0xF90056 = 6. For MMC1 the Backup port is 0xFF.
  That slot takes the `byte_AD09` branch, clears bit6 of the OCTRL3_PUD word (ioremap **0xF2912350**, inside the GPIO/pad block) and returns.
* `mcmnd_core_sethostclock @ 0x1c9c`: returns 0 for MMC targets, so there is no hardware access. For MS0 it uses `udif_devio_hclk` (clkrst4 set/clr 0xF2A30024/28).
* `mcmnd_core_getportstate @ 0x1880` reads 0xF3000330 ("GPIO3 PORTSEL").
* `mcmnd_core_init @ 0x2ecc` ioremaps 0xF2A32000 (0x200) for MS clocking (`gp_CRG11_41`).
* Config: `mcmnd_bkup_readbkupinfo @ 0x6738` reads Backup 0xD90001..0xD900D0 and 0x51000F (all present in the real
  Backup.bin). mmc_drv receives the per-slot config through `mcmnd_api_getcfginfo`.

The model only needs plain RAM semantics for GPIO (0xF2910000+0x100·n), clkrst4 (0xF2A30000) and 0xF2A32000.
An unimp device (reads 0, discards writes) also works, because nothing reads back for verification.

---------------------------------------------------------------------------------------------------------------------

## 3. mmc_drv: Sony's own SD/MMC/SDIO stack (not Linux mmc_core)

`mmc_drv` is a complete host and card stack. `mmc_mmcp_*` handles SD memory, `mmc_mem_*` handles MMC/eMMC, and `mmc_io_*`
handles SDIO. It exports a UDIF block device (`__udif_bdev_add_disk`) and the char devices 13/14/15 (`cdev_device_node`).
Linux `mmc_core.ko` is used only by the Wi-Fi bridge (§4).

### 3.1 Probe (boot): no register access
`init_module` → `udif_driver_register(udif_mmc)` → `mmc_probe @ 0x10c60` → `mmcd_init @ 0x4964`. This only clears state and
calls `udif_cdev_register`. If `mcmnd_api_getmediapower == 1` it also calls `mmc_hwconf_set_system_clk_on`, which issues
mcmn calls with no MMC register access.
`mmc_driver_init @ 0x10c74` → `mmc_hw_init @ 0x7760` ioremaps two windows (struct `{name, ?, size, PA}`):
* `dword_12CC4` = PA **0xF2A00000**, size 0x33000. This is the clock/reset window, covering clkrst4 at +0x30000 and 0xF2A32000 at +0x32000.
* `dword_12CE4` = PA **0xF2921000**, size 0x7000. These are the capability-preset registers (§3.6).

The per-slot SDHCI base is the VA taken from the kernel UDIF table entries 36/37/38 (`mmc_os_init @ 0xfdc`, `udif_devices[55]`).
Those entries are PA 0xF0300000/1000/2000, size 0x1000, IRQ 0xDA/0xDD/0xE0, with no UDIF clock fields, so
`__udif_devio_devclk` returns early.

### 3.2 First open: `mmc_hw_open @ 0x8d48` (per slot, on mount ioctl)
1. `mmc_os_register_int_handler(slot, mmc_hw_irq_handler)`, then `udif_request_irq` (IRQ 218/221/224).
2. write32 **0x1FC = 0** (vendor word; toggled 0/1 around long command timeouts in `mmc_hw_exec_command`).
3. Allocate an 8200-byte ADMA table (`mmc_os_alloc_hw_d_tbl @ 0xdc8`).
4. `mmc_hw_init_host_ctrl @ 0x8bf4`:
   * `mmc_hw_sw_reset(…,2)` → `mmc_hw_fsdh30_prepare_reset @ 0x83ac`: clock gate RMW (§3.5). Then **0x2C |= 1**, poll **0x2C bit1**
     (internal clock stable) for up to 100 ms (else -4). Then write8 **0x2F = 1** (SWRST_ALL), poll **0x2F & 7 == 0** for 100 ms.
   * `mmc_hw_fsdh30_init_vender_spec_reg @ 0x8b8c` (RMW-OR):
     **0x100 |= 7, 0x104 |= 1, 0x108 |= 0x0E010001, 0x114 |= 0x00030F0F, 0x124 |= 0x202**.
     These match the Linux `sdhci_f_sdh30` names: AHB_CONFIG 0x100, TUNING_SETTING 0x108, IO_CONTROL2 0x114,
     ESD_CONTROL 0x124 (bit9 = CMD_DAT_DELAY) **[names from memory of the upstream driver]**.
   * The clock gate again, then **0x2C = 1**, poll bit1 (100 ms).
   * read32 **0x40 (CAPS)**: bits 26/25/24 (1.8/3.0/3.3 V) build the OCR list.
   * write8 0x28 = 0x10 (**DMA select = ADMA2-32**), write32 **0x38 = 0x037F0123** (signal enables), write32 **0x120 = 3**,
     write8 0x2E = 8.
5. Poll **PRESENT_STATE (0x24) bit17** (card state stable) for up to 1 s. This bit is required even with no card.
6. Mask the card interrupt (0x34 &= ~0x100) and enable the vendor "1 ms/5 ms" interrupts: 0x11C |= 2 / |= 1 maps to event bits
   0x4000/0x2000 (`mmc_hw_enable_wait_*_int`). Nothing waits on those events: `mmc_hw_mwait @ 0x8354` uses the OS timeout
   on event 0x80 instead. **Vendor timer interrupts never need to fire.**

Then `mmc_card_open @ 0x6220`: `set_trans_width(1)`, `set_clk_freq(400 kHz)`, `set_max_volt` (0x29 = 0x0E for 3.3 V when
CAPS bit24), `supply_bus_power` (0x29 |= 1). Detection order is **SD (`mmc_mmcp_detect`) → SDIO (`mmc_io_detect`) → MMC
(`mmc_mem_detect`)**.

### 3.3 Command engine: `mmc_hw_exec_command @ 0x84b8`
* `0x34 |= mask` (per command class: 0x000F0001, or from table `dword_10ED0[66..68]` = 0x001F0003 / 0x20 / 0x037F0003).
  If 0x2C bit2 (SD clock) is off, set it for the duration of the command.
* Poll **0x24 & (CMD_INHIBIT | DAT_INHIBIT)** up to 100 ms.
* Data commands: build ADMA2 descriptors `{attr = (len<<16) | 0x21, addr}`, set END (|2) on the last one, then write32
  **0x58** = table PA, write16 0x04 = blksz, write16 0x06 = blkcnt. Transfer mode = DMA | (read 0x10) | (multi 0x22) |
  **ACMD12 (4)** or **ACMD23 (8, with write32 0x00 = blkcnt)**. write16 0x0C, write8 0x2E = 14.
* write32 0x08 = arg, write16 **0x0E** = cmd (index<<8, resp type, CRC/idx check bits, data-present 0x20).
* Wait for event bits 0xF0081 (cmd done / error / reject) for 1 s, or 150 ms + 850 ms with the 0x1FC toggle. Then read the
  response 0x10 (or 0x10..0x1C for R2). For data, wait for xfer-complete (bit1) / ADMA or data errors for 10 s. Errors:
  0xF0000 (cmd), 0x3700000 (data/ADMA). After a non-DMA data command, `sw_reset(3)` = 0x2F = 6.
* ISR `mmc_hw_irq_handler @ 0x7d08`: reads **0x30** (normal | error << 16) and **0x118** (vendor). It W1C-writes both back and
  ORs the result into the event flag. Card-int (0x100) goes to the SDIO handler.

**ACMD23 is used only in UHS mode.** In `sub_FCF8 @ 0xfcf8`, multi-block read/write uses ACMD12 unless the slot is
UHS-capable and the card is in UHS mode (byte 741 == 3). Every transfer is followed by CMD13 (`sub_FBEC`).
All register widths used (`mmc_os_read/write8/16/32`) match SDHCI: 0x0C/0x0E/0x2C/0x3E as 16-bit, 0x28/0x29/0x2E/0x2F/0x54
as 8-bit, 0x24/0x30/0x34/0x38/0x40/0x58 as 32-bit. 0x3E (Host Control 2) is used heavily for UHS/1.8 V/preset; 0x3C/0x3A are
read on error traces. **0xFE (version) is never read.** 0x40 is also *written* (16-bit) by `mmc_hwconf_set_basefreq @ 0x574c`
(`(freq<<8)|0x8C`). Writable CAPS low half on this SoC **[?]**; ignoring the write is harmless because the dividers come from
tables `dword_120C8...` and not from CAPS.

### 3.4 IRQ lines
mmc0/1/2 = GIC **218/221/224**, level-type (the ISR clears at the source). The lines in between (219/220, 222/223, 225/226)
are not requested by any driver here **[?]**.

### 3.5 Clock gate window (0xF2A00000 + …), all RMW without verification
* `sub_7314 @ 0x7314` reads 0xF2A30010 bit(slot). `sub_759C` then ORs mask (0xF or 0xB) << (8·slot) into 0xF2A30070
  (or base+0x30 when that bit is set **[?]**).
* `sub_7160/71BC` provide offsets 0x11A0/0x1170/0x11B0/0x1180 and 0x32130/0x32100/0x32140/0x32110 (slot 0/1; slot 2:
  0x11D0/0x11E0/0x32160/0x32170) for clock-delay / IO-drive settings.
* `mmc_hw_clk_cb @ 0x720c` (udif clk-change callback) writes `a2+0x100+16·i` and then `a2+0x30 = 1`, and polls bit0
  clear ≤100×1 ms (-110 on timeout). Reads of 0 pass.

### 3.6 Capability presets (0xF2921000 window)
`mmc_hw_update_capa_reg @ 0x8214` (exported, no caller inside the module) briefly clears 0xF2A300A0 bit, writes CAPS presets at
0xF2921000 + {0x10, 0x00} (slot0), {0x1010, 0x1000} (slot1), {0x6010, 0x6000} (slot2), then sets the bit again. This means the
SoC allows the SDHCI CAPS to be configured, but the stock flow never does it **[?]**. The model can pick fixed CAPS.

### 3.7 What QEMU's `sdhci` must provide (gap analysis vs `hw/sd/sdhci.c`, QEMU 10.1)
* Standard v3 behaviour at 0x00-0xFF. ADMA2-32, ACMD12, software reset and clock-stable are already supported. QEMU does
  **not** implement ACMD23 (`SDHC_TRNS_ACMD23` is defined but unused). Keep the controller non-UHS so the driver never uses it.
* `SDHC_REGISTERS_MAP_SIZE = 0x100`, so a vendor sub-region 0x100-0xFFF is needed (RAM-like, with 0x118 reading 0 and
  W1C). Write 0x120/0x1FC are just stored.
* CAPS recommendation: 3.3 V only (bit24), HS (bit21), ADMA2 (bit19), SDMA (bit22), base clock 48 MHz (0x30<<8), timeout
  clock in MHz units. **No 64-bit (bit28)**. CAPS1 (0x44) = 0, so there is no SDR50/104/DDR50 and no tuning. The SD card model
  must not set S18A, so the driver never sends CMD11.
* PRESENT_STATE bit17 must be set with or without a card. QEMU sets 0x1FF0000/0x1FA0000, which satisfies this.
* `sd-card` with SDHC (>2 GB) is fine: block addressing is chosen from OCR CCS (`a1[2]` in `sub_FCF8`).

---------------------------------------------------------------------------------------------------------------------

## 4. SDIO Wi-Fi: mmcioWrapper + mmc_core + ath6kl_sdio (AR6003)

* `mmcioWrapper init_module`: `mmc_alloc_host`. The channel comes from **Backup 0x013E000C & 0xF** = **1** (real camera), so
  it is **mmc1 / 0xF0301000 / IRQ 221**. It then calls `mcmnd_api_setledvalid(0x21,0)`, `mmcio_mount(1)` (= mmc_drv
  `mmc_card_open` → `mmc_io_detect`), `mmcio_start_irq/control_func/control_irq/set_buswidth(4)`, `mmcio_request_irq`
  and `mmc_add_host`. The Linux mmc_core host ops are a thin bridge: CMD52 → `mmcio_read/write_ioreg`, CMD53 →
  `mmcio_read/write_iomem` through a 32 KiB bounce buffer. `card_present` always returns 1.
* The modules are loaded only by userspace when Wi-Fi is switched on. Firmware lives in `/usr/wlan/ath/*`
  (`ath6k/AR6003/hw2.x`).
* **If absent:** with an SDHCI at mmc1 and no card, CMD5 times out, `mmcio_mount` fails, insmod returns an error and the Wi-Fi
  UI reports failure. There is nothing on the boot path **[?]**: no boot-time MAC/NVRAM read through SDIO was found.
  An unimp mmc1 also works: `mmc_hw_open` then fails after 100 ms on 0x2C bit1.

---------------------------------------------------------------------------------------------------------------------

## 5. ms_drv: Memory Stick (Sony MSPro-AL)
* `ms_probe @ 0x200e4`: allocates buffers, `msdl_core_init` → `msd_init` → `msproal_init` (`msproal_user_init_system`
  returns 0 and touches no registers), registers a cdev and **requests IRQ 206** (`ms_handler`). No MMIO at probe.
* The MSIF register base is UDIF VA + **0x30** (`msd_sys_get_base_adrs @ 0x303c`), i.e. PA 0xF0303030. Clocks go through
  UDIF hclk 0xF2A30024/28.
* `ms_handler` → `msd_check_int`: if no handle is open it calls **msleep(10)** in the handler. IRQ 206 must never be asserted
  while no MS is mounted.
* Mount happens only when mcmn reports `INSERTEDMEDIA_MS`. **Keep MS absent** (MSIN# = 1).

---------------------------------------------------------------------------------------------------------------------

## 6. USB: fusb_otg glue + F_USB20HDC UDC (kernel built-in)

### 6.1 Blocks (from `fusb_otg_probe @ 0x401d9498`; the real-camera snapshot of the state struct at 0x4bd1d880 confirms the list)
| UDIF idx | Block | PA | Use |
|---|---|---|---|
| 0 | otg | 0xF2920000 | VBUS/ID status, mode select, IRQ enables |
| 1 | ho (EHCI/OHCI) | 0xF0200000 | host; +0x54 (EHCI PORTSC1) bit12 checked by `fusb_start_gadget` |
| 2 | hdc (F_USB20HDC) | 0xF0210000 (0x20000) | device controller |
| 3 | hdmac | 0xF0204000 | USB DMA, IRQ 228/229 |
| 4 | h2xbb | 0xF0203000 | bridge |
| 5 | clkrst4 | 0xF2A30000 | probe writes **+0xA4 = 7** then msleep(1) (USB clocks) |
| 6/7 | IRQ | — | VBUS **232**, ID **233** (`_udif_request_irq` then immediately `_udif_disable_irq`) |

### 6.2 OTG registers (otg base)
* **+0x10 status: bit0 = VBUS, bit1 = ID.** It is read by `fusb_interrupt_usb_vbus @ 0x401d9200`, `fusb_interrupt_usb_id @
  0x401d9284` and `fusb_start_control @ 0x401d9918`.
  Polarity is inferred from three facts. `fusb_otg_get_vbus @ 0x401d8c08` returns "3" (present) when the stored bit0 == 0.
  `fusb_start_gadget @ 0x401d88e8` requires ID == 1 and stored VBUS == 0. `event_cid_exec` maps ID 0 → 1 (A-device), 1 → 2.
  The conclusion: **bit0 is active-low VBUS ("VBUS absent" = 1) and bit1 = 1 means ID float (B-device)** **[?: verify on hardware
  via devmem 0xF2920010]**. With no cable the register must read **0x00000003**.
  If it reads 0 (today's unimp), `f_usb20hdc_enable_communicate` and the otg core see "cable + A-device". The UI would enter
  USB mode and host logic would start.
* +0x40 |= 0x100 (VBUS) / 0x200 (ID) and +0x50 |= same at probe: interrupt enable and edge/both-edge **[?]**.
* +0x04 = 16 (`fusb_start_host` / `resume_initial_set`) and +0x08 = 16 (`fusb_start_gadget`): mode strobes.
* The ISRs never clear a status bit, so the GIC lines must be **pulsed on change** (raise then lower). Otherwise a level IRQ
  storms.

### 6.3 State machine
`fusb_otg_init @ 0x401d9708` initialises state = 1, ID = 1, VBUS = 1 (no cable). `fusb_start_control` (otg-core op, invoked from
userspace through `/dev` ioctl) enables IRQ 232/233, reads +0x10, posts CID/VBUS events and sets state = 2. Gadget start
(state 3) and host start (state 5, which also needs hdc+0x200 bit5) happen only after userspace reacts to a connect event.

### 6.4 F_USB20HDC (0xF0210000)
* Register table `f_usb20hdc_register @ 0x4029c0e0` (offset, readable, writable): CONF 0x0, MODE 0x4, INTEN 0x8,
  INTS 0xC, EPCMD0-15 0x40-0x7C, DEVC 0x200, DEVS 0x204, FADDR 0x208, TSTAMP 0x20C, OTGC 0x300, OTGSTS 0x310,
  OTGSTSC 0x314, OTGSTSFALL 0x318, OTGSTSRISE 0x31C, OTGTC 0x320, OTGT 0x324, DMAC1/DMAS1/DMATCI1/DMATC1 0x400-0x40C,
  DMAC2.. 0x420-0x42C, TESTC 0x500, EPCTRL0-15 0x8000-0x803C, EPCONF 0x8040-0x807C, EPCOUNT 0x8080-0x80FC,
  EPBUF 0x10000 / 0x18000.
* `f_usb20hdc_udc_probe @ 0x401cf774` (platform probe at boot): computes the endpoint FIFO layout (≤ 0x6000), records base
  = hdc, DMA = hdmac and IRQ 227, then calls `fusb_otg_gadget_bind`. **No register access.**
* `usb_gadget_probe_driver @ 0x401d3154` (a gadget function registers) → `f_usb20hdc_enable_communicate`. This touches
  hardware **only if `fusb_otg_get_vbus()`** reports present.
* `f_usb20hdc_udc_otg_probe @ 0x401cfb44` (on connect): `f_usb20hdc_initialize_controller` sets CONF bit2 (soft reset) and
  polls for it to clear (≤ 0xFFFF reads, so 0 passes). It then requests IRQ 227 (`f_usb20hdc_on_usb_function`) and 228/229
  (`on_end_dma_transfer`).
* DEVS (0x204) bits 5/6 = D+/D- line state (`usb_otgcore_get_line_state @ 0x401da85c`). `usb_portMonitor` polls them for
  charger (DCP) detection after a VBUS event.
* Boot-loaded gadget/function modules (`usbg_sen`, `usb_extcmd`, `usb_portMonitor`) only register with gadgetcore/otgcore
  and read Backup. `usb_portMonitor` drives GPIO from Backup 0xF902C7/C8, which is 0xFF (unused) on this camera.

---------------------------------------------------------------------------------------------------------------------

## 7. Prioritised modelling plan

1. **P0 (boot correctness, tiny):** an OTG stub at 0xF2920000 (0x1000) with +0x10 = 0x3 (no cable), RAM elsewhere. Also preset
   sysctl +0x10 = 0x0C010000 (SD present, RW) or 0x0D010000 (empty), selectable by a machine property.
2. **P1 (SD works):** a `cxd90014-sdhc` wrapper. It contains QEMU `generic-sdhci` (sd-spec-version 3, CAPS as §3.7, no UHS) at
   0xF0300000, a vendor RAM sub-region 0x100-0xFFF (0x118 reads 0), IRQ → GIC 218, and an `sd-card` on its bus backed by
   `-drive if=sd,file=<img>` (FAT32/exFAT image, ≥2 GB for SDHC). Bind the card-present GPIO to sysctl bit24 and WP to bit25.
3. **P1b:** RAM-semantics regions for GPIO/pad 0xF2910000-0xF2912FFF, clkrst4 0xF2A30000 and 0xF2A32000, so writes are kept
   and reads give back what was written. Nothing polls them; this is cosmetic and gives better logs.
4. **P2:** the same SDHCI model without a card at mmc1 (0xF0301000, IRQ 221) and mmc2 (0xF0302000, IRQ 224), so the Wi-Fi
   enable path fails fast and cleanly.
5. **P2b:** hot-plug through a QMP/monitor command that toggles the sysctl CD bit and the sdhci inserted state. Sending the
   front-media OSAL notify is still to be found **[?]**.
6. **P3 (USB device mode):** the F_USB20HDC register file from §6.4 with EP0 SETUP handling, IRQ 227, HDMAC 228/229, and
   OTG VBUS/ID changes delivered as pulses on 232/233. Back it with QEMU's USB-redir/usbip, or a host "gadget" bridge for
   mass storage / PTP.
7. **Not needed:** Memory Stick (keep MSIN# = 1, IRQ 206 low), EHCI/OHCI host, ath6kl SDIO card model.

### Verification on the real camera (when powered)
Using `devmem`/`bk` over telnet, read only: 0xF2915010 with SD in, out and write-protected; 0xF2920010 with and without a USB
cable; 0xF0300040/44 (CAPS) and 0xF03000FE (version); and confirm the mmc1 channel through Backup 0x013E000C.

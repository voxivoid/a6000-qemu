# RE: HDMI output and CEC (TX core, HPD, DDC/EDID, CEC, VPH line output)

Static RE of firmware 1.10 plus two short runs of the frozen emulator binary. It follows `RE_DISPLAY_GPU.md` §6
(the first look at hdmi.ko / cec.ko) and `RE_DISPLAY_SCANOUT.md` (VPH panel path).
Sources:
* `/usr/kmod/hdmi.ko` and `/usr/kmod/cec.ko`. These are relocatable ELFs with symbols. Hex-Rays output is in
  `re/ida/gfx/gfx_{hdmi,cec}.ko.c`. Function addresses below are `.text` offsets, so the runtime address is the module
  base plus the offset (hdmi 0x3F34B000, cec 0x3F372000).
* The `.data` tables of both modules: register descriptors and executor "cell" programs. They were decoded with
  `.rel.data` relocations (`objdump -r`). The IDA addresses of `.data` in hdmi.ko are file offset + 0x13CE8.
* `av-cam.bin`: VPH line-output mode setters in `re/ida/gfx/vdf_vph_driver.c`, and the sub-micro task in a copy of
  `smc_avcam.i64`.
* `libInfraHdmi.so` (`re/ida/ui/ui_libInfraHdmi.so.c`) and `libObj.so` (CEC classes).

Claims marked **[?]** are inferred and not proven.

---------------------------------------------------------------------------------------------------
## 0. Summary

* **The HDMI transmitter is an on-SoC APB block at PA 0xF2A24000 (16 KiB). There is no external chip and no I2C.**
  Its register map is an 8-bit register file on a 32-bit stride, split into two pages:
  * page 0 (system / interrupts / DDC) at +0x000
  * page 1 (audio-video / InfoFrames) at +0x400
  * a TMDS PHY at +0x2000

  The layout matches the Silicon Image-derived TX core that TI documents for OMAP4 (`HDMI_CORE_SYS` /
  `HDMI_CORE_AV`, same ×4 offsets) [? IP vendor; every field used below comes from Sony's own descriptors].
  Two helper windows are also used:
  * "hdmiaudi" at 0xF2A20000 (0x2C bytes): audio clock
  * "hdmicolo" at 0xF292C000 (0x0C bytes): the colour-bar test-pattern generator
* **Hot-plug detect is polled, with no IRQ.** hdmi.ko reads **SYS_STAT +0x024 bit1 (HPD)** and **INTR1 +0x1C4 bit6**
  every **100 ms**. In the emulator these are the steady ~20 reads/s to 0xF2A24024/0xF2A241C4. That is normal
  "no cable" behaviour and not a symptom. A second HPD source is the sub-micro: av-cam task 48 sends an "HDMIState"
  message to OSAL queue **0x28017D**, which hdmi.ko also listens on (`hpd_handler`).
* **EDID is read over the TX core's own DDC master** (+0x3B4..+0x3D4, 16-byte FIFO). The driver sends
  `ADDR=0xA0, SEGM, OFFSET, COUNT=0x80, CMD=4` and then reads the FIFO 16 bytes at a time.
* **CEC is a separate APB block at PA 0xF3C10000 (0x190 bytes).** cec.ko drives it through hdmi.ko's executor, also
  polled with no IRQ.
* **The CEC traffic that grows over time in the emulator is a modelling bug, not a firmware poll loop.** cec.ko's RX
  path clears the RX-ready bit by writing 1 to **+0x180** (write-1-to-clear). The generic io-f0 register file reads
  that 1 back, so the driver sees a frame, reads **RX count +0x094 = 0**, and `hdmi_tx_set_read_size(0)` fails
  (-EINVAL). The RX executor then completes with an error. libObj's "CEC Msg Thread" reacts by calling `set_power`
  ioctl 0xC0044302 in a loop:
  * ~9.6 k calls in 45 s
  * 4,040 power-on/off cycles in the first ~100 s
  * a rising rate of 0xF3C10xxx accesses (≈ 6 k/s after 2 min)

  Fix: make +0x180 bits 0..3 W1C (§6.1).
* **VPH feeds HDMI from its "line" output** (sel 0):
  * line YUV = AIF ch52, line OSD = AIF ch54
  * timing generator at 0xF3446000
  * line active size in **VPH+0x454** ([11:0] width, [27:16] height per field)
  * mode in **VPH+0x410**: [18:16] format = 0 480i/576i, 1 480p/576p, 2 1440×1080i, 3 1080i, 4 720p, 5 1080p,
    6 2160p; [2:0] rate family
  * line vsync = GIC **101** [? inferred from the status-register index]

---------------------------------------------------------------------------------------------------
## 1. Hardware

### 1.1 Address windows
| PA | size | name (UDIF / driver) | role | evidence |
|---|---|---|---|---|
| **0xF2A24000** | 0x4000 | `hdmi` | TX core: page 0 +0x000..0x3FF, page 1 +0x400..0x7FF, PHY +0x2000..0x2034 | UDIF table, `hdmi_module_apb_probe`. The run shows init writes at 0xF2A244F4, 0xF2A24020, **0xF2A26008, 0xF2A26020** |
| 0xF2A20000 | 0x2C | `hdmiaudi` | audio clock set/clear (+0x24 / +0x28) | `hdmi_task_initialize` @0xb08c, `hdmi_core_audio_clock_set/clear` @0x9c54/0x9cd0 |
| 0xF292C000 | 0x0C | `hdmicolo` | colour-bar generator: +0x04 enable / resolution, +0x08 disable / 480p | `hdmi_core_{enable,disable}_colorbar`, `..._set_colorbar_reso_{480p,720p,1080p,2160p}` @0x99e8–0x9dc8 |
| **0xF3C10000** | 0x190 | `cec` | CEC controller | UDIF table, `cec_module_apb_probe` |

* No IRQ is used: the UDIF entries for hdmi and cec carry none, and both drivers are state machines driven by
  `osal_utimer` (OSAL timer service id **0x910040**, µs units: `hdmi_tx_execute_timer` @0xc840 sends `1000·ms`).
* The register descriptors are 16-byte `{u32 offset, u32 mask, u32 shift, u32 shadow_index}` in hdmi.ko and 8-byte
  `{u16 offset, u8 mask, u8 shift, u32 shadow_index}` in cec.ko. Their names are `addr_<off>_<bits>`, for example
  `addr_0024_1` = +0x024 bit1. Every register is accessed as a 32-bit word that carries an 8-bit value. The
  emulator run shows `rd4/wr4` for HDMI and `rd1/wr1` for CEC.

### 1.2 TX core registers used by hdmi.ko (offsets from 0xF2A24000)
The OMAP4 name is given only as a reading aid [?].

| off | field (Sony descriptor) | meaning | evidence |
|---|---|---|---|
| 0x014 | byte | soft reset (`SRST`): written 3 then 0 | power-on / power-off cells (IDA 0x1740C, 0x15E18) |
| 0x020 | bit0 | `SYS_CTRL1.PD#` (1 = TX powered) | `hdmi_task_initialize`: `0x020 = 1`. Power-on cell |
| **0x024** | **bit0 P_STABLE, bit1 HPD, bit2 RSEN** | `SYS_STAT` (read-only) | `hdmi_core_check_p_stable` @0x9984 (`addr_0024_0`), `hpd_exec_hpd_state` @0xfee8 (`addr_0024_1`), `hdmi_core_check_rsen_state` @0x9904 (`addr_0024_2`) |
| 0x0CC | bits 4, 5 | [?] (`addr_00CC_4/5`) | |
| 0x120 | [1:0] | pixel repetition / input clock multiplier [?] (`VID_CTRL`) | `hdmi_core_task_content`: `addr_0120_1_0` set from the mode |
| **0x1C4** | **bit6 = HPD change** | `INTR1`, **write 0x40 to clear** | `hpd_exec_hpd_intr` @0xfe7c reads it. `hpd_exec_hpd_intr_reset` @0xfdec writes 0x40. `hdmi_core_intr_read_hpd/check_hpd` @0x8f38/0x9024 |
| 0x1D4 | bit6 | `INT_UNMASK1` [?] | |
| 0x1E4 | byte, power-on value 0x02 | `INT_CTRL` (pin polarity) [?] | power-on cell |
| 0x208 | [6:5] = 1 | [?] | power-on cell |
| **0x3B4** | byte = 0xA0 | `DDC_ADDR` (EDID slave address) | EDID cells (IDA 0x1731C) |
| **0x3B8** | byte | `DDC_SEGM` = block >> 1 | `hdmi_core_edid_segment_and_offset` @0x933c |
| **0x3BC** | byte | `DDC_OFFSET` = (block & 1) ? 0x80 : 0 | same |
| **0x3C0** | byte = 0x80 | `DDC_COUNT1` (bytes to read) | EDID cells |
| 0x3C4 | [1:0] | `DDC_COUNT2` (not written by the EDID path) | |
| **0x3C8** | bit3 = FIFO full (16 bytes ready), **bit5 = NO_ACK, bit6 = BUS_LOW** | `DDC_STATUS` | `hdmi_core_chk_edid_status` @0x8f94: bit5 or bit6 → error (255), bit3 → "data ready" (1), else retry |
| **0x3CC** | [3:0] command | `DDC_CMD`: **0xF abort, 0x9 clear FIFO, 0xA clock SCL, 0x4 enhanced (segment) sequential read** | EDID cells |
| **0x3D0** | byte | `DDC_DATA` (FIFO pop) | 16 × read cells → `hdmi_core_edid_set_buffer` @0xa794 |
| 0x3D4 | byte | `DDC_FIFOCNT` [?] (not read by the driver) | descriptor only |
| 0x404..0x4BC | various | video/audio path config (`0x404 bit1`, `0x408 [2:0]`, `0x450`, `0x474=0x49`, `0x478=4`, `0x484/0x488=2`, `0x490=0x5C`, `0x4BC=0x15` at power-on) | power-on cell |
| 0x4F4 | bit0 | `AV DPD` (power-down control): 0 at init, 1 when powered | `hdmi_task_initialize`, power-on/off cells |
| 0x4F8 / 0x4FC | bytes | InfoFrame enable / repeat bits (`addr_04F8_3`, `_6`, `addr_04FC_0/_1`) | InfoFrame cells |
| **0x500–0x540** | AVI InfoFrame: type 0x82, ver 2, len 0x0D, checksum +0x50C, DB1.. at +0x510 + 4n; **+0x51C [6:0] = VIC** | `hdmi_core_task_content` (`addr_051C_6_0`), power-on cell | |
| 0x580–0x5F0 | SPD InfoFrame (type 0x83, vendor bytes "Sony" at +0x590..+0x59C) | power-on cell | |
| 0x600–0x634 | Audio InfoFrame (type 0x84, len 0x0A) | power-on cell | |
| 0x680–0x6F8 | MPEG / vendor-specific InfoFrame (type 0x81 = HDMI VSIF, 3-D modes) | power-on cell, `hdmi_core_task_vsif` | |
| 0x700–0x77C | generic packet (+0x700 = 0x0A, +0x704 = 0x81, +0x708 = 0x31) [?], +0x77C = 0x10 | power-on cell | |
| **0x2004..0x2034** | **TMDS PHY**: 0x2004 bit4/bit5, **0x2008 bit0 = PHY on** (0 at init, 1 at power-on), 0x2020 bit0 = 1 at init / [4:1]=6, 0x2024 [1:0]=2 and [6:4] = bias step (Backup **0x9B0005**, default 7), 0x2030 [7:6] = rise/fall time (Backup **0x9B0006**), [5]=1, [4:0]=0x0B, 0x2034 bit0 = 1 | `hdmi_core_write_bias_step` @0x93f8, `hdmi_core_write_chng_rise_fall_time` @0x937c, power-on cell | |

### 1.3 Hot-plug detect: two sources
hdmi.ko's probe walks a 7-entry init table (`.rodata`+0xA70): inter, apb, tx, core, dev, **detect_exec**, **detect_front**.
Both HPD sources are installed, and both end in `hdmi_hpd_detect_notice` @0xf24 → `hdmi_core_on_changed(1, 0, level)`.

1. **detect_exec (TX register poll)**, set up in `sub_FC54` @0xfc54 with `dword_196C0 = 100` (ms). The cell program at
   IDA 0x18EF0 runs in a loop:

   | cell | action |
   |---|---|
   | type 6 | read +0x024 → `hpd_exec_hpd_state` (bit1) |
   | type 6 | read +0x1C4 → `hpd_exec_hpd_intr` (bit6) |
   | type 13 | if the intr bit is set, write 0x40 to +0x1C4 (`hpd_exec_hpd_intr_reset`) |
   | type 12 | `hpd_exec_notify` |
   | → `hpd_exec_check_hpd_comp` | if the level changed: notify, then wait `hpd_exec_set_check_cycle` = timer 100 ms, then repeat |

   This matches the run: 745 reads each of 0xF2A24024 and 0xF2A241C4 at a constant 10 Hz.
2. **detect_front (sub-micro)**: `sub_FFF8` @0xfff8 registers `hpd_handler` @0x100dc on OSAL queue **0x28017D**.
   Message byte 3 bit0 = "HPD valid", byte 4 bit0 = HPD level. av-cam task 48's HDMIState handler `sub_43915DC4`
   (log id 0x85 HDMIState_Snd, `RE_SUBMICRO_LINK.md` §3.2) references queue 0x28017D and latches two flags from
   the decoded slot-A state (`+252 bit0`, `+41 bit5`) [? exact frame bytes and roles; the decompile is unreliable there].
   Presumably the front micro sees the connector's HPD pin while the main CPU sleeps [?].

For emulation the TX register is enough: the driver polls it no matter what the sub-micro says.

### 1.4 HDMI state machine (hdmi.ko, kernel thread "hdmi_state")
`hdmi_core_status_change_table` / `_state_chart_vector_table` (`.data` +0x0/+0xD8):
**power_off → hpd_detect_wait → edid_analyze → rsen_detect_wait → output_setting_wait → outputting**.
* **edid_analyze** runs the DDC program of §1.5. The parser `hdmi_core_edid_parser` @0x3780 accepts EDID 1.x
  block 0 plus CEA-861 extensions: tag 2, video / audio / VSDB / extended data blocks, HDMI VSDB OUI 03 0C 00, block
  map 0xF0. It returns -11 to ask for the next block. The 128-byte buffer goes up to userland through
  `hdmi_core_edid_get_data`.
* **rsen_detect_wait / outputting**: `hdmi_core_read_rsen` @0x97c8 reads +0x024 bit2. While outputting, the cell at
  IDA 0x18C14 (timer 1000 ms) re-reads RSEN once per second.
* **Power-on** (cell program IDA 0x1740C, 104 cells):
  * `start_idck`, audio clock set
  * page-0/1 defaults (table in §1.2)
  * waits for **+0x024 bit0 P_STABLE** (`hdmi_core_check_p_stable`)
  * PHY on (0x4F4=1, 0x2008=1, 0x2020, 0x2024, 0x2030, 0x2034), bias / rise-time from Backup
  * waits 30 ms
  * INT_CTRL = 2
  * HPD intr check, soft reset 3 → 0
  * default AVI / SPD / audio / VSIF / generic InfoFrames
* **TMDS on** (`hdmi_core_task_tmds_on`) rewrites the AVI InfoFrame (VIC in +0x51C) and the other InfoFrames for the
  requested resolution.

### 1.5 DDC / EDID transaction (exact cell order, hdmi.ko IDA 0x1731C and 0x18A84 / 0x18AD4)
```
DDC_CMD(0x3CC)=0xF ; =0x9 ; =0xA ; wait 10 ms ; =0xF       abort, clear FIFO, clock SCL, abort
DDC_ADDR(0x3B4)=0xA0
SEGM(0x3B8)=blk>>1 ; OFFSET(0x3BC)=(blk&1)?0x80:0          hdmi_core_edid_segment_and_offset
COUNT1(0x3C0)=0x80
DDC_CMD=0x4                                                 enhanced DDC read
loop: read DDC_STATUS(0x3C8)
        bit5|bit6 → error ; bit3 → pop 16 × DDC_DATA(0x3D0) ; else wait 30 ms and poll again
      (≤ 6 empty polls, then re-read once; 128 bytes → parser)
```
A model has to fill a 16-byte FIFO from `EDID[seg·256 + offset …]`, set bit3 while ≥ 16 bytes are pending, and
refill as the driver pops. When HPD is 0 the driver never gets here.

### 1.6 CEC block (offsets from 0xF3C10000)
Derived from the cec.ko cell tables (`cec_core_cell_*` in `.data`) and `cec_core_*` functions @0x7000–0x8400.

| off | field | meaning | evidence |
|---|---|---|---|
| 0x000–0x03C | bytes | **TX frame buffer** (header = initiator<<4 \| dest first; polling = `la·0x11`) | `cec_core_write_tx_frame` @0x7cdc (burst write at `addr_0000`), `cec_core_pre_polling` @0x77c8 |
| 0x040 | [4:0] | TX length; also indexes the timeout table `"68Acfilnqtwy…"` (20 ms ticks per length) | `cec_core_init_line_checker` @0x70ec |
| 0x044 | bit0 | **TX start** (1 in send_frame / polling, 0 in power on/off) | cells `send_frame`, `send_polling` |
| 0x048 | [6:0] = 0x12 | bit timing [?] | `power_on` cell |
| 0x04C | byte = 0x57 | bit timing / free time [?] | `power_on` cell |
| 0x054–0x090 | bytes | **RX frame buffer** (burst read of `count` bytes from `addr_0054`) | `cec_core_read_rx_frame` @0x7d7c |
| **0x094** | [4:0] | **RX byte count** (1..16, 0 is invalid) | `recv_read` cell, `hdmi_tx_set_read_size` @hdmi 0xc700 rejects 0 |
| 0x098 | bit0 | RX buffer release / RX enable [?] (0 at power on/off, 1 in `recv_clear`) | cells |
| 0x09C | bit0 = "ignore" while LA = 15, bits1..3 [?] | `cec_core_set_force_ignore` @0x7eb0 | |
| **0x0A0** | [3:0] | **own logical address** (15 = unregistered) | `cec_core_set_logical_addr` @0x7e48 |
| 0x0A4 | [5:0] = 9 | clock prescaler [?] | `power_on` cell |
| 0x0AC | bit0 = 1 | CEC enable [?] | `power_on` cell |
| 0x0B0 | bit0 pulse 1→0 | soft reset [?] | `power_on` cell |
| 0x0B4–0x120, 0x128/0x12C, 0x14C–0x158 | bytes | descriptors only (logs / debug dump `cec_core_dump_register` @0x4310) [?] | |
| **0x180** | **bit0 RX ready, bit1 TX done – no ACK, bit2 TX retry / arbitration error, bit3 TX done – ACK** | **interrupt status, write-1-to-clear** (0xF at power-on, 0x1 after RX, 0xE after TX) | `cec_core_chk_rx_ready` @0x7c1c (bit0), `cec_core_chk_tx_int_request` @0x7a58, `cec_core_chk_tx_ready` @0x7828 (bit2 → resend once, result = fail unless bit3), `cec_core_chk_tx_int_polling` @0x7b14 (bit1 on a polling frame → address free → adopt it; bit3 → address taken → try next) |
| 0x184 | byte = 0xF / 0 | interrupt enable (power on / off) | cells |

Loops:
* **RX** (`cec_core_task_control_for_hdmi_limited` @0x74c4, from power-on): read 0x180 bit0. If clear, run
  `recv_timer` (20 ms), then check again. If set: read 0x094 → burst-read 0x054 → `cec_core_analyze_rx_message` →
  write 0x180 = 1 and 0x098 = 1.
  On real hardware this is **50 single-byte reads per second**, forever, even with no cable.
* **TX**: write 0x180 = 0xE, the frame, 0x040 = length, 0x044 = 1. Then, every 20 ms, read 0x180 bit3 (and 1/2)
  until a completion bit appears or the line-checker count reaches 0. A count of 0 means a timeout, reported as
  failure.

---------------------------------------------------------------------------------------------------
## 2. Output pipeline (VPH line output → HDMI TX)

The VPH has a panel output and a line output (`RE_DISPLAY_SCANOUT.md` §1). HDMI is the **line** output (index /
"sel" 0):

| item | line output | evidence |
|---|---|---|
| YUV layer | AIF **ch52** (0xF3441280), enable **VPH+0x184 bit0** | `sub_438F90CC`, `sub_438F6CF4(a1=0)` |
| OSD layer | AIF **ch54** (0xF3441380), **VPH+0x414 bit0** = 32/16 bpp, window 0x18C/0x190, resizer 0x6500 | `sub_438F4838(a1=0)`, `sub_438F503C` |
| active size | **VPH+0x454**: [11:0] width, [27:16] lines per field | `sub_43AC1380` (W_440454_FFF / _FFFs16) |
| mode | **VPH+0x410 [18:16] format, [2:0] rate family** | `W_440410_7s16`, `W_440410_7` in every mode setter |
| timing generator | **0xF3446000–0xF3446034** (12-bit fields): 0x600C [12:0] H start, 0x6010 [27:16]/[11:0] V start of field 1 / field 2, 0x6008 three 7-bit sync widths, 0x6034 two 10-bit fields, 0x6000/0x6004/0x601C–0x6028 start/end pairs [?] | `sub_43AC1E90`, `sub_43AC1EAA`, `sub_43AC1EF0`, `vdf_vph_regs.txt` |
| vsync | GIC **101** (`sub_43AC0F4E` reads 0x118 = status of index 0) [? that index 0 is the line output] | `RE_DISPLAY_SCANOUT.md` §4 |

Per-mode line-output setters in av-cam. The second family (0x438FB9A0…) uses H start 88 for SD; the first family
(0x438F9B20…) also exists. "H total" is the global word_43E555CC; "V" is word_43E555BE.

| setter | 0x410 fmt / rate | active (0x454) | H start (0x600C) | V start (0x6010) | H total | V | standard |
|---|---|---|---|---|---|---|---|
| 0x438FB9A0 / 0x438FBC44 | 0 / 0, 0 / 1 | 720×240 / 720×288 | 88 | 0,0 / 23,336 | 858 / 864 | 525 / 623 | 480i / 576i |
| 0x438FBE44 / 0x438FC038 | 1 / 0, 1 / 1 | 720×480 / 720×576 | 88 | 45 | 858 / 864 | 524 / 620 | 480p / 576p |
| 0x438FABA0 / 0x438FAD64 | 2 / 0, 2 / 1 | 1440×540 | 144 | 21,584 | 1650 / 1980 | 1123 | 1440-wide 1080i [?] |
| 0x438FC22C / 0x438FC430 | 3 / 0, 3 / 1 | 1920×540 | 192 | 21,584 | 2200 / 2640 | 1123 | **1080i 60 / 50** |
| 0x438FC630 / 0x438FC828 | 4 / 0, 4 / 1 | 1280×720 | 252 | 26 | 1650 / 1980 | 745 | **720p 60 / 50** |
| 0x438FCA1C / 0x438FCC14 | 5 / 0, 5 / 1 | 1920×1080 | 192 | 42 | 2200 / 2640 | 1121 | **1080p 60 / 50** |
| 0x438FCE08 / 0x438FD000 | 5 / 0, 5 / 1 | 1920×1104 | 192 | 20 | 2200 / 2640 | 1123 | [?] (1080p variant) |
| 0x438FD1F4 / 0x438FD3F0 | 5 / 0, 5 / 1 | 1920×540 | 192 | 584 | 2200 / 2640 | 1123 | [?] (half-height 1080p) |
| 0x438FD5EC / D814 / DA3C | 6 / 2, 6 / 5, 6 / 3 | 3840×2160 | 334 | 83 | 4400 / 5500 / 5280 | 2242 | 2160p 30 / 24 / 25 (unused on a5000 [?]) |

The H totals are the CEA-861 values: 858/864, 1650/1980, 2200/2640, 4400/5280/5500. So [2:0] = 0 is the 60 Hz
family and 1 is the 50 Hz family.

Software path for a connect [? order inferred from class and message names, not traced]:
1. hdmi.ko notifies (`hdmi_core_on_changed` → `/dev/hdmi` poll/read and an OSAL message to 0x8F013A [? log/notify service]).
2. `libInfraHdmi` (`HDMI::HdmiCtrl("/dev/hdmi")`, `HdmiInfraImpl`, states `WaitStart`, `RSEN_Lo`, …) reads the EDID and
   picks the output format (`MSG_PARAM_SET_RESOLUTION`, `…_SET_OUTPUT`).
3. It tells av-cam VDF through DataflowInfra (`VDF_Data_t`; VDF classes `VdfDisplayCmdSetHdmi*`). VDF switches the VPH
   line output with one of the setters above and connects the YUV/OSD pins (libObj
   `OBJDISPLAY::SequenceConnectHDMIPinCmd`).
4. It asks hdmi.ko for `set_reso` / `tmds_on` (AVI VIC). CEC (libObj `CEC::CecCtrl`) is told through `set_connect`.

---------------------------------------------------------------------------------------------------
## 3. Software: who polls what (emulator run, stock firmware, no SD, no cable)

Measured with `-d unimp` (io-f0 register file log) and the a5ktrace plugin on `cec_dev_ioctl` (0x3F378058).

| owner | thread | registers | rate | verdict |
|---|---|---|---|---|
| hdmi.ko detect_exec | hdmi.ko executor (osal_utimer callback) | rd 0xF2A24024, rd 0xF2A241C4 | 10 Hz each, constant | **correct** "no cable" behaviour |
| hdmi.ko state machine | kernel thread `hdmi_state` | — | idle in hpd_detect_wait | correct |
| cec.ko RX loop + **libObj CEC** | `CEC Msg Thread` (libObj `CEC::MessageThread`, UIPC 0x9C015B), one `set_connect` from `THREAD_INFRA2` | see below | 28 k accesses in the first 15 s after the UI, ≈ 6 k/s after 2 min, rising | **symptom of a missing model** (W1C) |

The CEC cycle as logged:
```
power_on:  44=0 98=0 A4=9 48=12 4C=57 B0=1 B0=0 9C=5 A0=F 180=F 184=F AC=1
recv_clear: 180=1 98=1
RX check:  rd 180 -> 1 (stale, should be 0)  rd 94 -> 0  → set_read_size(0) = -EINVAL → RX executor error
power_off: 44=0 98=0 184=0 180=F 9C=5 A0=F  →  userland set_power ioctl again …
```
ioctl numbers (`cec_dev_ioctl` @0x6058):

| ioctl | function |
|---|---|
| 0xC0044301 | `set_connect` |
| **0xC0044302** | **`set_power`** (9,581 calls in ~45 s, all from "CEC Msg Thread") |
| 0xC0044303 | `set_status` |
| 0xC0044304 | `set_request` |
| 0xC001430E | `set_lgc_addr` |
| 0xC004430B | `get_abort_flag` |

The kernel error-report path `sub_7F20` (TX-side error message) never ran.

---------------------------------------------------------------------------------------------------
## 4. Emulation plan

### 4.1 (a) "No cable": stop the CEC churn (minimal, no new device)
The only broken piece is CEC +0x180, which must not read back written 1s. The existing register-file helper
already supports write-to-clear bits, so the board change is one line:
```c
/* CEC 0xF3C10000: +0x180 = interrupt status, write-1-to-clear (cec.ko
 * cells recv_clear / post_request write 1 / 0xE; chk_rx_ready reads bit0).
 * docs/RE_HDMI.md §1.6 */
a5k_regfile_ex("cec", 0xF3C10000, 0x1000, NULL, "0x180=0xf", -500);
```
Expected result [? not run, since the frozen binary cannot be changed]:
* CEC powers on once
* then 0xF3C10180 is read 50 times per second (the RX timer), with no power cycling and no `set_power` storm
* HDMI needs nothing: +0x024 / +0x1C4 already read 0, which is "HPD low, no change"

### 4.2 Proper models
**`hw/display/cxd90014_hdmi.c`**, device `cxd90014-hdmi`: MMIO 0xF2A24000/0x4000, priority 0 (above io-f0).
* It is an 8-bit register file on a 32-bit stride. Unknown offsets are RAM-backed and logged with `LOG_UNIMP`.
* Properties: `hpd` (bool, can be changed at run time with `qom-set /machine/hdmi hpd on`) and `edid=<file>`.
  The default is a built-in generic EDID, not taken from any Sony or real-monitor dump.
* **0x024 (RO)**:
  * bit1 = `hpd`
  * bit2 (RSEN) = `hpd && phy_on`, where phy_on = 0x2008 bit0 && 0x4F4 bit0
  * bit0 (P_STABLE) = 0x020 bit0 (PD#) [? on real hardware it needs the pixel clock from the VPH line output]
* **0x1C4**: bit6 is set on every change of `hpd`; writing 1 clears it.
* **DDC engine**:
  * a write of 0x3CC [3:0] = 0x4 with 0x3B4 = 0xA0 and `hpd` = 1 queues 0x3C0 bytes from
    `edid[0x3B8·256 + 0x3BC]` into a 16-byte FIFO
  * 0x3C8 bit3 = FIFO holds 16 bytes (or the last bytes of the transfer) [?]
  * a read of 0x3D0 pops a byte, and 0x3D4 = FIFO count
  * 0x9 / 0xF clear the FIFO
  * with `hpd` = 0, or an address other than 0xA0, set 0x3C8 bit5 (NO_ACK)
* **Output signal**:
  * TMDS active = 0x2008 bit0 && 0x4F4 bit0 && `hpd`
  * the mode comes from the AVI VIC at 0x51C [6:0], cross-checked against the VPH line mode
  * the result goes to the VPH model through a GPIO line or a QOM link
* InfoFrame / PHY registers are RAM.
* hdmiaudi 0xF2A20000 and hdmicolo 0xF292C000 stay register files. The colour bar is optional: if
  0xF292C004 is enabled, show bars on the HDMI console.

**`hw/misc/cxd90014_cec.c`**, device `cxd90014-cec`: MMIO 0xF3C10000/0x200, with a virtual CEC bus.
* 0x180 W1C status, 0x184 enable, TX buffer 0x000–0x03C, 0x040 length, 0x044 start, RX buffer 0x054–0x090,
  0x094 count, 0x098 release, 0x0A0 own LA.
* When 0x044 bit0 is written, decode the destination from TX byte 0's low nibble. After about
  `4.5 ms + 24 ms·len` (or at once) set:
  * **bit1 (no ACK)** if there is no cable, or if no virtual device holds that LA. This includes the polling
    frame to its own LA, so the camera adopts the address.
  * **bit3 (ACK)** for LA 0 when a virtual TV is attached
  * then clear 0x044 bit0
* Optional virtual TV, behind a property: answers `Give Physical Address` and `Give Device Power Status`, and
  sends `Active Source` / `User Control Pressed` by filling the RX buffer, setting 0x094 = len and 0x180 bit0.
  `RX ready` is never set without a frame.

**HDMI frame as a second QEMU console.** This goes in the VPH model (`hw/display/cxd90014_vph.c`), which already
latches AIF state at vsync.
* `graphic_console_init` a second console, "hdmi".
* At each line-output vsync, while TMDS is active, compose:
  * line YUV: ch52 start / line bytes / count / pitch, enable 0x184 bit0
  * line OSD: ch54, bpp from 0x414 bit0, window 0x18C/0x190, resizer 0x6504
* Use the panel compositor code and size the surface from 0x454 (double the lines when 0x410 [18:16] ∈ {0, 2, 3},
  the interlaced modes).
* While the console is inactive, show a "no signal" surface.
* Pulse GIC 101 at the field rate of the line mode (60 / 50 Hz family from 0x410 [2:0]). Today it is pulsed at
  60 Hz together with 102.

**Default EDID** (generated in code, 256 bytes):
* block 0: EDID 1.3, generic "QEMU HDMI" monitor, preferred timing 1920×1080p60
* CEA-861 extension with video data block VIC 16 (1080p60), 31 (1080p50), 5 (1080i60), 20 (1080i50), 4 (720p60),
  19 (720p50), 2/3 (480p), 17 (576p)
* HDMI VSDB, OUI 00-0C-03, physical address 1.0.0.0, which cec.ko needs before it allocates a logical address
  (`cec_core_status_task_check_phy_addr_state` tests +212 != 0xFFFF)

**Verification steps**
1. With the change in §4.1: count 0xF3C10180 reads per 10 s (≈ 500) and the `set_power` ioctls (≤ 2).
2. `qom-set hdmi hpd on`, then confirm, in order:
   * the HPD notify
   * the DDC sequence of §1.5 in `-d unimp`
   * `hdmi_core_edid_parser` returning 0 (plugin probe at hdmi base + 0x3780)
   * the VPH line-mode setter that gets called (probe the table in §2)
   * TMDS on
   * a picture on console 1

---------------------------------------------------------------------------------------------------
## 5. Open questions
1. Which `DDC_STATUS` bit means "FIFO has data" exactly: is bit3 FIFO-full, and how does the hardware flag the last
   partial chunk? The driver always asks for 128 bytes, so 8 × 16 is enough for emulation.
2. P_STABLE (0x024 bit0): does it depend on the VPH line output running (pixel clock), or only on PD#? If the
   power-on cell loops, gate it on VPH line output enable.
3. The sub-micro HDMIState path (queue 0x28017D): the exact slot-A byte/bit, and whether the UI needs it in addition
   to the TX HPD bit (for example for wake-on-connect, or "HDMI connected" icons coming from av-cam).
4. Confirm that IRQ 101 is the line output's vsync. Probe `sub_43AC0F4E` once the line output is enabled. Also,
   does VPH stop pulsing it when the line output is off?
5. Meaning of the 1920×1104 and 1920×540 (V start 584) line modes, and of format 2 (1440×540).
6. libObj CEC retry policy: why the `set_power` rate rises over time instead of backing off. This becomes moot after
   §4.1.
7. Roles of CEC 0x048/0x04C/0x0A4 (bit timing at which input clock?) and of 0x0B4–0x158. The cell programs only
   write them.
8. The libInfraHdmi → VDF message for "connect HDMI" and the order of `set_reso` / `tmds_on` / VDF mode switch. This
   needs a run with HPD = 1.

---------------------------------------------------------------------------------------------------
## 6. Implementation notes (emulator, branch `hdmi`)

The §4.2 plan is implemented as three models plus the §4.1 CEC fix (which stays — it is what keeps
the churn down with the cable unplugged).

* **`hw/display/cxd90014_hdmi.c`** (`cxd90014-hdmi`, 0xF2A24000/0x4000). 8-bit-on-32-bit register
  file, RAM-backed, unknown offsets logged `LOG_UNIMP`. Property `hpd` (bool, run-time settable),
  `edid=<file>` (128/256 bytes, default = built-in generic 1080p60 EDID generated in code). SYS_STAT
  +0x024 RO, INTR1 +0x1C4 W1C, a DDC engine that serves EDID from `edid[segm·256 + offset]` through a
  16-byte FIFO, and GPIO out 0 = TMDS active (`hpd && 0x2008 bit0 && 0x4F4 bit0`).
* **`hw/misc/cxd90014_cec.c`** (`cxd90014-cec`, 0xF3C10000). 0x180 W1C status, TX at 0x000/len 0x040/
  start 0x044, RX at 0x054/count 0x094, own LA 0x0A0. Property `connected` (follows `hpd`) and `tv`
  (default on): with the cable in, a virtual TV at LA 0 ACKs directed frames (bit3), everything else
  no-ACKs (bit1) so the camera adopts its logical address, and the TV answers Give Physical Address /
  Power Status / CEC Version / OSD Name / Vendor ID / Menu Language by filling the RX buffer.
* **HDMI as console 1** in `hw/display/cxd90014_vph.c`: the per-output compositor was factored into
  `vph_compose(dst, W, H, yuv_on, ky, ko, bpp, osd_scale)`; the panel uses it with ch51/ch53 (1:1),
  the HDMI console with the **line** channels ch52/ch54, sized from VPH+0x454 and doubled for the
  interlaced +0x410 formats, OSD nearest-neighbour scaled to the line size. A GPIO in `hdmi-tmds`
  (from the TX model) gates it; while inactive the console shows a dark "no signal" surface.
* **Board** (`hw/arm/sony_a5000.c`): `-M hdmi=on|off` (default off) wires `hpd`/`connected`; it is
  run-time settable (`qom-set /machine hdmi on`). The TX GPIO goes to the VPH `hdmi-tmds` input. The
  VPH gets id `vph` so `screendump FILE vph 1` grabs the HDMI console.

### Findings from the frozen-firmware runs (hdmi=on boot and run-time qom-set)
* **SYS_STAT +0x024 bit0 is not OMAP4 P_STABLE.** `hdmi_core_check_p_stable` @0x9984 (power-on cell)
  loops until the bit reads **0**: it returns 3 (retry, `sub_D0E4`, -EIO after 20 tries) while the bit
  is set and 2 (continue) once clear. With bit0 driven from PD# (= 1 after power-on) the power-on cell
  retried out and the TX never came up. The model now reads bit0 = 0 (comment in `hdmi_sys_stat`,
  resolves doc §5 q2 for emulation: bit0 is a "not yet stable / busy" flag, active high).
* With that fixed, the full path runs on a stock boot with `hdmi=on` **and** on a run-time
  `qom-set /machine hdmi on` from the shooting screen: HPD notify → power-on cells (PHY 0x2008/0x2020/
  bias/rise-fall, soft reset 3→0, InfoFrames) → DDC reads EDID block 0 and 1 (`nack 0`) →
  `hdmi_core_edid_parser` returns 0 → `hdmi_state` reaches outputting → AVI VIC +0x51C = **0x10 = 16
  (1080p60, = the EDID's native VIC)** → TMDS on.
* **VPH line output** then programs format 5 (1080p) in +0x410, active **0x04380780** in +0x454
  (0x780 = 1920 W, 0x438 = 1080 H), and the line OSD channel **ch54** (0xF3441380) with the same OSD
  buffer the panel uses (0x8F758000) — so the HDMI console shows the live UI, scaled to 1920×1080, and
  MENU/PLAY render on it. The av-cam line-output format comes from the OSAL idck service 0x66025A
  (`sub_43AC0974` in av-cam, writes VPH+0x068/+0x06C = 0x1000000).
* **CEC with the cable in:** the camera polls, adopts a logical address (polling frame no-ACKs), and
  the virtual TV answers; a handful of TX frames, no `set_power` storm. With the cable out, the CEC
  block (§4.1 W1C + `connected=off`) never raises RX-ready, so there is no churn — unchanged from
  stock.
* **Open:** bit0-stable semantics (now modelled as always-ready) and the sub-micro HDMIState queue
  0x28017D path (doc §5 q3) are not needed for the TX register poll to detect the cable; the line
  resizer at VPH+0x6500 is not decoded (OSD is full-frame nearest-neighbour).

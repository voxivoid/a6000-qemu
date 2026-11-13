# RE: input / indicator / IR / I2C / sensors / NFC / RTC / power / timer / backup kernel modules

Scope: `input.ko indctr.ko sircs.ko i2c.ko accel.ko mag.ko compass.ko nfc.ko udate.ko upm.ko utimer.ko backup.ko`
from `/usr/kmod` (firmware 1.10). Method: Hex-Rays decompilation of every function (windev IDA 9.4; output kept in
`re/ida/kmod_i2cgrp/<module>.c`, addresses below are **IDA addresses = .ko section layout, .text starts at 0**),
kernel `vmlinux.elf` disassembly, the real-camera WBI RAM snapshot (`re/wbi`), and the user's
`re/backup/setting/Backup.bin` (decoded in `re/backup/whitelist/backup_props_all.tsv`).
`[?]` = not proven.

## 0. Big picture (read first)

**Most of these modules touch no hardware at all.** The SoC MMIO they touch is limited to:
`i2c` (0xF2401000), `sircs` (0xF2400000), `gpio_pN` (via kernel `gpio_get/set_data_bit`), `clkrst4`
(clock gate / reset, via `__udif_devio_devclk` and one direct reset sequence in i2c), and the GIC distributor
pending registers (nfc). Everything else is software over the Sony **OSAL message-queue** IPC.

The sub-microcontroller ("front"/power micro), the key ADC, the jog dial, the SIO driver and the RTC-write
path are **not in Linux at all**: they live in **`av-cam.bin`** (`nflasha3/av-cam.bin`, magic `ORIL` =
LIRO, a µITRON RTOS image hosted by `liro.ko`). Proof: every queue ID that input/upm/udate/indctr/nfc *send to*
is registered nowhere in /usr, kernel or initrd, but appears as a literal in av-cam.bin:

| queue (OSAL id) | used by | meaning | literal hits in av-cam.bin |
|---|---|---|---|
| 0x280132 | input `sub_BC` @0xbc | "front_key" — sub-micro key/power-key snapshot | 3 |
| 0x280229..0x280230, 0x28023b, 0x280261, 0x2802df/e0, 0x2803c5/c6 | upm | battery / power / temperature / vgrip from the sub-micro | 2–3 each |
| 0x280152 | indctr `sub_28` @0x28 | LEDs driven by the sub-micro ("front" indicators) | 2 |
| 0x720018 | input `sub_0` @0x0 | "liro_adc" — 24 ADC channels | 13 |
| 0x740017 | nfc `sub_0` @0x0 | SIO transfer service (NFC chip on SIO ch2) | 32 |
| 0x900140 | udate `sub_0` @0x0 | write date to the hardware RTC | 2 |
| 0x90013d | (udate registers it) | udate set/get service, called by av-cam [?] | 4 |
| 0x7a0020 | input `sub_AF4` @0xaf4 (sender 0x410025) | key-event sink consumed by userspace (libObj etc.) | 9 — av-cam posts keys here too (JogDev/JogTpu/CRingDial → dial events) [?] |
| 0x520250 | i2c (server) | I2C transfer service | 9 (av-cam is also an I2C client) |

So: **sub-micro is reached via av-cam (LIRO) ↔ OSAL**, not via a Linux driver; keys reach userspace as OSAL
messages on queue **0x7A0020** (input.ko for ADC/GPIO/front keys; av-cam for the jog dial [?]); userspace
`im.elf` turns them into the uinput `/dev/input/event0` keys. To get real keys/RTC/power state the emulator
must either run av-cam.bin (LIRO) with its peripherals (SIO sub-micro protocol, ADC, TPU jog counter, RTC),
or HLE the 0x28/0x72/0x74/0x90 queues. That work is out of this document's scope but its interface is
fully described below (message layouts as seen by the Linux side).

Other common services used everywhere: 0x910042 = utimer µs counter (`osal_snd_sync_direct`, result
u64 at +24), 0x910041 = delay, 0x910040 = alarm timer (all served by **utimer.ko**); 0x8F003A / 0x8F0313
= logging (ulogio).

## 1. Common UDIF plumbing (kernel)

UDIF resource table `udif_devices` entries, stride 0x5C (VA 0x40379b28 + 0x5C·n): `name[8], PA, size, VA,
?, irqname[16], irq, flags, ?, ?, ?, clk{rst_set, rst_clr, rst_bit, clk_set, clk_clr, clk_bit, ...}`.
Drivers access `dev+20 → entries[ch]`; `+16` = VA, `+24` = IRQ resource, `+56` = clock resource.

| # | name | PA | IRQ | rst set/clr, bit | clk set/clr, bit |
|---|---|---|---|---|---|
| 39 | sircs | 0xF2400000 (0x1000) | 198 | 0xF2A30044/48, 2 | 0xF2A30064/68, 16 |
| 40 | (sircs ch1) | — | 197 "sircs_overun" | — | — |
| 41 | i2c | 0xF2401000 (0x1000) | 199 | 0xF2A30044/48, 1 | 0xF2A30064/68, 17 |
| 53..57 | sio0..4 | 0xF2405000+0x200n | 201..205 | none | none |
| 4..21 | gpio_p0..17 | 0xF2910000+0x100n (0x50) | — | — | — |
| 50 | clkrst4 | 0xF2A30000 | — | — | — |

All these VAs are identity-mapped (VA == PA).

`__udif_devio_devclk(clk, on, x)` (0x4004f9e0): `on=1` → `*clk_set = 1<<clk_bit` (0xF2A30064),
`on=0` → `*clk_clr = 1<<clk_bit` (0xF2A30068). Write-only set/clear registers; the emulator can accept and ignore.

Kernel GPIO (`hw_gpio_*`, 0x4018fd5c..0x40190858), per-port registers (32-bit):

| off | meaning (from accessors) |
|---|---|
| +0x00 / +0x04 / +0x08 | direction: value / set / clear (`set_mode_port` writes +0x04/+0x08) [? which is out] |
| +0x10 | **pin input data** — `gpio_get_data_bit` = `(rd(+0x10) >> bit) & 1` |
| +0x20 / +0x24 / +0x28 | input-enable value / set / clear (`get_mode_bit` reads +0x20) |
| +0x30 / +0x34 / +0x38 | mode/pull value / set / clear (`get_mode_bit` reads +0x30) |
| +0x40 / +0x44 / +0x48 | output latch value / set / clear (`set_data_bit`: +0x44 / +0x48, `set_data_port` writes +0x40) |

`gpiopin_to_irq(group, pin)` (0x4004fb58): group 0, pin 0..15 → IRQ 121+pin; group 0 pin 16 → 144;
group 1, pin 0..6 → IRQ 137+pin.

Procfs: `udif_create_proc` creates entries under the dir made by `udif_init_proc` → `proc_mkdir("udm")`, i.e.
`/proc/udm/<name>` [? path inferred from the string at 0x40314500].

## 2. i2c.ko — SoC I2C master (UDIF driver "i2c_uipc")

Init: `i2cu_probe` @0x204 → `i2c_init` @0x1848: devclk on, `regs = entry->VA` (stored at 0x1CDC),
`request_irq(199, i2c_handler)`, tasklet. Reads Backup **0x520000** (1 byte; user's value 0x00): if nonzero,
speed modes 16/17 are forced to 17. Registers OSAL sync queue **0x520250** (`i2cu_sync_if` @0x858) and a
thread `i2c_uipc` (`i2cu_thread` @0x3d4) for async requests. **Probe touches no I2C register** (only clock +
IRQ), so it cannot fail on missing hardware.

### Register map (all accesses 16-bit at base+off)
| off | R/W | semantics (function+offset of evidence) |
|---|---|---|
| any | R | **bit15 = register busy**: before every write `sub_FE4` @0xfe4 polls bit15 of that register up to 16×2.5 ms; on timeout → error recovery. Emulator: always read bit15=0. |
| 0x00 | R/W1C | status; **bit5 = NACK/no-ack** (tasklet @0x128c: if set, write 0x20, retry ≤3, then -ENODEV) |
| 0x04 | W | command, starts a phase: `0x80` always, `|1` START, `|2` STOP, `|4` TX (bytes from FIFO), `|8` RX (`sub_A8C` @0xa8c, `sub_10AC` @0x10ac) |
| 0x08 | W/R | W: bits3:0 = byte count−1 of this phase. R: **bits7:4 = bytes actually transferred −1**; tasklet requires `((r>>4)&0xF)+1 == count` else -EFAULT |
| 0x0C | R/W | data FIFO (16 deep, one byte per 16-bit access). TX: address byte `(addr<<1)|R` first |
| 0x10 | W | timing 1: 53 (modes 0/17, ~100 kHz) or 9 (modes 1/16, ~400 kHz) [? rates] |
| 0x14 | W | timing 2: 53 or 7 |
| 0x1C | W/R | enable: write 1, poll bit15, then **bit0 must read 1** else -EFAULT (`i2c_xfer` @0x14b0); written 0 at end |
| 0x20 | W | soft reset (written 1 at the end of every transfer by `sub_EC8` @0xec8) |
| 0x24 | W | FIFO clear (written 1 before each phase, `sub_11A0` @0x11a0) |
| 0x28 | R/W1C | **bit0 = phase-complete interrupt status**; IRQ handler @0x1100 reads, writes back `v|1`; `sub_10AC` clears it (write 1) before issuing a command |
| 0x2C | W | interrupt enable (1 at transfer start, 0 at end) |
| 0x34 | W | 1 at start (modes 0/1/16/17), 0 at end [? bus monitor] |
| 0x38 | R | bus line state; **must read 3** (SCL=SDA=1) before a mode-0/1 transfer, else -EIO |
| 0x18, 0x30 | — | never touched (register dump `sub_DEC` skips them) |

Error/end path `sub_EC8`: `0x34=0`, `0x20=1`; if bus-check disabled (modes 16/17): clkrst4 `0xF2A30048=2,
0xF2A300B8=8, 0xF2A30044=2, 0xF2A300B4=8` (reset assert/deassert of I2C, bit1 and a second block bit3) [? polarity].

### Transfer protocol (what the model must do)
Per message chain (`i2c_xfer`): enable, timing, then per phase: clear FIFO (0x24), push TX bytes, write count−1
to 0x08, write command to 0x04 → controller performs START/addr/data/STOP, sets 0x28 bit0, raises IRQ 199.
A read message = phase 1 `START|TX` with only the address byte, then phases `RX` (≤16 bytes each), last has STOP.
A write message = `START|TX` with address + up to 15 data bytes, further `TX` chunks, last has STOP. Each new
message starts with START (repeated start). Absent slave → set 0x00 bit5, still complete (IRQ). Transfer times out
after 500 jiffies without IRQ.

### Client message format (queue 0x520250)
`u16 nmsgs; u16 speed_mode(0,1,16,17); i32 result; { u16 addr7; u16 flags(bit0=read; 0x7010/0x800 → -ENOSYS); u32 ?; u32 len(≤0x7EC); u8 data[len] } × nmsgs`.
Result bytes: 0 ok, 2 -EBUSY/timeout, 3 -EINTR, 4 -ENODEV(NACK), 6 other. Clients: accel.ko, mag.ko, av-cam.bin,
userspace `adjstctl.elf`, `libsencore.so`, `libObj.so`, `libInfraMediaCommon.so` (bus devices they address not
analysed here [?]).

## 3. accel.ko — Freescale MMA8452Q accelerometer (I2C 0x1C) — disabled on a5000

`.rodata` register symbols: WHO_AM_I=0x0D, OUT_X_MSB..OUT_Z_LSB=0x01..0x06, FF_MT_CFG=0x15, FF_MT_THS=0x17,
FF_MT_COUNT=0x18, CTRL_REG1..5=0x2A..0x2E, `I2C_SLAVE_ADDR`=0x1C. Expected WHO_AM_I = **0x2A** (`acc_api_cb`
@0xf78, compares `v44 == 42`) → MMA8452Q. Data: 12-bit left-justified (`sub_630` @0x630, clamp ±1026).
Per request it first writes CTRL_REG2 (0x2B) ← 0x40 (RST, `sub_9B0` @0x9b0 → `sub_7E4(43, 0x40)`), waits, then reads WHO_AM_I.

`accel_init` @0xeb4: reads 19 Backup props 0xA30000..0xA30013 into .bss (`sub_C80` @0xc80); registers OSAL
queues 0xA30281/0xA30282; creates the polling thread only if Backup **0xA30011 == 1** — user's value **0x00**.
**No I2C traffic at init**; I2C only happens if userspace sends a request on 0xA30281/2 (speed mode 16, retry
0xA30002=3, delay 0xA30001=20 ms). Emulator: nothing required; optional MMA8452Q model.

## 4. mag.ko — AKM AK8975-class magnetometer (I2C 0x0C) — disabled on a5000

Registers: WIA=0x00 (expect **0x48**), ST1=0x02 (DRDY bit0), HXL..HZH=0x03..0x08, ST2=0x09 (DERR=4, HOFL=8),
CNTL=0x0A (power-down 0, single 1, fuse-ROM 0x0F), ASTC=0x0C, ASAX..Z=0x10..0x12. `mag_init` @0x1534 reads
Backup 0x1110000..0x1110009, then `sub_B5C` @0xb5c probes only if Backup **0xA40000 == 1** (user's value
**0x00**) → **no I2C at init**. Probe sequence when enabled: write CNTL(0x0A)=0 and ASTC(0x0C)=0 (`sub_A30` @0xa30), read WIA (expect 0x48,
50 retries).

## 5. compass.ko — pure software
API callback `cmps_api_cb`; no hardware, no Backup. Nothing to emulate.

## 6. sircs.ko — SoC IR-remote (SIRCS) receiver

UDIF ch0 = 0xF2400000 IRQ 198 (`sircs_rx_register` @0x1218), ch1 = IRQ 197 "sircs_overun"
(`sircs_ovr_register` @0x13ac), both → `sircs_intr_handler` @0xde0. Registers 32-bit:

| off | semantics |
|---|---|
| 0x00 | control: `sircs_start` @0xfcc writes 0x654C; on overrun bit6 is cleared then set (FIFO reset); stop writes 0 |
| 0x08 | 0x12 at start [? clock divider] |
| 0x10 | status: loop ends when `(r & 0x90) == 0x10` (bit4 = FIFO empty) |
| 0x14 | FIFO: bits15:0 = pulse width, bit16 = level |
| 0x18 | interrupt status: must read **2** (data) else "FIFO overrun" path writes 0 |
| 0x1C | 1 at start = IRQ enable |
| 0x20 | 0xA01 at start |

Pulse classes (`.data` 0x94..0xC0): mark (bit16=0) 0..0x23 → "0", 0x24..0x40 → "1", 0x48..0x70 → guide;
space (bit16=1) 0x10000..0x10FFF → data-off. ~26 µs/unit [?] (SIRC: 0.6/1.2/2.4 ms). Decoded codes are
debounced (`sircs_event` @0x8fc, 10-jiffy timer) and sent as `{3=press|4=release, code}` to OSAL queue
**0x45014A**; app control via sync queue 0x7D0049. **Start happens only when an app enables reception** (via
0x7D0049), probe touches only IRQ registration. `/proc/udm/sircs` (eventcnt/prevcode). Emulator: register file
that reads idle (0x10 bit4 = 1, 0x18 = 0, no IRQ); optional IR injection.

## 7. input.ko — key scanner (UDIF "input")

`kbd_init` @0x38f0: reads key table **Backup 0x1120000** (4120 B, magic "BIN!") — fails with -12/-22 if
backup.ko has no data. `kbd_probe` @0x36e8 creates `/proc/udm/input` (`table|get|adc|front|gpio|vgrip`) and
starts polling timers (`sub_3238`); `kbd_timer_hndlr` @0x20f0 runs per period class (10/20/100 ms).
Entries are 64 bytes: `id, src(1=adc,2=gpio,4=front,7=vgrip), port, bit, active_low, lo, hi, period_ms,
debounce_n, keycode, ?, opt(+44 u16), multi-ref(+44/+45), multi-type(+46), check-only(+47)`.

Key table actually loaded on the real camera (snapshot, `dword_54B8`=0x4c3a6000, identical to Backup.bin):

| src | port/channel | bit | condition (pressed) | keycode | opt |
|---|---|---|---|---|---|
| ADC | ch 0x0C | — | 0..91 | 0x1F8 | 1 |
| ADC | ch 0x0D | — | 0..91 / 92..273 / 274..439 / 440..599 / 600..772 | 0x1AC / 0x03 / 0x15 / 0x30 / 0x1AF | 1 |
| ADC | ch 0x0F (multi) | — | 80..244 / 245..358 / 359..442 / 443..514 / 515..1023 | 0x1BF (one key, value) | 2 / 0x8002 / 0x8001 / 1 / 0 — zoom-lever-like 5-position [?] |
| GPIO | **P5** | **2** | pin == 0 (active low) | 0x70A6 | 1 |
| GPIO | **P10** | **10** | pin == 0 (active low) | 0x1F0 | 1 |
| front | CAkey1 (0x6000) | 0 | bit == 0 | 0x0002 | 1 |
| front | CAkey1 | 1 | bit == 1 | 0x01F7 | 1 |
| front | CAkey1 | 2 | bit == 1 | 0x01AB | 1 |
| front | CAkey1 | 5 | bit == 1 | 0x70D5 | 1 |
| front | CAkey1 | 9 | bit == 0 | 0x7094 | 1 |

ADC/GPIO debounce = 4 consecutive 10 ms samples, front = 1. Keycodes are Sony internal codes; mapping to the
Linux codes of event0 (MENU=514 …) is done by userspace [?].

Sources:
* **GPIO** — `gpio_get_data_bit(port, bit)` → GPIO port +0x10. **Released state must read 1** on P5.2 and P10.10.
* **ADC** — sync msg to **0x720018** (`sub_0` @0x0): msg `u32 0xFFF1; u8 group(0|1)`; reply `u16 ad[12]` at +6
  (group 0 = ch 0..11, group 1 = ch 12..23), 10-bit values. Served by av-cam (liro_adc). Released ⇒ value above
  every range (ch 0x0C/0x0D > 772, ch 0x0F inside 515..1023 = "position 0").
* **front** — sync msg (24 B) to **0x280132** (`sub_BC` @0xbc, decode @0x2500): `[2]=error`, `[4]` valid mask
  (bit0 power keys, bit1 jack AD, bit2 ACV/batt), `[5]` PwrKey bits (src ports 0x4001..0x4004 test bits 2/3/4),
  `[6]` JackAD (port 0x1000 analog), `[7]` ACVBatt bit0 (port 0x2000/0x8000), `[8..23]` CAkey1..4 (u32, ports
  0x6000..0x6003). Served by av-cam (sub-micro).
* **vgrip** — 0x18A04D1, no entries on a5000.

Output (`sub_AF4` @0xaf4): async msg 16 B to **0x7A0020**, sender 0x410025: `{u32 keycode, 0, u32 value(1/0 or
multi value), u32 opt}`.

## 8. indctr.ko — LEDs/indicators

`indR_get_dev_list` @0x1b8 reads **Backup 0x1120002**; entries 20 B `{id, type(0=GPIO,1=front,2=console), port,
bit, polarity}`. Real camera (snapshot `dword_1AD0`, = Backup.bin): **id 2 → GPIO P0 bit 0, pol 0**;
**id 5 → GPIO P2 bit 6, pol 1**. GPIO type uses `gpio_set_data_bit` (P0/P2 +0x44/+0x48); type 1 sends `{op, led,
?, val}` (6 B) to av-cam queue 0x280152. Blink via utimer alarms (0x910040). Sync API queue 0x4E013B. No
probe-time hardware dependency besides GPIO writes. `/proc/udm/indctr`.

## 9. nfc.ko — NFC front-end (UDIF "nfc_rsrc")

Pins/params from Backup 0xF904D0.. (user's values): SIO channel **0xF904D2 = 2** (sio2) and mode 0xF904D3 = 1;
power/CS GPIO **P7.14** (0xF904D6/D7) toggled after each transfer (`sub_394` @0x394); power-off GPIO **P7.10**
(0xF904D8/D9); IRQ pin "nfc_rsrc_irq" = gpiopin (0,4) = **IRQ 125**; RF-detect "nfc_rsrc_rfdet" =
(0,8) = **IRQ 129** (`nfc_rsrc_request_irq` @0xf98); approach sense = gpio_get_data_bit on P0.8 [? the group/pin
pair is passed as port/bit]. It writes the GIC distributor directly: `0xF8001280 + 4·(irq>>5) = 1<<(irq&31)`
(ICPENDR, clear pending) and `0xF8001200 + …` (ISPENDR, *software-raises* the RFDET IRQ in
`nfc_rsrc_set_irq_rfdet` @0xd7c). SIO data go through av-cam's SIO service **0x740017** (64-byte request,
callback `sio_comm_cb`, 3000-jiffy timeout) — nfc.ko never touches SIO MMIO. Backup 0x1820000 bit4 selects an
alternate SIO param set (0xF9054B/4C). Init needs no hardware response (only IRQ registration, which succeeds
for any SPI).

## 10. udate.ko — software wall clock (no RTC hardware access)

There is no RTC driver here. udate keeps `base_date (UTC, struct {u16 y; u8 mon,day,h,m,s; u16 flag})` + `base_us`
(utimer counter 0x910042) and computes `now = base + (counter−base_us)/1e6` (`udate_get_localtime` @0x508),
clamped to ≥ default date and ≤ year 2037. Backup props (user's values): TZ offset 0x900001 = 0x021C (540 min,
+9:00), DST 0x900000 = 0; default date 0x90000E/0F (2000+0x0E = 2014), 0x900008 month 1, 0x900004 day 1,
0x900006/07/09 = 00:00:00, default TZ 0x90000A/0x900005; 0x900010 "date set" flag = 0; 0x51000F boot mode.
Real-camera snapshot: base = 2013-12-31 15:00:00 UTC (= 2014-01-01 00:00 +9h, the default — clock never set)
captured at base_us = 0x52AB18 (5.4 s after boot).

Setting the date (`udate_set_date` @0x918, via sync queue **0x90013D**, `udate_kif_setget` @0x1144) forwards the
date to **0x900140** (`sub_0` @0x0, `{u16 0; u16 status; date[10]}`) = av-cam's RTC writer, unless boot mode
(0x51000F) == 12 (BIS). Change notifications go to registered queues (0x90013F) and 0x904142. Mode 4 =
"resync from hardware" path used at boot/resume [? caller = av-cam reading the sub-micro RTC]. Module params
`udate_year..udate_unset` exist but init passes none. `/proc/udm/udate`.
**The hardware RTC is in the sub-micro behind av-cam.** For the emulator: without av-cam the clock stays at the
default date + uptime [?]; to show a real time, HLE a `udate_set_date` call (queue 0x90013D, op 0) or emulate
av-cam's RTC read.

## 11. upm.ko — power-management policy (UDIF "UPM")

Pure policy: battery level/percent/minutes, heat alerts, auto-power-off (MSS timers), off-factor sequencing,
DFS (CPU clock, `k_dfs_do_full_clk/half` → 0x924045), boot-factor bookkeeping, kikilog counters. **No MMIO.**
Hardware facts come from the sub-micro via av-cam subsystem 0x28 (`k_upm_init_battery` @0x7adc):

| sync request | size | meaning |
|---|---|---|
| 0x280229 | 20 | battery type/presence (`byte_14A4E==1 && byte_14A50==0` → "no InfoLITHIUM battery") |
| 0x28022A | 26 | battery state |
| 0x28022B | 4 | battery authentication |
| 0x28022C | 8 | unregulated voltage |
| 0x2802DF | 4 | box temperature |
| 0x2803C5 | 2 | vgrip state |
| 0x28023B | — | `k_upm_snd_front_enddata` (power-off hand-shake) |

and async notifications registered on 0x28022D/2E/2F/30/2E0/3C6 (+0x280261). Requests to an unregistered queue
fail and are simply skipped (state stays default) [? whether the APO/low-battery logic then shuts down].
Published services: 0x9241C2/0x9243D8 battery level (broadcast), 0x924223 heat alert, 0x924126 power status,
0x924220 MSS, 0x924222 battery check, 0x924224 heat check, 0x924127 boot factor, 0x920021 upm task,
0x920277 DFS, 0x92026F "senser" (factory). Off requests go to 0x920168/0x920169/0x94016A.

Boot factor (`k_upm_setup_boot_factor` @0xbf88): Backup **0xFA0029 holds a physical pointer = 0x8002104C**; upm
reads the u16 there via `arch_phys_to_noncache` (real snapshot: 0x00000007). Also reads 0x510013 / 0x510014
(boot-factor bits, user's file: 0x00000001 / 0xFFFFFDF6), **0x51000F = loader boot mode**
(1 COLD, 2 HOT, 3 UPDATER, 4 CIPA, 5 ADJUST, 6 USB_CHARGE, 10 WARM, 11 SS, 12 BIS; snapshot = 12, user's
Backup.bin = 2), 0xFA0018 (→ 0x80021064, snapshot 0x610). These live in the bootloader hand-off page
`wbimem=0x1000@0x80021000` (also `pm.resume=0x80021054`, `pm.sus_error=0x80021060`). `/proc/udm/upm_bootmode`.

## 12. utimer.ko — OSAL timer services
Wraps kernel `hwtimer_*` (CXD4115 channels hwtimer0..3 = 0xF2403500..800, IRQ 158..161 — already modelled):
`utimer_get_syscnt` / `get_schedclock` from `hwtimer_read_clock_cpu(0)`; serves 0x910042 (counter, op0 =
syscnt, op1 = schedclock), 0x910040 (alarm timers), 0x910041 (delay). Thread "utimer". Nothing new to model.

## 13. backup.ko — Backup (settings) store

Not a hardware driver. `backup_init` @0x9268: cdev "backup" (`udif_cdev_register`), select `/setting/Backup.bin`
else `/boot/Backup.bin` (`backupfile_select_path`), then `backupfile_load(Backup_load)` — init passes
**`Backup_load=0`**. `backupfile_init` @0x4250 reads only the first 256 B of the file: `u32 phys_addr; u32 cookie
0xAAAAAAAA; …; "BK4" at +12`. It then uses **the copy the bootloader already placed in RAM at phys_addr**
(no reload when Backup_load != 1) and re-checks "BK4"/cookie there (`Data_init` @0x2f18). The user's
Backup.bin header says **phys 0x82EC0000** (size 780971 B), inside `memrsv=0x13A7000@0x82D90000`.
Lookup: `Data_SearchId` @0x308c (6-byte index records, subsystem = id>>16). "senser" factory protocol and
`wbi_drop` are extra. If this fails, **input, indctr, udate, upm, accel, mag, nfc, i2c all lose their config**
(input.ko init fails with "key table size check error").

## 14. What /dev and /proc entries appear
`/proc/udm/{input, indctr, sircs, udate, utimer, upm_bootmode}` [? dir name], cdev `backup` (node via devno),
OSAL queues listed above. No /dev/input from these modules — `/dev/input/event0` is created by userspace
(im.elf uinput).

## 15. Open items
* Who loads `liro.ko` and with which `fn=/adr=` (av-cam.bin) — it is a registered UDIF driver ("liro" in the
  stock suspend log) [?].
* Exact jog-dial path (av-cam JogTpu → 0x7A0020) [?]; Sony keycode → event0 mapping (userspace).
* Which I2C slaves av-cam/userspace address on the a5000 bus.
* `Backup_read_4Byte(0xFA00A5)` (udate init) — property not in Backup.bin; value semantics [?].

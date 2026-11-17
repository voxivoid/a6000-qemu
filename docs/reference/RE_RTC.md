# RE: real-time clock (RTC)

Scope: where the a5000 keeps wall-clock time, who reads and writes it, why the emulator always shows the clock as
"not set", and what the emulator needs to model. Method: static (objdump of `udate.ko`, IDA 9.4 + Hex-Rays on a copy
of `smc_avcam.i64`, existing `re/ida/kmod_i2cgrp/udate.c`, `re/ida/libObj.so.c`) plus dynamic runs of the
unmodified firmware with the `a5ktrace` plugin and gdb. `[?]` = not proven.

Related: `RE_SUBMICRO_LINK.md` (transport, slot A/B), `RE_INPUT_I2C_POWER.md` §10 (first notes on udate.ko),
`BOOT.md` (hand-off page).

---

## 0. TL;DR

* **The battery-backed clock lives in the sub-micro** (the power/front microcontroller). There is no SoC RTC block,
  no kernel RTC driver and no I2C RTC chip in this path. The clock travels in the **128-byte slot-A packet**
  (task 48 in av-cam):
  * micro → CPU, **RX slot A bytes 6..12** = `{status, yy, mon, day, hour, min, sec}`. The values are binary
    (not BCD), the time is **UTC**, and the year is 2000 + yy. `status == 2` means "clock valid".
  * CPU → micro, **TX slot A bytes 4..10** = `{0x02, yy, mon, day, hour, min, sec}` (UTC): the "set clock" request.
* At run time the wall clock is **software**: `udate.ko` keeps `base_date + (utimer µs − base_us)`. The micro
  clock is read **once at boot** (av-cam "BootClock_Snd" → udate, mode 4). It is written once each time the user
  confirms a new date (udate → av-cam queue 0x900140 "ClockReq_Rcv" → TX slot A).
* **Why the emulator says "not set":** the factory hand-off page that the board preloads has
  **`*(u32 *)0x800210E4 = 1`**, and Backup **0x900010 = 0**. With these values `udate_init_clock` marks the stored
  base as **UNSET (flag 2)**, and the boot-time clock from the micro is forced to UNSET too. libObj sees UNSET and
  starts the first-boot "area/date/time" setup on **every** boot. Verified: with that word patched to 0, udate stores
  the micro's clock as valid UTC and the camera boots straight to the shooting screen (§5).
* Even when it is accepted, the micro clock is **frozen**. The HLE replays the snapshot slot A, which says
  2013-12-31 15:34:01 UTC. The HLE also ignores TX, so a date the user sets is lost at the next boot.
* **The blank "Date/Time" row** in the setup list (after editing the date and before the final "OK") is **not**
  produced by the clock path. No udate call happens while that row is drawn. After the final "OK", udate holds the
  new date correctly (UTC, flag 1). See §6 (open).

---

## 1. Where the clock lives

| Layer | Component | Evidence |
|---|---|---|
| hardware | sub-micro RTC, reached only through the SIO sub-micro link | slot-A field parsers below; no RTC MMIO in `av-cam.mmio.tsv`; `udate.ko` has no MMIO |
| av-cam (µITRON, LIRO) | task 48 slot-A field codec. RX field 5 = boot clock, TX field 4 = clock write | `43AC65E0`, `43AC65FA`, `439120DC`, `43AC663E`, `43AC5EB0` |
| kernel | `udate.ko` software wall clock + time zone (OSAL queues 0x90013D set/get, 0x90013F notify) | `udate_set_date` @0x918, `udate_get_localtime` @0x508 |
| userland | libObj `UDATE_MWF` / `M_CLOCK` model (states NORMAL_SETTLED / FORCE_SETTLED / UNSETTLED / NOTIME) | strings `[M_CLOCK] ...`, `sub_A67234` |

No I2C address and no SoC register are involved.

### 1.1 Slot-A clock fields (128-byte task-48 packet, CRC as in `RE_SUBMICRO_LINK.md` §3)

**RX (micro → CPU, packet type 0x20).**
* `43AC65E0` copies RX bytes **6..12** into task-48 state `0x43E5581E + 43..49`.
* `43AC65FA` (pointer at `0x43ED46E4`) decodes them into the RX logical state `*0x43ED44B0 = 0x43ED43B0`:

```
+8  u16 year = 2000 + rx[7]      +10 mon = rx[8]   +11 day = rx[9]
+12 hour = rx[10]  +13 min = rx[11]  +14 sec = rx[12]
+16 u16 flag = (rx[6] == 2) ? 1 /*UTC*/ : 2 /*UNSET TIME*/
```

  It then marks field 5 updated (`sub_43910564(5)`).

**TX (CPU → micro, packet type 0x21).**
* `43AC663E` (field 4) writes staging `0x43E5581E + 218..224` as `{2, (year_lo + 0x30) & 0xFF (= year − 2000),
  mon, day, hour, min, sec}`. It is called after `43AC66C2` stored a ClockReq "set".
* Observed in the live TX packet (task-48 TX buffer `*0x43E0C9A4` = PA 0x8338201C, and the link TXbuf PA 0x833443A0)
  after the user set 2015-01-01 00:00 local (+9:00):

```
21 00 00 00 02 0e 0c 1f 0f 00 00 ...   → TX bytes 4..10 = {02, 14, 12, 31, 15, 00, 00} = 2014-12-31 15:00:00 UTC
```

**Acknowledgement.** The TX builder `43914E28` (around `0x43915080`) clears pending TX field 4 only when all of the
following hold:
* RX state byte 43 (= rx[6]) has **bit 1** set;
* `memcmp(staging+219, state+44, 6) == 0`, i.e. the micro **echoes the written yy..ss** in rx[7..12].

Until then the request stays pending. Whether TX byte 4 drops back to 0 after the ack is unknown [?].

**Snapshot RX slot A** (`data/submicro_frames.bin` +0) bytes 6..12 = `02 0d 0c 1f 0f 22 01`. So the factory micro
reports a valid clock of **2013-12-31 15:34:01 UTC**. That is the udate default (2014-01-01 00:00 at +9:00) plus
34 minutes: on that unit the clock was never set by a user.

---

## 2. Read path (boot)

1. av-cam task 48 parses each RX slot A (`439149F8` → field parsers → `43AC65FA`).
2. The per-field dispatcher `43AC6F52` calls the RX handler table at `0x43ED4860` (8 bytes per id). Field 5 =
   **`439120DC` BootClock_Snd**. Its behaviour depends on the per-field mode (`0x43E0C554 + 16·id + 13`):
   * mode 2: send once, then mode 0;
   * mode 3: send whenever the value changes;
   * mode 1: send once.

   In the trace it fires **once**, at the first valid frame.
3. `4391203C` sends `{op 0, date[10], area-valid = 1 (no area), mode 4}` on sync queue **0x90013D** → `udate_kif_setget`
   @0x1144 → **`udate_set_date(date, NULL, 4)`** ("resync from hardware"). It logs task-48 id 0x82.
4. `udate_set_date` mode 4 (`0xABC..0xB78`):
   * if `byte_20BE` (= Backup **0x900010**, only read when the hand-off word ≠ 0) is set: load the Backup default date
     (0x90000E/0F/08/04/06/07/09), convert it to UTC, **write it back to the RTC** (`sub_0` → 0x900140), write Backup
     **0x90000D = 0**, then clear `byte_20BE`;
   * otherwise: **if the stored base flag (`word_20A8`) is 2 (UNSET), force the incoming flag to 2**;
   * store `base_date`/`base_us` (`sub_2E0`) and notify (0x90013F list + 0x904142).
5. From then on `udate_get_localtime` returns `base + elapsed + TZ` (TZ 0x900001 = +540 min, DST 0x900000). It
   returns the raw value without conversion when the base flag ≠ 1 (UTC). It clamps to [default year, 2037] and
   rewrites the RTC when out of range.
6. libObj `sub_A67234` (UDATE_MWF, date-change handler) reads the clock (op 2):
   * flag 2 (UNSET) → `sub_A67DE0(1,1)`;
   * flag 0 (LOCAL) → clears Backup 0x90000D if it is set.

   `M_CLOCK onGetInfo(INFO_CURRENT_LOCALTIME)` (`0x40514C`) reports "valid" only for flag 0. UNSET drives the
   first-boot area/date/time setup.

**`udate_init_clock` @0xBBC.** The base flag starts at 3 (NO DATA). Then:

```
v = *(u32 *)Backup_read_4Byte(0x00FA00A5)   ; property value = 0x800210E4 (a PA, identity-mapped)
if (v == 0 && no udate_unset param) -> keep NO DATA (micro clock accepted as is)
else if Backup 0x900010 == 0         -> base flag = 2 (UNSET TIME)
else                                 -> byte_20BE = 1 (re-initialise RTC to default at mode 4)
```

`0x800210E4` lies in the bootloader hand-off page (`wbimem=0x1000@0x80021000`, next to `wbi_memclr=0x800210e8`).
Nothing in the kernel, the modules, av-cam or `sabin/` references it, so the bootloader writes it. Its meaning is
"RTC contents not trustworthy / clock lost" [?]. Snapshot: `0x800210E0 = bf810000 00000001 00000001`.

## 3. Write path (user confirms date/time)

Traced end to end (set 2015-01-01 00:00 local):

```
libObj M_CLOCK ActionSetDateTime / ActionSetInitialSetting   (osal_set_date)
 → queue 0x90013D op 0: {local 2015-01-01 00:00:00 flag 0, area {TD 540, DST 0}, mode 1}
 → udate_set_date(date, area, 1): sub_46C local→UTC (flag 1) ; sub_0 → av-cam queue 0x900140 {op 0, UTC date}
     → 43AC5EB0 ClockReq_Rcv: 43AC66C2 copies into TX state 0x43ED44B4+8.., sub_4391037C(4); status 0 at once
     → next tick: 43AC663E encodes TX slot A bytes 4..10, sent on the SIO link
 → sub_2E0 stores base (2014-12-31 15:00:00, flag 1 UTC), TZ → Backup 0x900001/0x900000, notify
```

`sub_0` returns as soon as av-cam has queued the request. It does not wait for the micro. udate is therefore already
correct for the session even without a micro model. The RTC write is skipped completely in boot mode 12 (BIS,
Backup 0x51000F; `byte_20BF`). ClockReq op 1 (`43AC669C`) returns the RX clock (`*0x43ED44B0` + 8..17).

## 4. udate structures (`.data`; live base 0x3F088E48 with udate at 0x3F087000)

| off | field |
|---|---|
| +0x00 | base date `{u16 year; u8 mon, day, hour, min, sec, pad; u16 flag}`. flag: 0 LOCAL TIME, 1 UTC, 2 UNSET TIME, 3 NO DATA (strings in `udate.ko`) |
| +0x10 | u64 `base_us` (utimer 0x910042 counter) |
| +0x18 | TZ offset (min), +0x1A DST (min), +0x1C area flag |
| +0x1E | `byte_20BE` re-init-RTC flag, +0x1F `byte_20BF` BIS mode |

Live values in the emulator after boot (stock data): `07dd 1f0c 220f 0001 0002` = 2013-12-31 15:34:01, **flag 2**,
TZ 0x021C.

## 5. Why the emulator shows "not set" (verified)

| run | 0x800210E4 | udate_set_date mode 4 in → stored | UI |
|---|---|---|---|
| stock data (snapshot hand-off) | 1 | flag 1 → **flag 2** (`udate_store` probe) | "Set area/date/time" prompt; list shows "Not set" |
| `A5K_HANDOFF` with the word = 0 | 0 | flag 1 → **flag 1** | boots directly to the shooting screen |

The sub-micro HLE has a second problem: it replays a fixed slot A and ignores TX. As a result:
* the clock never advances between boots (always 2013-12-31 15:34:01 UTC);
* a user-set clock is never echoed, so TX field 4 stays pending, and it is lost at power-off.

## 6. The blank "Date/Time" row (open)

Reproduced: area → Date/Time → edit → Enter. The list row "Date/Time" turns blank. It showed "Not set" before.
* During editing and redraw, no `udate_kif_setget` / `udate_set_date` call happens (probes on 0x3F088144/0x3F087918).
* `PAS_LabelDate::setDate`, `sub_2E3C40` and `setDummyDate` in `viewUnified2.so` (mapped at 0x14349000 in im.elf,
  so +0x2E3E3C, +0x2E3C40, +0x2E3C2E) are not called either.
* After the final "OK", `udate_set_date(…, 1)` runs and the stored base is valid (flag 1).

So the row is drawn from UI-side temporary data, and the empty text comes from the UI/text layer, not from the RTC.
Whether the real camera shows the date there is unconfirmed [?]. Suggested next step for the UI side: find the
widget behind the `cmnViewSettingNodeRootClockDateTimeSetup` list row and trace its text and variant formatting.

---

## 7. Minimal emulation plan (implemented, see §9)

**Where:** the sub-micro HLE in `hw/arm/sony_a5000.c` (`a5k_submicro_xfer`, which already owns the slot-A replay
buffer and `a5k_submicro_crc`). Do not add a new MMIO device, because there is no RTC hardware on the CPU side.

1. **RX clock (every slot-A reply).** Before copying slot A into `buf`, rewrite bytes 6..12 of the reply and recompute
   the CRC:
   * `rx[6] = 2` (valid);
   * `rx[7] = year − 2000`, `rx[8..12] = mon, day, hour, min, sec`. All binary, **UTC**.

   Time source: `qemu_get_timedate(&tm, s->rtc_offset)` (honours `-rtc base=utc|localtime|<date>`, as pl031 does),
   so the host time feeds in. Keep the year in 2014..2037: udate clamps to [Backup default year, 2037] and rewrites
   the RTC otherwise.
2. **TX clock write.** In the same callback the CPU's TX packet is in `buf` before the copy. If `buf[0] == 0x21`,
   `buf[4] == 2` and `buf[5..10]` differ from the last write applied, then:
   * set `s->rtc_offset` so that the RTC now reads `buf[5..10]` (UTC, year 2000 + `buf[5]`);
   * report exactly those six bytes in the next RX (so the `43914E28` ack matches), and tick from there on.

   Apply the write on change only. TX byte 4 may stay 2, and re-applying it every frame would freeze the clock [?].
   Persistence: the offset lives for the session (it survives the guest reboots that `a5k_boot_reset` handles). To
   keep it across QEMU runs, add a machine property (e.g. `rtc-offset=` / a small state file) or let users pass
   `-rtc base=…`. The Backup store is not involved: on the real camera the micro keeps the time.
3. **Hand-off word.** For a "normal" power-on, the board should present `*(u32 *)0x800210E4 = 0` (clock retained).
   Keep 1 only to model a lost backup battery: that gives UNSET plus the first-boot prompt, as today. This is a
   board/data change (`data/handoff_80021000.bin` is the factory snapshot, see `BOOT.md` caveat). Patch only this
   word in `a5k_boot_reset`, with this document as the evidence, and expose it as a machine option if both
   behaviours are wanted.
4. With 1 + 3, the boot clock is the host time, the first-boot prompt goes away, and a user-set time persists for the
   session. With 2, the clock write is acknowledged as on the real camera.

## 8. Open questions

* Exact meaning of hand-off word 0x800210E4 (written by the bootloader, probably from the micro's "RTC lost" status)
  and of Backup 0x900010. The code paths are proven; the names are guesses [?].
* What rx[6] values other than 2 mean, apart from "not valid" (e.g. a bit 1-only ack versus a full status byte) [?].
* Whether TX byte 4 is cleared after the ack, and whether the micro also needs TX field 4 outside a user set (e.g.
  the udate clamp/re-init path) [?].
* Field-5 mode handling on resume (`udate_snd_resume_notify`, task-48 mode 2/3) was not traced.
* The blank Date/Time list row (§6) belongs to the UI layer.

---

## 9. Implementation status

Implemented in `hw/arm/sony_a5000.c` (sub-micro HLE), following §7.

* **RX clock.** Every slot-A reply is built from the snapshot frame with `rx[6] = 2` and `rx[7..12]` =
  `qemu_get_timedate(&tm, rtc_offset)` (UTC with the default `-rtc base=utc`), year clamped to 2014..2037, CRC
  recomputed. `-rtc base=<date>` moves the camera clock.
* **Clock write.** TX slot A `buf[0] == 0x21`, `buf[4] == 2`: bytes 5..10 are applied once per request
  (`rtc_offset` = written time − current QEMU RTC time) and echoed verbatim in the next replies.
  **Observed:** after the ack the CPU sends TX byte 4 = 0 again (answers the §8 question). The board stops echoing
  then and re-arms, so a later write of the same value is applied again. The echo is capped at 20 replies
  (hypothesis, about 1 s).
* **Hand-off word.** The preloaded hand-off ROM blob is patched after loading: `0x800210E4 = 0` by default,
  `= 1` with the machine option **`rtc-lost=on`** (clock backup lost → first-boot area/date/time setup).
  `data/handoff_80021000.bin` is unchanged.
* **Session only.** The offset lives in the QEMU process. It is not saved across QEMU runs. To start at a fixed
  time, use `-rtc base=…`.

Verified (unmodified firmware, pristine NAND, no SD):

| run | result |
|---|---|
| default | boots straight to the shooting screen. Setup → Date/Time editor shows the host local time (TZ +9 from Backup). Setting month 11 → 12 logs `sub-micro RTC set to 2026-12-14 11:40:00 UTC`; RX slot A then ticks from that time and the editor shows the new date when reopened |
| `rtc-lost=on` | first-boot "Set area/date/time" dialog → PlayMemories notice → shooting screen; MENU renders |

Still open:
* The "Date/Time" list row in Setup → Date/Time setup is **blank** when the clock is valid (it read "Not set"
  before). This is the §6 UI-layer issue and is not caused by the clock path.
* Guest `system_reset` from HMP does not get past stage 1 in this build. That is unrelated to the clock, so
  persistence across a guest reset was not checked.

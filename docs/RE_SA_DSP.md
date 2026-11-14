# SA DSP cores: manager protocol, completion path and a "null SA" model (fw 1.10)

Target: `av-cam.bin` (LIRO µITRON image, VA 0x43541000, Thumb-2). Addresses are av-cam VAs unless noted.
Method: Hex-Rays on a private IDB copy (`C:\a5000\emu\sa\sa.i64`, copied from `sdrepair\gavcam.i64`), plus one emulator
run (`r1`, pristine NAND, `a5ktrace fn=` probes on the SA manager, `-d unimp,plugin`, PLAY pressed at the shooting
screen). `[?]` = not proven.

Related documents: RE_LIVEVIEW_REQUEST.md §3–§4 (found the same SA0 start independently; this document confirms and
extends it), RE_SD_MEDIA_SERVICE.md §3.3 / §7.2 (gate A), RE_AVCAM_IRQ_BOOT.md §1.1 (ISRs), RE_USERLAND_SUBCPU.md §3
(sabin, kernel `cmpr`), RE_AUDIO.md §2.2 (codecs, LPS).

---------------------------------------------------------------------------------------------------------------------

## 0. Summary

* **Both open gates are the same SA call.** The `liro-kliro_90` stack in RE_SD_MEDIA_SERVICE §3.3
  (`4398EA5B ← 4398ECD7 ← 43990C81 ← … 4398DDC5`) is inside **`ZIT ConvZ1C` `43990B2C`**, the imager's sensor
  defect-table conversion. It is not face detection. RE_LIVEVIEW_REQUEST §3 found the same call. "ZIT" is the imager
  defect tool; face detection is a different client (FDT, §6).
* There are two layers:
  1. **Clients** (ZIT, FDT, JPEG, codecs, NR, …) talk to the **SA manager task 44** (`439232BC`, queue 0x101) with
     OSAL messages. Open, load and close are pure software: the program is loaded into DRAM by the userland
     **libOnDemandLoader** (ODL). The emulator already completes them (§3).
  2. **The hardware** is touched only on a function start (cmd 0x41/0x44). The manager programs a core and writes GO.
     The completion comes back **only** through the core's **source-A interrupt** (IRQ 51/54/61/64). The ISR turns it
     into an event that task 44 converts into the client's result message.
* **Only one SA start happens during boot to the shooting screen**: ZIT ConvZ1C on **SA0 (0xF34A0000)**, program id 2.
  It blocks forever because no IRQ 51 comes. PLAY added no further SA access, because the camera user is already
  blocked behind it.
* **The null SA model** (§5) needs four things: the per-core ID string at `+0xF0`, a W1C factor register at `+0x104`,
  an enable mask at `+0x108`, and, a short time after GO, `factor |= 0x01` ("finished") with the source-A IRQ
  asserted. Every result message is built by firmware, so the model never writes guest RAM.
* ConvZ1C accepts the untouched parameter block ("0 output defect points"). That is a valid, empty result (§4.3).

---------------------------------------------------------------------------------------------------------------------

## 1. Hardware map

### 1.1 Cores (accessors `43923E74…43924660`; core index 0..3 → base)

| core idx | base | ID string at +0xF0..+0xFF (`4391B334`) | clock set reg | reset set reg | IRQ A / B / C | src# A / B / C |
|---|---|---|---|---|---|---|
| 0 | **0xF34A0000** | `"ZMA0V1.0SA5_V1.0"` | 0xF34000B0 | 0xF34002B0 | 51 / 52 / 53 | 0 / 1 / 2 |
| 1 | 0xF34B0000 | `"ZMA1V1.0SA5_V1.0"` | 0xF34000C0 | 0xF34002C0 | 54 / 55 / 56 | 3 / 4 / 5 |
| 2 | 0xF34C0000 | `"SA0_V1.0SA5_V1.0"` | 0xF3400130 | 0xF3400330 | 61 / 62 / 63 | 6 / 7 / 8 |
| 3 | 0xF35C0000 | `"SA1_V1.0SA5_V1.0"` | 0xF34004E0 | 0xF34006E0 | 64 / 65 / 66 | 9 / 10 / 11 |

* IRQ numbers are GIC IDs (board `GIC_ID(n) = n − 32`). Task 44 enables 51–56, 61, 63, 64 and 66 (`ena_int` in
  `439232BC`); 62 and 65 stay masked.
* Clock/reset (`43923880` = clock, `43923910` = reset, `439238B0` / `439238E0` = the same registers at +4): init writes
  reset = 0x11 and clock = 5 for all four cores, reads the IDs, then writes 5 / 0x11 to the +4 twins. Each function
  start writes reset = 0x11 and clock = 5 again. This looks like a set/clear pair [? which one gates the core]. The
  block at 0xF3400000 can stay a regfile.

### 1.2 Per-core register map

| offset | access | meaning | evidence |
|---|---|---|---|
| +0x000 | RMW bit0 | run enable. Written 1 just before GO | `43923E74`; r1 trace |
| +0x004 | RMW bit0 | **GO**. Bit0 is read back by `43923EAC` [? busy] | `43923EE0`; r1 trace |
| +0x008 | W | `0x80000000` when a DRAM program is used (ZIT). Otherwise `PA(0x98000000 + core·0x40000)` [? resident program] | `43923F18`, `439208F8` |
| +0x00C | W / RMW | misc. Bytes 0 and 2 written separately | `43923F48/FA8/4008` |
| +0x01C | RMW bits1:0 | from request byte 15 when its bit7 is set | `43924038` |
| +0x030 | W | program entry PA (ZIT: 0x11E10600, inside the ODL-loaded image) | `43924070`; r1 trace |
| +0x084, +0x088 | R | read on completion and printed with the result [?] | `439240A0/D4` in `4391D3BC` |
| +0x0F0..+0x0FC | R | **16-byte core ID** (ASCII, LE words), compared at task-44 init | `4391B334` |
| +0x100 | R | source-A status byte. Copied into the event (b3) | ISR `4391A4C4` |
| **+0x104** | R / **W1C** | **source-A interrupt factor** (byte). The ISR writes back the value it read. Arming writes 0xFF | `439241A8`, `43925FB4` |
| **+0x108** | RW | **source-A factor enable mask** (byte). Arming writes 0xFF, the ISR writes 0 | `43924214` |
| +0x110..+0x11C | RW | host→SA mailbox, 4 words. 0x41: request words +16/+20/+24 and 0. The ISR state-1 path clears them | `43924284`, `439208F8` |
| +0x120..+0x12C | R | SA→host mailbox, 4 words. Copied into the 0x41 result | `439242C0` via `43AC864E` |
| +0x140 / +0x144 / +0x148 | R / W1C / RW | source B status / factor / mask (ISR B writes +0x144 = 0xFF, +0x148 = 0) | `439242F8…439243CC` |
| +0x150..+0x15C, +0x160..+0x16C | RW / R | source-B mailboxes (cleared on the state-1 path) | `4392443C`, `43924478` |
| +0x200 / +0x204 / +0x208 | R / W1C / RW | source C status / factor / mask (ISR C writes +0x204 = 0xFF, +0x208 = 0) | `439244B0…43924584` |
| +0x210..+0x21C, +0x220..+0x224 | RW / R | source-C mailboxes | `439245F4`, `43924630` |

Every accessor takes a core index and switches on the four bases above. Other offsets were not seen.

### 1.3 Interrupt service (`4391A4C4…4391A880`, one per source)
Source A of core n (the others use the same pattern at +0x140 or +0x200):
```
ev = { 8, 0xC1, src#, b3 = rd8(+0x100), b4 = rd8(+0x104), b5 = rd8(+0x108), 0, 0 }
wr(+0x104, (rd & ~0xFF) | b4)      ; ack: write back the factor that was read (W1C)
wr(+0x108, (rd & ~0xFF) | 0)       ; mask all factors
osal_snd_msg(0x101, ev)            ; msg tag 0x930101, 8 bytes (4391A42C)
```
Sources B and C write `0xFF` to the factor and 0 to the mask. So the line behaves as level
`(factor & mask & 0xFF) != 0`, and the ISR's ack and mask clear drop it.

---------------------------------------------------------------------------------------------------------------------

## 2. The manager protocol (client ↔ task 44)

### 2.1 Transport
* Requests: `43AC7D84(0x101, msg, len)`, OSAL valloc + copy + `osal_snd_msg`. Wrappers: `43AC7CAA/7CB6/7CE6` (20 B),
  `43AC7CDA` (28 B), and others in `43AC7C62…43AC7D2E`.
* Results: task 44 `4391B254(hdr, msg)` sends to the queue in request bytes 2–3, using the same `43AC7D84`.
* The ZIT client creates a temporary queue (`43A4EF6C(q, 2, 188)`, id 0x60EB in r1) and blocks in `4398EA3C`
  `rcv_msg_tmo(q, &m, −1)`. **No timeout.**
* Task 44 dispatch (`439232BC`) uses byte0 & 0x7F = length and byte1 = command. A wrong length goes to `43ACAA7C`
  (error). Tags 0xBD017A (ODL reply) and 0x61003C are handled before the switch, and so is a first halfword of 0xEECA
  (`43926F6C`: program install by name, `"SA_PREINSTALLED_PRG:"`, `"SA_JPEG_ENC"`, `"SA_JPEG_DEC"`).

| cmd | len | handler | meaning |
|---|---|---|---|
| 0x21 | 20 | `43AC873A` | **open** (allocate handle, then load through ODL) |
| 0x23 | 20 | `4391D9F4` | **close** |
| 0x41 | 28 | `43AC879E` → `4391CFC0` | **function start / CalcStart** (mailbox from request words) |
| 0x42 | 20 | `4391D694` | [?] (ZIT cmd 66 path in `4398E93A`) |
| 0x44 | 60 | `43AC87CE` | function start with a long parameter list (`43AC9584` fills the mailbox) |
| 0x11/0x12/0x13/0x14/0x18/0x19/0x28/0x29/0x31/0x32/0x54 | 20–24 | `43ACA658`, `43ACA2CC`, `43922710`, `43ACA518`, `4391FDFC`, `4391FA0C`, `4391FCF8`, `43AC9164`, `43ACB6DC`, `43ACB7BC`, `43922710` | other services (memory, program management) [?] |
| 0x81…0x87 | 20/44 | `43AC9800…43AC9D1E` | [?] |
| **0xC1** | 8 | **`43925888`** | **IRQ event** from the ISRs (§1.3) |
| 0xC2…0xCF | 4–52 | `4391AD00`, `4391F13C`, `4391E9EC`, `43921198`, … | internal events [?] |

### 2.2 Message layouts (observed in r1, LE words)

Request (client → 0x101):
```
open  0x21: 60eb2114 00000000 00000200 00000000 00000000
            b0 len=0x14 | b1 cmd | u16@2 reply queue | b8 handle | b9 program id (2) | …
start 0x41: 60eb411c 00000000 03000201 00000000 11cb0000 10a1b000 00000000
            b8 handle (1) | b9 program id (2) | b11 function type (3) | w@16/20/24 → mailbox +0x110/114/118
```
Result (task 44 → client), 20 bytes for 0x21/0x22/0x23, 28 for 0x41:

| byte | 0x21 open | 0x22 load | 0x23 close | 0x41 start done (`4391D3BC`) |
|---|---|---|---|---|
| 0 | 0x14 | 0x14 | 0x14 | 0x1C |
| 1 | 0x21 | 0x22 | 0x23 | 0x41 |
| 2–3 | 0x0101 | 0x0101 | 0x0101 | 0x0101 |
| 4 | handle | handle | handle | handle (from the core table) |
| 5 | program id | program id | program id | program id |
| 6 | error flag | error flag | error flag | 0 |
| 7 | error status | error status | error status | 0 |
| 8 | 0 | **loaded flag (must be ≠ 0)** | 0 | 0 |
| 9–11 | 0 | core info / u16 | 0 | b10 = **int_fact = ev.b4 (factor at IRQ time)**, b11 = 0 |
| 12–27 | 0 | word @12 | 0 | mailbox +0x120, +0x124, +0x128, +0x12C |

Observed: `01012114 00000201 …` (open OK, handle 1) and `01012214 00000201 042b0001 …` (load OK). Errors from
the start path use `43AC82E6`, which sets b6 = 1 and b7 = code.

### 2.3 ZIT acceptance (`4398EA3C`, disassembly 4398EA60…4398EB3A)
* Every result: b1 == expected id and b6 == 0 and b7 == 0, otherwise "Driver Error" (704 + b7).
* 0x21: stores b4 as the handle. 0x22: b8 ≠ 0, otherwise "OPEN Comp Received But SA Load Error".
* **0x41: b10 must be exactly 1**, otherwise "Start Done Received But SA int_fact Error %d mbox %x %x %x %x"
  (720 + b10).

Other clients test the same field the same way. SDF `WrapSaCalcBase` `436532D0` gives the **int_fact encoding**:
**0 = unfinished, 1 = finished, 2 = cancel, 4 = invalid param, other = reserved**. FRC2 `4395F5E4` likewise:
`b6 != 0` is an error, `b10 == 1` is OK, `2` is cancel, `0` is "NOT_FINISH". Factor value **0x01 = finished**.

---------------------------------------------------------------------------------------------------------------------

## 3. Open / load / close: no hardware involved
* **Open** `43AC873A`: looks up the program (`43AC8EEE(prog id)`). If it is unknown, it answers 0x21 + 0x22 with
  error 3. Otherwise it allocates a handle (`439211E8`, 64-slot bitmap), answers 0x21 OK and starts the load
  `43AC84BA`.
* **Load**: `43AC8D32` decides "already resident" or "load". A load sends an ODL request to userland
  (`mq_send(0xBD017A, …)`, seen in r1: `00000001 00000001 00002710 …`). On the ODL reply (tag 0xBD017A in task 44) it
  marks the program state `dword_43E0D4F0[255·core + prog] = 2` (loaded) and answers 0x22 with b8 = 1
  (`43AC813C`). **This already works in the emulator** (r1: open and load results arrive within the same
  millisecond).
* **Close** `4391D9F4`: frees the core slot (`43924CF4`, `4392035C`) and answers 0x23 (`43AC81F6`). No MMIO.
* The program image is executed in place from DRAM: the core gets the entry PA in `+0x030` (0x11E10600 for ZIT).
  There is **no program DMA into the core** to model.

---------------------------------------------------------------------------------------------------------------------

## 4. Function start and completion (the part that needs hardware)

### 4.1 Start: `4391CFC0` → per-core `439208F8` (SA0), `43920A40` (SA1, also used as SA0's partner for dual-core jobs), `43920BA4` (SA2), `43920CE8` (SA3)
Order (SA0, confirmed by the r1 `-d unimp` trace, which matches RE_LIVEVIEW_REQUEST §3.3):
1. reset reg = 0x11, clock reg = 5;
2. `+0x110..+0x11C` = request words +16, +20, +24 and 0 (cmd 0x41), or the long parameter list (cmd 0x44);
3. `+0x008` = 0x80000000, `+0x030` = entry PA (`439256E4` resolves the program), `43924D50` / `43924A70` (bus
   windows, outside the core blocks [?]);
4. **arm** (`43925FB4`/`FD8`/`FFC`/`6020` for cores 0–3): `state[core] = 3`, `+0x104 = 0xFF` (clear),
   `+0x108 = 0xFF` (enable all);
5. log reads of `+0x110..+0x11C`;
6. `+0x000 |= 1`, then `+0x004 |= 1` (GO).

### 4.2 Completion
1. The core raises source A (`factor & mask`) → ISR (§1.3) → event `{8, 0xC1, src#, st, factor, mask}` → task 44.
2. `43925888` switches on src#. For a source-A event (src 0/3/6/9) it checks that core's state
   (`dword_43E10FF8` for SA0, `…1004` SA1, `…1010` SA2, `…1018` SA3):
   * **state 3** → state := 0 → **`4391D3BC`**, which builds the 0x41 result (§2.2, b10 = factor) and sends it to the
     client queue. Then, if `factor & 0x3F`, it ends the core job (`4391CE7C/CEF4/CF38/CF7C`). Otherwise it continues
     (`43926044`, [?] multi-part job);
   * states 1, 2, 4, 5 are other job types (state 1 clears all mailboxes and runs the install path) [?];
   * any other state: the event is dropped.
3. Source-B/C events go to the per-source state words `…0FFC/1000/…`, used by other job types [?]. ZIT does not use
   them.

### 4.3 What the caller checks in memory (ConvZ1C `43990B2C`)
The parameter block (40 bytes, VA 0x91CB0000 = PA 0x11CB0000, passed in mailbox word 0) is filled by av-cam before
the start:

| off | content |
|---|---|
| +0x00 | PA of the input defect list |
| +0x04..+0x10 | PA of 4 output buffers |
| +0x14 | u16 input point count |
| +0x16 | format byte (bits1:0 = input format 1 or 3, bit6 = flag, `4` preset) |
| +0x18..+0x26 | 8 × u16 output counters, **zeroed by the caller** |

After a 0x41 OK, ConvZ1C requires `sum(out[0..3]) <= in_count` and then `43AE1A46 → 43AE19A6`: for format 1 the
counts 1–3 must be 0, and every count must be ≤ its buffer capacity. **All zeros passes.** That means "no defect points
to correct", a valid result for an emulated sensor. The model does not need to touch the block.

---------------------------------------------------------------------------------------------------------------------

## 5. Spec: `cxd90014-sa` ("null SA") device

### 5.1 Instances and mapping
* 4 instances at 0xF34A0000, 0xF34B0000, 0xF34C0000, 0xF35C0000. Each is an MMIO region of 0x1000 bytes [? real
  size; the highest used offset is +0x22C], mapped over the `io-f0` catch-all.
* IRQ outputs per instance: A, B, C → GIC IDs (51, 52, 53), (54, 55, 56), (61, 62, 63), (64, 65, 66).
* Property `core-id` (16-byte string) per instance, from the table in §1.1. Its source is the compare strings in
  av-cam `4391B334` (firmware expectation; tag S-like, not V until run).
* Leave 0xF3400000 (clock/reset) as it is (regfile). Do not gate the model on it [?].

### 5.2 Registers
| offset | behaviour |
|---|---|
| +0x000 | RW latch |
| +0x004 | RW latch. **Write with bit0 = 1 → start** (§5.3). Reads: bit0 = 1 while "running", 0 after completion [?: `43923EAC` read sites not traced; 0 after completion is the safe choice] |
| +0x008, +0x00C, +0x01C, +0x030 | RW latches (log on start) |
| +0x084, +0x088 | read 0 |
| +0x0F0..+0x0FC | read-only ID string as 4 LE words (`"ZMA0"` → word 0x30414D5A, …) |
| +0x100, +0x140, +0x200 | status bytes: read 0 |
| +0x104, +0x144, +0x204 | factor A/B/C: read = pending factor bits (byte). **Write = W1C** of the written low byte (writing back the read value, or 0xFF, clears) |
| +0x108, +0x148, +0x208 | enable mask A/B/C: RW latch (byte) |
| +0x110..+0x11C, +0x150..+0x15C, +0x210..+0x21C | host→SA mailboxes: RW latches |
| +0x120..+0x12C, +0x160..+0x16C, +0x220..+0x22C | SA→host mailboxes: read 0 (only printed on error) |
| anything else | RW latch + `LOG_UNIMP` |

IRQ line X = `(factor_X & mask_X & 0xFF) != 0`, re-evaluated on every write to factor or mask (level).

**Pitfall:** the current `io-f0` regfile reads back the 0xFF the arm step writes to +0x104. If such a stale 0xFF were
delivered with an IRQ, the result would carry int_fact = 0xFF and ZIT would reject it (720 + 255). The factor register
must clear on write (W1C).

### 5.3 Start behaviour
On a write to +0x004 that sets bit0:
1. log (trace event) `core, +0x030 entry, +0x008, +0x110..+0x11C`. That identifies the client (§6) at run time;
2. start a one-shot timer, **100 µs virtual** [? real run time unknown; any delay > 0 works]. Do not complete inside
   the MMIO write: the ISR posts to task 44, which is the task doing the write;
3. on expiry: `factor_A |= 0x01` ("finished", §2.3), clear +0x004 bit0 (and +0x000 bit0 [?]), update IRQ A.

Never raise sources B or C. Never write guest RAM. There is no reset dependency beyond device reset (all latches 0,
ID constant).

### 5.4 Expected result (acceptance)
* r1 probe set (§8): after `sa_calc` an `irq_evt` (`43925888`) with src 0, then `start_done` (`4391D3BC`), then an
  `mq_send` to queue 0x60EB of `0101411c …` with **byte 10 = 0x01**; `sa_wait(0x41)` returns 0; `sa_cmd(0x23)`;
  `sa_wait(0x23)`.
* ConvZ1C returns 0 and the imager job clears busy `0x43ED6D67` (RE_LIVEVIEW_REQUEST §4.5).
* `liro-kliro_90` no longer parks in `4398EA5B`. `AV_4067cb` runs for every `AV_4067` (RE_SD_MEDIA_SERVICE §7.2).
* No `!!!ZIT ERROR!!!` text from `AV_err 43894848`.

---------------------------------------------------------------------------------------------------------------------

## 6. Other SA clients, and which ones need real data

| client (av-cam) | program (`/usr/sabin` or `/system/sabin`) | when | null-SA effect |
|---|---|---|---|
| **ZIT ConvZ1C** (`43990B2C`, imager start job) | program id 2 [? `sa_defect.bin` / `sa_zphdefdet.bin`; the id → file map is in Backup / ODL] | **boot, imager start (only SA start before the shooting screen, r1)** | OK: 0 defect points (§4.3) |
| FDT face detection (`439A8B3C`, `"[FACE]: FDT Int Error"`) | `sa_dfdet.bin`, `idt_cam.bin` | live view with face detection on [?] | copies a 324-byte result from the SA output buffer. It is untouched (stale or zero) DRAM → zero = "no faces" [?]. Factor bit4 = error path |
| Tracking (`[TRK] [BT]`, `439AF14C`) | `sa_bayes_tracker.bin` | lock-on AF [?] | empty track [?] |
| JPEG enc/dec (`SA_JPEG_ENC/DEC`, `tsk_jpeg` result ids 0x13–0x15) | `sa_jpeg_{enc,dec}_{1core,1unit}.bin` | capture; **playback / thumbnails** | **needs real data**: no hang, but no image (black or garbage); encode writes no valid JPEG |
| NR / still pipeline (`sa_func_{BFNR,KKNR,LFNR,YKNR,SPICA,STAR,TBOX,CONTP,BOKS,SFTFCS,PreDCV,NFLASH,DUALREC_*}`, GOLFSHOT, SDF, FRC2) | `sa_kknr`, `sa_lfnr_*`, `sa_spica`, `sa_star`, `sa_tbox`, `sa_sftfcs`, `sa_dcv`, `sa_frc2`, … | still capture, scene modes, movie | in-place filters on image buffers: the buffer is left as it is (no NR) → acceptable [?] |
| Movie AVC (`ZIMA`) | `sa_avce_*_zma.bin`, `sa_avcd_*_zma.bin`, `sa_zima_startup_*.bin` | movie record / play | **needs real data** (bitstream / decoded frames) |
| Audio codecs, LPS (RE_AUDIO §2.2) | `sa_aaclcenc_nr`, `sa_dd51enc_nr`, `sa_integenc_nr`, `sa_ac3dec`, `sa_aaclcdec`, `sa_bd_lpcmdec` | movie audio, playback | "finished" with no data = silent / empty frames [?]. `LPS GetMailBox 438B48D8` waits on a mailbox callback that this model never produces [?] |
| Panorama (`ddls_srotate/stitch`) | `sa_sweep_panorama.bin` | sweep panorama | **needs real data** |
| kernel `cmpr` (WBI writer) | `ssboot.bin`, `ssboot_any.bin` | only when writing a WBI | polls instead of using the IRQ (RE_USERLAND_SUBCPU §3). **Needs real data** (compressed image). Out of scope |

Minimal data that satisfies a caller: for every client seen, a result with b6 = b7 = 0 and int_fact = 1 passes the
driver layer. Only the consumers of the output buffers (JPEG, AVC, panorama, cmpr) need real computation. Those
would need either an SA ISA emulator or per-program HLE, which is not needed for boot, menus, AUTOREPAIR or Format.

---------------------------------------------------------------------------------------------------------------------

## 7. Open questions
1. Real SA execution time, needed for realistic timing (§5.3). 100 µs is a placeholder.
2. Semantics of `+0x004` read-back (busy?), `+0x000` after completion, `+0x008` (0x80000000 vs resident base), `+0x00C`,
   `+0x01C`, `+0x084/+0x088`, and the status bytes `+0x100/+0x140/+0x200`.
3. Sources B and C: which job types use them (state machines `…0FFC`, `…1000`, …) and whether any client blocks on
   them (LPS mailbox, SFRA "SA Notify"). The null model never raises them.
4. Consequence of an ID mismatch (`byte_43E78F94`, only a data xref at `43EE0588`). The model returns the expected
   strings, so the question only matters for confirming the strings are right.
5. The program id → file mapping (ODL / Backup), to name the ZIT program exactly.
6. Which SA clients run once live view is up (FDT, tracking). After §5 is in, use the start trace (§5.3 step 1) to list
   them.
7. Gate B (`0x3007`, RE_SD_MEDIA_SERVICE §7.3) is independent of this model **[?]**. Re-test it once SA0 completes,
   since the imager job was its probable predecessor.

---------------------------------------------------------------------------------------------------------------------

## 8. Reproduce
* IDA: `C:\a5000\emu\sa\sa.i64` (copy of `sdrepair\gavcam.i64`; pristine copy `pristine.i64.bak` alongside), query runner
  `q.py` (copy of `sensor\cf_q.py`).
* Probes (`a5ktrace fn=`, `fncap=300,fnr1w=10`, run with `-d unimp,plugin` or the plugin prints nothing):
  ```
  43ac7d84 mq_send   4391a42c isr_post   4398ea3c sa_wait   4398e93a sa_cmd   4398e870 sa_calc
  4391d3bc start_done 43925888 irq_evt   43ac873a h_open    4391d9f4 h_close  43ac879e h_start
  43ac84ba load      4391cfc0 start_core
  ```
* `-d unimp`: grep `f34a0|f34b0|f34c0|f35c0` for core access and `f3400` for clock/reset.

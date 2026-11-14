# RE: av-cam ⇄ sub-micro link — transport layer

Scope: the **transport** between `av-cam.bin` (link driver module 0x4369A000–0x4369E000, protocol task 48 module
0x4390FFD0–0x43916100) and the power/front sub-microcontroller. The goal is that the emulator can play the micro's
side of the wire. Method: static only. IDA 9.4 + Hex-Rays on windev (`C:\a5000\emu\lnk_avcam.i64`, a copy of
`smc_avcam.i64`). Hex-Rays loses most globals in this PIC module, so every claim below was checked against the
**disassembly** (`scratchpad/smc/linkdis.txt`, `t48dis.txt`). Board values come from the user's Backup.bin
(`re/backup/whitelist/backup_props_all.tsv`). The real-camera frames come from `re/wbi` (`emulator/tools/wbi.py`).
`[?]` = not proven.

Battery authentication is **out of scope**. `BATT_AUTH_Snd` (log id 0x89, handler `43911814`) is listed once
and not analysed. In the emulator, any bytes related to it are opaque and replayed from the snapshot.

**Two corrections to the brief** (both checked in code and against the snapshot):
* **CRC byte order.** The CRC is CRC-16/CCITT-FALSE over bytes 0..125, stored **little-endian**: the low byte is at
  126 and the high byte at 127. The compare at `4369b8a6` is `crc == b[0x7E] | b[0x7F]<<8`. In the snapshot,
  frame +0 has CRC 0xA05B stored as `5b a0`, and frame +0x80 has CRC 0xA659 stored as `59 a6`.
* **Header buffers in `4369c32c`.** TX = `0x43E3354E` and RX = `0x43E33546`. The request descriptor is
  `{tx, rx, u16, u16, len}`, and the SIO driver (`43918294` → `439181CC`) writes `desc[0]` into the FIFO.
  `0x43E33546` is the buffer that gets parsed (`4369b3e0`, `4369c008`).

---

## 0. TL;DR for the emulator

* There are **two kinds of exchange**, and the level of the request line **p3.17** selects which one runs when
  the micro edges the strobe **p0.12**:
  * **p3.17 low** → **frame exchange**: 128-byte full-duplex TLV status frames. The CPU sends type 0x31 and
    the micro sends type 0x30.
  * **p3.17 high** → **command exchange**: an 8-byte header, then an optional data phase.
* The CPU never starts a header on its own. Every header or data phase starts on a **p0.12 edge** from the
  micro. The exception is one CPU-initiated 128-byte "slot A" exchange per engine tick (event 4, §1.4).
* **Idle levels:** p3.17 = 0, p0.12 = 0 (a high p0.12 makes the CPU skip its own exchange). p10.3 is a CPU
  output (S2REQ) and is normally 0.
* **SIO channel [?].** Header and data phases go out on channel `byte_43E62C0E` = **3** (hard-coded in
  `4369dd3c`), with chip select **p5.14**. Every 128/256-byte frame transfer is built by `4369b374`, which writes
  **channel 0** and CS port/pin 0/0. Statically the frames therefore travel on **SIO ch0**, not ch3. Confirm in
  the emulator.
* Every SIO transfer must complete (done bit + IRQ). The link uses the driver's direct path. It has no timer of
  its own, so a transfer that never completes leaves it stuck in that sub-state (§5).

### Board lines (from Backup, read by `4369dd3c` via `sub_4355DCD8`)

| global pair | Backup ids | user value | role | direction |
|---|---|---|---|---|
| `43d684e4/43d684b4` | F904CA/CB | **p3.17** | micro "request": selects command vs frame (verified) | micro → CPU, level |
| `43d684cc/43d684e8` | F90465/66 | **p0.12** (IRQ 133) | micro strobe: every edge = **event 0** | micro → CPU, edge (level read in `4369bed0`) |
| `43d684b8/43d684bc` | F9045F/60 | **p0.7** (IRQ 128) | **event 16** with arg = level of p0.7. `4369a9cc` sets or clears flag 19 bit 0x64E4 and logs 304 "ReleaseLock" [?] | micro → CPU |
| `43d684d4/43d684f0` | F90200/01 | **p0.16** (IRQ 144) | `4369a978`: only logs 308/309 ("ExpBefore"/"ExpStartComp") | micro → CPU |
| `43d684c0/43d684c8` | F90455/56 | **p10.3** | "S2REQ". The CPU drives it (events 0x11/0x13 = high, 0x12/0x14 = low, via `43ac7734/7740`). While it is high, the link stays in command mode | CPU → micro [?] |
| `43d684d8/43d684ec` | F900B1/6D | **p5.14** | SIO CS for header and data phases (request bytes +2/+3) | CPU → micro |
| `43d684dc/43d684e0`, `43d684d0/43d684c4` | F9045D/5E, F90457/58 | p7.15, p7.13 | read nowhere in the state machine [?] | – |

The emulator patch currently models p3.17 (high), p0.7 and p0.12 pulses. That reaches state 1/0, which is the
*command* path, and so only ever produces 8-byte header transfers.

---

## 1. State machine and transaction sequences

The link task `4369af28` reads 5-word events from queue 0x1502C7. The dispatcher `4369cf88` handles events 0–4
through `table[state] + 0x14·sub + 4·event` at 0x43EA78FC. It handles 0x10–0x14, 0x20/0x21, 0x30/0x31 and 0xF8
inline. A handler returns either 0xFF (wait for the next event) or the number of the event to re-dispatch at once.

Events:

| event | source |
|---|---|
| 0 | p0.12 edge (IRQ 133 → `43a3ddde`) |
| 1 | SIO transfer done (completion callbacks `4369bdfe`/`4369be78`; they post event 1 whatever the status) |
| 2 | upper-layer request to send a command (`4369b07c`/`4369b110`, msg byte0 = 2). Normally `4369c098` only enqueues it in a 10-slot ring at 0x43D68820 |
| 3 | unused (every entry logs "TskJobNG") |
| 4 | engine periodic tick: `4376abfc` → `43a3ddc0`, apparently once per frame/VD when a flag is set [?] |

Handler table (address+1 = Thumb). `dd67` = log TskJobNG, `dd4f` = log TskJobOK, `ddb7` = enqueue command
(`4369c098`), `b2c5` = remember "edge pending" (`byte_43E62C12` = 1).

| state/sub | ev0 (p0.12 edge) | ev1 (SIO done) | ev2 | ev4 (tick) |
|---|---|---|---|---|
| **4/1 idle** | `b4c5`: p3.17 high → 1/0, else → 4/3; returns 0 (re-dispatch ev0) | dd67 | ddb7 | `bed1` CPU slot-A send |
| 4/2 | b2c5 | `b859`: forward RX slot A to task 48 → 4/3 | ddb7 | dd4f |
| 4/3 | `c821`: slot-B frame exchange → 4/4 | dd67 | ddb7 | `bed1` |
| 4/4 | b2c5 | `bd9d`: parse RX TLV frame → 4/1 | ddb7 | dd4f |
| **1/0 cmd idle** | `c32d`: 8-byte header exchange → 3/1 | dd67 | ddb7 | `b501`: p3.17 low **and** p10.3 low → 4/1, returns 4 (→ bed1 at once) |
| 3/1 | `c90d`: start data phase (needs `C18`==3) → 3/2 | `b3d9` (`b3e0`): header received | ddb7 | dd4f |
| 3/2 | b2c5 | `c009`: dispatch header + data → 1/0 | ddb7 | dd4f |
| 2/2 | `c121`: CPU data TX (needs `C18`==3 and a queued command) → 2/3 | `b461` | `c105` | dd4f |
| 2/3 | b2c5 | `b671`: TX-done handler, dequeue → 1/0 | ddb7 | dd4f |
| 2/1 | – | – | `c235`: 4.8 MHz CPU header TX → 2/2 | – |

Nothing writes state 2/1: the only writes to the state bytes are at the sites listed by `grep STRB…A0E/A0F`. So
`4369c234` (4.8 MHz, mode 2/2, TX-only, 8 bytes from the ring head) appears to be **unreachable** in this build
[?]. Event 0 in any "waiting" sub-state only sets `byte_43E62C12`. The completion handlers then return 0 and
replay event 0 (`b671`, `c008`, `bd9c`, `b858`), so an edge that arrives during a transfer is not lost.

`byte_43E62C18` is the data-phase gate. `c32c` clears it to 0. Bit 0 is set by the p0.12 edge handler (`c90c` in
3/1, `c120` in 2/2). Bit 1 is set by `b3e0`/`b468` when the header announces data. The data phase starts only
when both bits are set (== 3).

### 1.1 SIO transfers used (request = 64-byte msg to 0x740017, all with +0x10 = 0, i.e. direct start)

| who | ch | CS | clock | +0x28/+0x2C (+0x2C: 1 = RX only, 2 = TX only, 3 = full duplex) | TX | RX | len |
|---|---|---|---|---|---|---|---|
| `4369c32c` header | 3 | p5.14 | 8.25 MHz | 1 / 3 | 0x43E3354E (always 8× 0x00) | 0x43E33546 | 8 |
| `4369c90c` data, micro→CPU | 3 | p5.14 | 8.25 MHz | 1 / 1, +0x30 = 1 | – | buffer from row fn1 (`4369b178`) | hdr.len in ≤128 B segments |
| `4369c120` data, CPU→micro | 3 | p5.14 | 8.25 MHz | 1 / 2, +0x30 = 1 | queued command payload | – | queued len in ≤128 B segments |
| `4369c90c` cat5/cmd4 (0x54) | **0 [?]** | 0/0 | 8.25 MHz | 1 / 3 (`4369b374`) | TXbuf+0x00, +0x80 | RXbuf+0x00, +0x80 | 2×128 |
| `4369bed0` CPU slot A | **0 [?]** | 0/0 | 8.25 MHz | 1 / 3 | TXbuf+0x00 | RXbuf+0x00 | 128 |
| `4369c820` slot B | **0 [?]** | 0/0 | 8.25 MHz | 1 / 3 | TXbuf+0x80 | RXbuf+0x80 | 128 |
| `4369c234` (dead) | 3 | p5.14 | 4.8 MHz | 2 / 2 | 0x43E3354E | – | 8 |

`TXbuf` = `off_43D68918` and `RXbuf` = `dword_43D689E8`, both 256 bytes. The snapshot at PA 0x8334417C is
**RXbuf** (§4).

### 1.2 Micro → CPU frame (frame exchange, "slot B")
1. Idle 4/1, p3.17 = 0. The micro puts its 128-byte **type 0x30** frame in its shift register.
2. The micro edges **p0.12**. `b4c5` sees p3.17 = 0 → 4/3 and re-dispatches ev0. `c820` checks p3.17 again (if it
   is high: log 0x15B "CamSioSnd InRel H" and abort). Then `c580` builds the CPU's **type 0x31** TLV frame, and
   one **128-byte full-duplex** transfer runs: TXbuf+0x80 ⇄ RXbuf+0x80 → 4/4.
3. On done (ev1), `bd9c` → `b88c` checks the CRC (error → log 0x14F "CRC Err") and byte0 == 0x30 (else 0x150
   "Category Err"). If both pass it walks the TLVs, then hands the TLV pointer table to the consumers (§3.3),
   replays a deferred S2REQ, and goes → **4/1**.

The same transfer carries the **CPU → micro** TLV frame (type 0x31), so one exchange moves one frame each way.

### 1.3 Command exchange (p3.17 high)
1. Idle 4/1. The micro raises **p3.17** and edges **p0.12** → `b4c5` → 1/0 → ev0 → `c32c`. The CPU clocks 8 bytes
   (it sends zeros) and receives the micro's header (§2) → 3/1.
2. On done, `b3e0` reads `len` = hdr[4..5]:
   * `len == 0`: → 3/2 and re-dispatch ev1 → `c008` calls `4369b1c4(hdr)`, which runs the row's **fn2**
     (receive handler) → **1/0**.
   * `len != 0` and `hdr[3] != 2` (**micro → CPU data**): the CPU waits for the next **p0.12 edge**. `c90c`
     then runs fn1 of the row (`4369b178`, allocate) and does an RX-only transfer of `len` bytes (maximum 0x17A0,
     else log 0x203) → 3/2 → done → `c008` → fn2(hdr, data) → 1/0. Special case cat 5 / cmd 4: a 256-byte
     slot A+B exchange (§1.1), after which fn2 of row 0x2A (`43a3de38`) forwards slot A to task 48 and parses
     slot B.
   * `len != 0` and `hdr[3] == 2` (**the micro polls for CPU → micro data**): → 2/2. On the next **p0.12 edge**,
     `c121` (only if the ring holds a queued command) sends that command's payload TX-only, 8.25 MHz, ≤128 B
     segments → 2/3 → done → `b671` runs **fn3** (TX-done) of the *queued* header's row, dequeues → 1/0.
     The CPU's own header bytes are never sent (2/1 unreachable). How the micro learns that the CPU has queued a
     command is not visible in the link [?]; it is probably a flag in the 0x31 TLV frame.
3. **Back to idle.** The micro drops p3.17. On the next event 4 (tick), `b501` sees p3.17 = 0 and p10.3 = 0 →
   4/1 and re-dispatches ev4 → `bed1`. While in 1/0, **every p0.12 edge starts another header exchange, even
   with p3.17 low**. So after a command the micro must not strobe until the CPU is back in 4/1.

### 1.4 CPU-initiated exchange ("slot A", per tick)
`bed1` (ev4 in 4/1 or 4/3) does the following:
* if p10.3 is high → 1/0;
* else if p0.12 is high → log 0x157 "FrtComSkip";
* else it polls queue 0x2804B7 for a 128-byte packet from **task 48** (`b620`). If there is none, it logs
  0x144 "FirstRcvErr" / 0x148 "FrtDelay" and does nothing;
* otherwise it runs a **128-byte full-duplex** exchange TXbuf+0 ⇄ RXbuf+0 → 4/2 → done → `b858`/`b7f0` sends
  RXbuf+0 to task 48 (queue 0x2804B6) → **4/3**. The micro is then expected to strobe p0.12 for slot B (§1.2).
  In 4/3 a further tick repeats slot A (log 0x15C "FrtComContinue").

Task 48 (`439106c4`) runs in lock-step: for every RX slot-A packet it receives, it posts one TX packet to 0x2804B7.
Link init (`4369cb84`) seeds it once: it sets `C15` = 1, calls `b7f0` (sending a zero RX packet), then clears
`C15`. So one TX packet is queued at boot.

**Full periodic cycle (best reading):** tick → CPU clocks slot A (task-48 packets both ways) → micro strobes →
CPU clocks slot B (TLV frames both ways) → idle.

---

## 2. 8-byte header (command exchange)

**CPU → micro:** always 8 × 0x00. `0x43E3354E` is written only by the unreachable `c234`, which would copy
ring-entry bytes 4..11 = {u32 hdr0..3, u16 len, u16} [?].

**Micro → CPU (RX, `0x43E33546`).** The code reads it as two little-endian words `a1 = b0..3` and `a2 = b4..7`.

| byte | meaning |
|---|---|
| 0 | not checked by the link (logged in 0x116 "SIO RCV") [?] |
| 1 | **category** 0..9 |
| 2 | **command** 0..12 |
| 3 | direction: **2** = "send me your queued data" (CPU → micro); anything else = micro → CPU data follows (when len ≠ 0) |
| 4..5 | **u16 LE data length**. 0 = header only. The maximum is 0x17A0 |
| 6..7 | carried in `a2` high half, not interpreted by the link [?] |

There is no checksum or sequence number in the header.

**What to send:**
* To make the CPU **receive** `n` bytes: `{x, cat, cmd, 0, n_lo, n_hi, 0, 0}` with a type-2 row.
* To make it **send**: `{x, cat, cmd, 2, n_lo, n_hi, 0, 0}`.
* To make it do the **256-byte slot A+B** exchange: cat 5, cmd 4, byte3 ≠ 2, len ≠ 0. Then strobe p0.12 again
  to start the data phase.

### 2.1 (category, command) → row → handlers
Dispatch is `row = byte_43BDB4D1[13·cat + cmd]` (130 bytes). `dword_43EA7534[row]` = `{type, fn1 alloc/rx-setup,
fn2 rx-handler, fn3 tx-done}`. Type 1 = CPU may queue this command (`4369b07c` refuses anything else). Type 2 =
micro → CPU with data (`c90c` logs 0x202 otherwise). Cat 5 cmd 0/2 are excluded from the RX log.

| cat | cmd → row (type) |
|---|---|
| 1 | 2→1B(1) |
| 2 | 0→01(1) |
| 3 | 0→03(2) 1→04(1) 2→05(2) 3→08(2) 4→09(1) 5→0A(2) 6→0C(2) 7→0D(1) 8→0F(1) 9→0E(2) A→06(2) B→07(1) C→0B(2) |
| 4 | 0→19(2) 1→1A(1) 4→1C(2) 5→1F(1) 6→20(2) 7→1D(2) 8→1E(1) |
| 5 | 0→21(2) 1→22(1) 2→23(2) 3→24(1) **4→2A (slot A+B frame)** |
| 6 | 0→10(1) 1→11(2) 2→25(2) |
| 7 | 1→12 2→13(1) 3→14 4→15 5→16 6→17 7→18 (2), 8→28 9→29 (row empty) |
| 8 | 0→02(2) |
| 9 | 0/2/4/6→26(2), 1/3/5/7→27(1) |

The row-level handlers for categories 3 and 8 live in the link module (`4369a1e0`…`4369a8cc`, each a pair of
allocate/receive functions). All other rows go to small stubs at 0x43A3DCA3–0x43A3E09D. Their meaning (release,
flash and lens: log names 0x120–0x13E "CaptureRequest", "PrefireStartNotify", …) is application layer and is not
decoded here.

---

## 3. 128-byte frames

Both frame kinds are 128 bytes with **CRC-16/CCITT-FALSE** (poly 0x1021, init 0xFFFF, MSB-first, no final XOR)
over bytes 0..125, stored **LE** at 126/127. The link uses `43a3dd7e` and task 48 uses `43ac5e04`.
`4369d83c` can poke up to 5 debug bytes into a frame from the config block `byte_43D68AB0+0x20`. Its default is
zero, so it normally does nothing [?].

### 3.1 Slot B — TLV status frame (link module)

| off | field |
|---|---|
| 0 | type: **0x30** micro → CPU (checked); **0x31** CPU → micro (set by init `4369cd56`) |
| 1 | 0 |
| 2 | u8: number of TLV bytes that follow |
| 3 | 0 |
| 4.. | TLVs `{u8 id, u8 len, u8 0, u8 0, data[len]}`. The next TLV starts at +len+4 |
| 0x7E–0x7F | CRC, LE |

* **RX:** ids ≤ 0x13 are kept. `off_43D68994[id]` points into the frame. If `byte_43D68AC0 & 4` is set, the data
  is instead copied into defaults taken from `byte_43ED9E1D`. The defaults are ids
  0,1,2,3,4,5,6,7,8,A,B,C,D,F,10,11,12,13 with lengths 4,4,4,4,4,8,16,4,16,32,16,4,60,252,4,8,76,12.
  * TLV 0 data[0..2] = micro protocol/firmware version, computed as 100·a + 10·b + c (log 0x110 "Last Vcom Ver").
  * TLV 1 data[0] goes to `4369d768`.
  * TLV 3 data[1] bits 0 and 6 are checked in power-off mode 0xD09 or 0xD20 (`4369bbb0`). Bit 6 starts the
    "SaveUsr" read POW/CAM handshake through 0x2804C1, using ids 0x20000500 and 0x30100. The answers come back
    through `4369b6e0` as events 0x30/0x31.
* **TX:** a TLV comes from `byte_43EDA0DD` (ids 12,0,1,3,5,6,7,8,A,C,E,11,13,14) only if its bit is set in
  config mask `dword_43D68AE0` (= `byte_43D68AB0+0x30`, runtime) [?]. The ids are laid out in the order of list
  `0x43E62C5D` by `4369c46c`, which logs an error past 122 bytes. `4369c580` refreshes the TX TLVs before every
  exchange.
* **Consumers.** They are synchronous queues that receive `&off_43D68994` or `&dword_43D68920`. Which TLV each
  one reads is up to the consumer [?].
  * RX (registered by): 0x150300 (`43703034`), 0x150301 (`4380fbc8`), 0x150302 and 0x15030A
    (`436e58f8` Camera::Lens), 0x150308 (`4388db40`), 0x150309 (`436b83dc`, `436cca78`), 0x15031F (`436dd634`),
    0x150311 (no registrant found), 0x1502FF (`43753194`).
  * TX providers: 0x1502FC, 0x1502FD, 0x150305, 0x150306, 0x150307, 0x15031D, 0x150336.

### 3.2 Slot A — task-48 packet (fixed-layout register map, not TLV)
* RX is decoded by `439149f8`: CRC check (error → log 7). Byte 0 == **0x40** takes a special path [?]; any other
  value is the normal path, which runs ~20 field parsers in a fixed order (`43ac5dc0`, `43ac73ae`, `43912258`,
  `439161d4`, …, `4391497c`).
* TX is built in the buffer `*(off_43E0C9A4)` (`43915638`) by field writers at fixed offsets (for example
  `439119f2` writes +0x3D..+0x45).
* **The named messages are task-48 log ids, not transport ids.** They are field groups inside slot A that task 48
  turns into OSAL notifications or requests. "_Snd" = micro → camera clients. "_Rcv" = camera request →
  micro. Byte offsets are not decoded [?].

| log id | name | handler (log site) |
|---|---|---|
| 0x82 | BootClock_Snd | `4391203c` (from `439120dc`; 10-byte clock → udate 0x90013D) |
| 0x83 | ClockReq_Rcv | `43ac5eb0` |
| 0x84 | MediaState_Snd | `43916150` (from `43916660`) |
| 0x85 | HDMIState_Snd | `43915c20` (from `43915dc4`) |
| 0x86 / 0x87 / 0x91 | TCUBState_Snd1/2/3 | `4391408c` (from `43914818`) |
| 0x88 | ACV_BATT_Snd | `43915b9c` (from `43915ecc`) |
| 0x8A–0x8C, 0x8D/0x8E | BATT_STATE1..3, BATT_TYPE1/2 | log site not found by immediate scan [?] |
| 0x89 | BATT_AUTH_Snd | `43911814` — **opaque, not analysed** |
| 0x8F / 0x90 | VID_CH1_SETTING / CH6_SETTING | `439134c4` / `43913594` |
| 0x92 | Unreg_Snd | [?] |
| 0x93 | Box_Temperature_Snd | `43911114` (from `4391155c`) |
| 0x100 / 0x101 / 0x104 / 0x108 | ExtEvfReq / IndicatorReq / SubLcdReq / WifiReq _Rcv | [?] (not found as MOV immediates) |
| 0x105 / 0x106 / 0x109 | Vgrip_Snd / ExtEvf_Snd / Wifi_Snd | [?] |

The log table lives at 0x43EA6274, with 400 entries of 12 bytes: `{u8 id, u8 hi, u8 cls, u8 argsz, …, char *name}`.

---

## 4. The two snapshot frames (RXbuf, PA 0x8334417C)

**+0x00 = slot A, micro → CPU, task-48 packet.** Byte 0 = **0x20** (normal path, not 0x40). CRC 0xA05B is OK.
The body is fixed-layout fields (`d3 00 02 0d 0c 1f 0f 22 01 14 11 11 1f 14 1f 00 07 27 20 …`, plus a block at
+0x38..+0x5F). It is not decoded at the transport level. Replay it as is.

**+0x80 = slot B, micro → CPU, TLV frame.** Type **0x30**, TLV bytes 0x68 (104), CRC 0xA659 is OK.

| TLV | len | data |
|---|---|---|
| 00 | 4 | `01 00 00 30` → version 100 (same as the static default) |
| 01 | 4 | `00 01 00 00` |
| 02 | 4 | `00 00 00 00` |
| 03 | 4 | `00 40 00 02` → data[1] bit 6 set (power-off/SaveUsr flag; the snapshot was taken at a WBI power-off [?]) |
| 0A | 32 | zeros, except data[8..15] = `ff ×8` |
| 0B | 16 | `02 40 02 00 c0 00 00 00 31 15 00 00 00 00 00 00` |
| 0C | 4 | zeros |
| 10 | 4 | zeros |

These frames were received through the **256-byte cat 5 / cmd 4 exchange** or as slot A followed by slot B. Both
end with slot A at +0 and slot B at +0x80.

---

## 5. If the micro never answers

* **No edges at all.** The link sits in 4/1 forever. Nothing in the link retries on a timer. Every tick (event 4)
  runs `bed1`. Because of the boot seed it clocks one 128-byte slot-A exchange, whatever MISO returns. Task 48 then
  gets a bad CRC (log 7) and answers with another TX packet, so slot A repeats each tick with log 0x15C. Slot B
  never runs. RX TLVs stay at their static defaults. The 0x1503xx consumers see defaults, or are never called
  because `b88c` only runs after slot B [?]. In the emulator IRQ 201+ch was never enabled, which suggests event 4
  was not firing yet [?].
* **SIO completion missing.** The link requests set +0x10 = 0, so they are started directly from the 0x740017
  handler (`439186c0`, which takes the channel mutex with a 20 ms try). Completion comes only from the SIO IRQ
  callback. The 5 s `osal_wai_sem_tmo` is used only on the channel-task path (+0x10 = 1, `439187ec`, which calls
  `cb(2)` on timeout). The link therefore waits in 3/1, 3/2, 2/3, 4/2 or 4/4 indefinitely. The next requests fail
  with "busy" (`req[1]` ≠ 0 → logs 0x20F/0x212/0x228) [?].
* **Error completions** (status ≠ 0: logs 0x216/0x21B) still post event 1, so the state machine proceeds with
  whatever is in the RX buffer.
* **A zero header** is cat 0 / cmd 0 → row 0. fn2 `43a3dcff` logs error 11 and the link returns to 1/0. It stays
  there until a tick finds p3.17 = 0.
* **A bad slot-B frame** (CRC or type) is logged (0x14F/0x150). The TLV pointer table is not updated, but the
  consumers are still notified.

---

## 6. Emulator recipe (micro side) [?]
1. **Reset:** p3.17 = 0, p0.12 = 0, p0.7 / p0.16 at a fixed level. Model SIO ch0 *and* ch3, and complete every
   transfer.
2. **Per tick** the CPU may clock slot A on its own. Answer the 128 bytes with the snapshot slot A (`+0x00`).
3. **Status frame:** pulse p0.12 with p3.17 = 0 (in 4/1, or right after slot A in 4/3). Answer the 128-byte
   transfer with the snapshot slot B (`+0x80`).
4. **Commands:** raise p3.17, pulse p0.12, answer the 8-byte transfer with a header (§2). If len ≠ 0, pulse
   p0.12 again for the data phase. Drop p3.17 when done, and do not pulse again until the next tick has returned
   the CPU to 4/1.

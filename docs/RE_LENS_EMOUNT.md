# RE: E-mount lens communication (fw 1.10)

Scope: how the a5000 body finds, powers and talks to an E-mount lens, and what the emulator must model so that a
virtual lens (for example the E PZ 16-50 kit lens) is accepted. Method: static RE of `av-cam.bin` with IDA 9.4 +
Hex-Rays (`C:\a5000\emu\lens\lens_avcam.i64`, a copy of `smc_avcam.i64`) plus runs of the unmodified firmware in the
emulator (TCG trace plugin, HMP `xp`). Addresses are av-cam VAs (PA = VA + 0x40000000). `[?]` = not proven.

Hex-Rays loses most globals in this PIC image (`off_X + 113xxxxxxx` noise). Every global named below was checked in
the disassembly or read at run time.

---

## 0. TL;DR

* **The lens is driven by av-cam itself**, not by a kernel module and not by the sub-micro. The "Mount" module
  (`Mount_Controller.cpp` / `Mount_DividedInfo.cpp`, log prefix `[BL]` = Body↔Lens) sits at 0x436B4000–0x436BC000.
  Its low-level driver sits at 0x43993700–0x43994000, behind the thunks 0x43A4A7A8–0x43A4A85C.
* **Physical path:** **UART1** (PL011, 0xF2001000, 750 kbaud, or 1.5 Mbaud for capable lenses). It uses **DMA in both
  directions** on av-cam's 16-channel memory DMA (ch0 = TX, ch1 = RX). The PL011 IRQ 151 callbacks are null. The
  path also uses six GPIOs (lens detect, two handshake pairs, power-class select). All pin numbers come from Backup
  ids 0xF905xx and were read at run time (§1).
* **Lens power is switched by the sub-micro.** Backup 0x01250007 = 1 on this body selects that mode. The body asks
  for power in the slot-B TLV 0x0A that it sends to the micro. The micro acknowledges in its TLV 0x03 data[3] bit0
  (§2.3). The current sub-micro HLE replays a recorded frame and never sets that bit.
* **Today's emulator state is "lens mounted but dead", not "no lens".** The lens-detect pin **P0.9** is active-low,
  and the GPIO model leaves it at 0, so the firmware believes a lens is attached. It asks the micro for lens power
  and waits 500 polls. Then it gives up with state 0xFF. When P0.9 is forced high (empty mount), the state machine
  idles cleanly in state 0 (§4).
* **Protocol:** packets `F0 | len16 LE | type | x | blocks… | sum16 LE | 55`. The checksum is a 16-bit byte sum
  over bytes 1..len-4. Blocks are keyed by a one-byte id with fixed lengths (table at 0x43ED86E0, §3).
* **`lensbin_lensproperty/LensAberrationData.bin`** is not protocol data. It holds per-lens aberration/shading
  correction tables for 32 lens ids. av-cam loads it through userland on demand (§3.5).
* **Minimal plan:** (1) a machine option for the P0.9 level, defaulting to an empty mount; (2) a lens-power
  ack in the sub-micro HLE; (3) GPIO echo for the handshake; (4) PL011 DMA plus a lens HLE that answers frames.
  Steps 1–3 are cheap. Step 4 needs reply contents that this static pass has not decoded (§5, §7).

---

## 1. Physical path

### 1.1 GPIO lines (driver `43993d48` reads the pins from Backup with `sub_4355DCD8(id)`)

Run-time values were read from the globals at PA 0x83E2E814–0x83E2E85C after boot, using the user's NAND/Backup.
GPIO accessors: `43ac7734`/`43ac7740` = set/clear the output latch (port base 0xF2910000 + 0x100·port, +0x44 set /
+0x48 clear); `43ac774c` = read the input (+0x10); `43ac76be` = configure the pin.

| Backup ids | global (port/pin) | value | function (accessor) | dir |
|---|---|---|---|---|
| F9050C/0D | 43E2E824/43E2E848 | **P0.9** | **lens detect**, active low: `439939c0` returns `!level` (thunk `43a4a82c`; `43a4a818` = detect && state∉{241,242}) | lens → body |
| F90510/11 | 43E2E844/43E2E834 | **P5.4** | handshake out "H1" (`43993ad4(on)`, thunk `43a4a838`). With on=1 and cfg byte `43CC5B30[10]` == 1, P0.5 is also set up as an IRQ input | body → lens |
| F9050E/0F | 43E2E82C/43E2E858 | **P0.5** | handshake in "H1 echo" (`43993d18`, thunk `43a4a844`); GIC 126 is possible (static handler `43a590c3` is null) | lens → body |
| F90508/09 | 43E2E820/43E2E818 | **P2.11** | handshake out "H2" (`43993cb0`, thunk `43a4a83c`). Later muxed to **UART1 TXD** (pin table 0x43ED70E4, `43993910`) [?] | body → lens |
| F9050A/0B | 43E2E828/43E2E85C | **P2.10** | handshake in "H2 echo" (`43993d30`, thunk `43a4a848`). Later muxed to **UART1 RXD** [?] | lens → body |
| F90512/13 | 43E2E850/43E2E814 | **P3.24** | power-class select (`439939e4(mode)`): mode 1 → P3.24=0, P3.26=1; mode 2 → P3.26=0, P3.24=1; else both 0 | body → board |
| F9046F/70 | 43E2E81C/43E2E838 | **P3.26** | see P3.24 | body → board |
| F9052A/2B | 43E2E854/43E2E840 | **P7.11** | `43993ce4` (thunk `43a4a840`); set to 1 after `436b7554(0)` in `43a4a84e` [?] | body → board |
| F9021E/1F | 43E2E84C/43E2E83C | **none** (0xFFFFFFFF) | direct lens-power GPIO (`43993a88`) and its status input (`43993abc`). Unused on a5000 because power goes through the sub-micro | – |

### 1.2 UART1 = lens serial
* Open: `43993910` (thunk `43a4a7e4`, called from `436b7a44`). It applies the 2-entry pin table at 0x43ED70E4 (P2.11,
  P2.10), runs `43919d70(1)` (UART1 enable + clock), and then calls `43993798(mode)`.
* `43993798`: PL011 channel 1, **750 000 baud** (mode 0) or **1 500 000 baud** (mode 1). The other config words are
  {+8 0, +12 0, +16 2, +20 2, +24 3} → 8N1 [?]. The PL011 clock is 12 MHz or 48 MHz (`43919e24` checks a clkgen byte
  == 63). TX/RX/error callbacks are `nullsub`s, so all data moves by DMA.
* Mode 1 is selected when the lens is power class 2 and lens-info byte +35 bit1 is set (`436b7a44` → `+0x3C`).
  "[BL][L->B]UART_HZ NG" / "FREQ Now:%d, Send:%d" (`436b8b10`) cover the speed change [?].
* Close: `43993998` / `43993b44` (pins back to GPIO, `43919dd0(1)`).
* PL011 registers used (`4391a170…4391a408`): CR +0x30 (TXE bit8 / RXE bit9 toggled separately), IMSC +0x38,
  ICR +0x44 (=0xFF2), **DMACR +0x48** (bit0 RXDMAE, bit1 TXDMAE, bit2 DMAONERR), and FR +0x18 (bit4 RXFE is polled to
  drain the FIFO). RX FIFO PIO read `4391a006` (≤16 bytes, `43993840`).

### 1.3 DMA (av-cam 16-channel memory DMA, 0xF0102000, GIC 173+ch)
* **TX** `4399387c` (thunk `43a4a7c0`): channel 0, src = packet buffer, **dst = 0xF2001000 (UART1 DR)**, byte count
  in the descriptor's 13-bit field `<<8`, channel config "type 2, request **12**" [? request-line meaning], cfg byte
  0x65. It then sets DMACR.TXDMAE (`4391a08c(1,1,0)`).
* **RX** `43993bd4` (thunk `43a4a7e0`, armed from `436b5618`): channel 1, src = UART1 DR, dst = RX buffer, count =
  len/4, config "request **13**", cfg byte 0x2E. It also calls `43993ad4(1)`, which raises P5.4 during reception
  [?: acts as RTS].
* The emulator's `cxd90014-mdma` only models memory→memory copies (word count in cfg [11:0]). The peripheral-paced
  mode used here (count in bits [20:8] of the descriptor word) is not modelled.

### 1.4 Things that are *not* on the lens path
* No kernel module touches the lens (the `.ko`/kernel symbol lists have no lens driver; `upm.ko` only has the
  `BF_KEY_LENS_APO/_DISCONNECT_APO` auto-power-off names).
* The SIO channels (0xF2405000) carry the sub-micro, NFC and panel, not the lens.
* `BonoboDSP_Send/Rcv` (`436d6d96`/`436d6e30`) and the `Iris`/`ND`/`MK` motor tables belong to camcorder-type
  internal iris/ND drivers. Their RE was not continued [?].

---

## 2. Software owner and boot → "lens attached?" path

### 2.1 Modules
| range | role |
|---|---|
| 0x436B4000–0x436BC000 | Mount state machine, packet make/parse, VD-synchronous scheduler (`[BL]` logs) |
| 0x43993700–0x43994000 | lens HW driver (GPIO, UART1, DMA) |
| 0x43A4A7A8–0x43A4A85C | thunk table between the two (listed in §1) |
| 0x436E4000–0x436E6000 | `Camera::Lens` — ODL aberration-data loading (`436e58f8 Initialize`, `job_Mount_*AberrationDataHndl`) |
| 0x43992600–0x43992D00 | received-block store (`43992998(&out, ch, blockId)` finds a block in the last RX frame) |

### 2.2 State variable (BL Info, `dword_43D707C8`, PA 0x83D707C8)
Init `436b817c` (from `43a4a6b4`): state = 0xF0, `+0x04` = 0xFF, and `+0x39` = **Backup 0x01250007** (user value
01). `+0x45` = Backup 0x01250008 (00); `+0x42` = (UPM value 0x92001B & 0xFF00) == 0x100. The main handler is
`436b8b10(event, arg)` (event 1 = periodic, 2/3 = others). Observed states:

| state | meaning (from code) |
|---|---|
| 0xF0 | initial |
| 0 | idle, no lens (detect high). Detect low → `436b77d0` (camera power state 0xC00–0xC03 or 0xC06 OK) → `436b79ec(1)` → state 3 or 4 |
| 4 | waiting for lens power: needs `436b7fe4()` == 1 and `+0x3A` == 1 → state 1. After 500 polls → log, state 0xFF |
| 1 | power/handshake sequence `436b7a98` (§2.4), then lens type `436b7e5c` and the UART protocol |
| 15 / 17 / 18 / 22 / 240 / 241 / 242 / 253 / 254 / 255 | detach, error and recovery states; 241 = "LENS FIRMERR" (`436b7cd8`), 253 = timeout (`436b8220`), 255 = gave up |

`+0x39`: 0 = lens power through the direct GPIO (absent here); 1 = through the sub-micro.
`+0x3A` = power requested. `+0x3B` = power acknowledged.

### 2.3 Lens power through the sub-micro (Backup 0x01250007 = 1)
`436b83dc` (from `436ba5dc`) runs only when `+0x39` == 1. It registers two sub-micro slot-B TLV hooks
(RE_SUBMICRO_LINK.md §3.1):
* **TX provider 0x150306 → `436b760c`**: writes into TLV **0x0A** (`*(tbl)+0x28`) data[1]. **bit0 = `+0x3A` (lens
  power request)**; bit5 = `byte_43EA8491` (set when detect state 2 has been processed) [?].
* **RX consumer 0x150309 → `436b75cc`**: `+0x3B = TLV 0x03 data[3] bit0` (`*(tbl)+0x0C`, byte 7 of the TLV).
  This is the lens power acknowledgement from the micro.

`436b79ec(1)` sets `+0x3A` = 1 only when `+0x3A` == `+0x3B`. At the same time it posts a ReleaseLock
(`436fad28(0x17)` → liro command 0x4049, mask 0x17, on) to av-cam's own command queue 0x40193. That command is the
same 0x4049 "SetReleaseLock" that libObj builds in `sub_87AC84`.

**Run-time evidence** (trace plugin on `436b79ec`/`436b77d0`, unmodified firmware): `436b79ec(1)` from
`436b8c2d`, `436b77d0(0)`, `436b79ec(1)`, and then ~500 × `436b79ec(1)` from `436ba21f`. The final state was 0xFF and
`+0x38..+0x3B` = `00 01 01 00`. The snapshot slot-B frame has TLV 03 = `00 40 00 02`, so data[3] bit0 = 0.

### 2.4 Power/handshake sequence `436b7a98` (sub-state in `+0x10`, 5 ms per step, 400 steps → "[BL][HS]POWER SEQ TIME OUT")
| step | body action | waits for |
|---|---|---|
| 0 | P5.4 := 1 | P0.5 == 1 |
| 1 | P2.11 := 1 | P2.10 == 1 |
| 2 | `436b7a44`: open UART1, start the first TX DMA; P5.4 := 0 | P0.5 == 0 |
| 3 | `43a4a800` (→ `43ae2158` if `436ba680()`) | – → step 4 = done, `436b4908(1)` |

After this, `436b7e5c` reads the lens power class `lensInfo[+34] & 3`: class 1 → `436b4864(4)`, `439939e4` mode 1;
class 2 → `436b4864(1)`, mode 2; anything else → "[BL]LENS_TYPE_UNKNOWN", state 0xFF. It waits
`100 × table[64|65]` µs [?].
`436b7cd8` (block 7, "lens info") copies the block into `*lensInfo + 33` / `+278`. Byte +38 ≠ 0 → state 241 "LENS
FIRMERR". `436b7ef8` (block 9, "startup info") accepts the block when camera power is 0xC06 or block[8..9] == 0.
Otherwise it logs "[BL]SYNC START UP INFO ERR".

### 2.5 Up to the UI
The userland caution "Check that the lens is attached … set 'Release w/o Lens' to 'Enable'" is chosen by
libObj/CautionConfig from the camera state that av-cam reports. The av-cam → libObj notification for the mount state
was not traced [?]. `436b8b10` posts it through `43893f98(5, code, …)` (codes 0x1008, 0x2001, 0x2005, 0x3000, …
[?]).

---

## 3. Protocol

### 3.1 Frame (`436b61d8` = RX parser, errors "[BL]HEADER_NG/FOOTER_NG/CHECKSUM_NG/DATASIZE_NG/COMMTYPE_NG_0")
```
off 0      0xF0                         header
    1..2   u16 LE total length N (whole packet, header..footer)
    3      type: (b3 & 7) ∈ {1,2,3}, else COMMTYPE_NG
    4      sequence/sub-id [?]
    5..N-4 blocks: {u8 id, data…}, length per id from the block table (§3.2); ids 5 and 6 have variable length (`436b7110`/`436b7128`)
    N-3..N-2  u16 LE checksum = Σ bytes[1 .. N-4] mod 0x10000   (`43ae20da`, called as sum(b+1, N-4))
    N-1    0x55                         footer
```
* Type 2 = reply to a request: b[5] must equal the pending command id (`+0x28`), then the callback at `+0x2C` runs.
* Type 3 / special: b[5] == 23 (0x17) → `436b5020(b+6)` (detach/err path); b[5] == 2 → 8 bytes copied to `+0x60` or
  `+0x68`; ids 58/59 → `436b714c` [?].
* Type 1 = periodic VD-synchronous info [?]. It carries blocks 5 and 6; block 6 data[1] & 3 selects the drive mode
  (1/5/2) [?].
* After parsing, blocks 5, 6, 67 and three ids from a table are copied out (`43a4bcdc`, `43a4bd8c`).

### 3.2 Block table (`*(0x43EAA518)` → 0x43ED86E0, 6-byte entries `{u16 id, u16 A, u16 B}`, read at run time)
`436cc708(id)` returns the entry, but only if the id's bit is set in the lens's supported-command bitmap
(`436cc6b0`, bitmap byte `(id-1)>>3`). **The lens declares which blocks it supports.** The RX parser uses field A as
the block length.
```
01:21,21 02:0,8 03:15,0 04:e,0 05:0,ff 06:0,ff 07:2,23 08:9,ca 09:5,c 0a:11,11 0b:3,3 0c..10:2,2
11:5,5 12:103,103 13:2,21 14:10,2 15:408,2 16:2,2 17:0,3 18:b,9 19:2,2 1a:3,0 1b:6,b 1c:1,2 1d:5,2
1e:5,2 1f:e,2 20:2,0 21:3,2 22:3,3 23:f,2 24:11,8 25:2,11 26:2,5 27:6,6 28:2,24 29:1,0 2a:1,0 2b:2,2
2c:1,41 2d:1,2 2e:3,3 2f:3,0 30:f,7 31:1c2,19a 32:8,8 33:15,0 34:18,18 35:2,28 36:8,8 37:8,8 38:c,c
39:8,10 3a:18,18 3b:8,1ec 3c:8,4 3d:18,40
```
Probably A = body→lens and B = lens→body [?]. For example, block 7 "lens info" is 2 / 0x23 and block 9 "startup
info" is 5 / 0x0C.

### 3.3 Timing
The exchange is driven by the frame sync. The logs mention "VSYNC ERR Informations1/2 VD Rap Detect", "PACKET NUM
OVER", "nextVDCycle/nowVDCycle", "vd_counter(VFC)/(BL)". So the body sends "Info1"/"Info2" packets every VD, and the
lens must answer within the VD period. "LC T.O. Cnt" counts lens timeouts.

### 3.4 Not decoded here
The meaning of most block ids, the exact init sequence after the handshake (expected order: capability bitmap →
lens info 7 → startup info 9 → periodic Info1/Info2), and the byte layout of lens info (focal range, aperture, lens
id). They need either more RE of `436b4000–436b6000` (make side) or a logic-analyser capture of a real lens.

### 3.5 `lensbin_lensproperty/LensAberrationData.bin`
Header `"II"`, u16 0x0100, u16 count = 32, u16 0x80. Then 32 × `{u16 lensId, u16 0x000F, u32 offset}`. The lens
ids are 0x1C–0x46 plus 0x63E3 and 0x64BF; the id↔model mapping was not checked [?]. The per-lens records hold
focal/aperture breakpoints and correction curves. av-cam's `Camera::Lens::Initialize` (`436e58f8`) loads the file
through userland libOnDemandLoader (AsyncSend 0xBD017A). `SetAberrationDataInfo(u16 lensId)` /
`job_Mount_LoadAberrationDataHndl` select a record when a lens is identified. The file is only image-correction data
(shading/CA/distortion). An unknown id means "no correction", not a failure [?].

---

## 4. Experiment: empty mount vs. current default

Two runs from the pristine NAND, with no SD card:

| run | P0.9 seen by firmware | BL state after boot | `+0x38..3B` |
|---|---|---|---|
| default model (GPIO ext = 0) | 0 → lens present | 0xFF (500 power polls, then gave up) | 00 01 01 00 |
| diagnostic: detect pin globals repointed to P5.2, which idles high (data poke of 0x43E2E824/48 via gdb, code untouched) | 1 → no lens | **0** (idle, no power request) | 00 01 00 00 |

Both runs reach the shooting-screen OSD. With an empty mount the firmware takes its real "no lens" branch
(`436b79ec(0)`, state 0/15) instead of the error state.

---

## 5. Emulation plan

### 5.1 Stage A — correct "no lens" (cheap, firmware-faithful)
* Add a machine option `lens=none|ekit` (default `none`). For `none`, set the GPIO external level **P0.9 = 1** at
  reset. A real empty mount has the detect line pulled high. The code shows that low means "attached";
  the pull-up itself is [?] until confirmed on hardware.
* This replaces today's "lens present but dead" state. Then 'Release w/o Lens' (§6) behaves as it does on a body
  with nothing mounted.

### 5.2 Stage B — power and handshake (for `lens=ekit`)
1. **P0.9 = 0.**
2. **Sub-micro HLE:** when the CPU's slot-B TX TLV 0x0A data[1] bit0 = 1, answer with slot-B TLV 0x03 data[3]
   bit0 = 1 (clear it when the request drops). This belongs in the existing sub-micro model in `sony_a5000.c`,
   because the real micro switches the lens supply.
3. **Handshake echo** (board model, after ~1 ms): P0.5 := P5.4 output latch, and P2.10 := P2.11 output latch.
   This must stay active only while P2.10/P2.11 are in GPIO mode. After `43993910` muxes them to UART, ignore them.
4. Optional: log P3.24/P3.26/P7.11 (power class) for diagnostics.

### 5.3 Stage C — serial lens HLE
* **New device file** `hw/misc/cxd90014_lens.c` ("a5k-lens", an HLE of the lens MCU; reason: no lens firmware is
  available). It connects to **UART1** and has two sides:
  * byte stream in: the bytes the CPU writes to UART1 DR. They arrive by DMA ch0, so **`cxd90014-mdma` needs a
    peripheral mode**: when a channel's dst is 0xF2001000, feed the bytes to the lens instead of memory.
  * byte stream out: reply bytes. They go into the UART1 RX FIFO, or straight into the RX DMA ch1 buffer at the
    programmed length, and then the ch1 done IRQ (GIC 174) fires.
  * Alternatively, add DMACR/DMA-request support to QEMU's `pl011` and pair it with mdma request lines 12/13 [?].
* **Replies a static lens must give**, all with the §3.1 framing and checksum:
  1. Capability bitmap: the blocks it supports (`436cc6b0`). The minimum set is 5, 6, 7, 9 and the periodic ones.
     The exact block that carries the bitmap is not decoded [?].
  2. Block 7 "lens info" (0x23 bytes): power class at +34 (relative to the copy base `+33`, so the byte offset in the
     block is [?]) = 1 (a PZ lens may be class 2 [?]); +35 bit1 = 0 (stay at 750 kbaud); firmware-error byte +38 = 0;
     the lens id must be one listed in §3.5 or unknown.
  3. Block 9 "startup info" (0x0C bytes): bytes 8..9 = 0.
  4. Periodic type-1 Info packets every VD with blocks 5/6 (focus/aperture/zoom status). Their content can stay
     constant: fixed focal length and F-number, AF "in focus".
* Decoding the content of 1, 2 and 4 is the main missing piece (§7). Static RE of the make/parse pairs at
  `436b4000–436b6000` and `436bb000–436bc000` can recover it. So can a capture of a real 16-50 on the mount pins.

---

## 6. Fallback: "Release w/o Lens"
* Menu setting `CustomReleaseWOLens` (settings_tree, setting id 13 = 65). libObj PARAMID **0x2EC**
  (`PARAMID_DATA_NO_LENS_RELEASE`), values 594 = on, 595 = off (`sub_3B70B0`/`sub_3B70FC`). It goes out as
  `MSGID_SET_NO_LENS_RELEASE_CMD`, which `CamMsgConv` (`sub_86957C`) turns into liro command **0x4085** for av-cam.
* **Backup id: not found** [?]. It is not among the named hints of the 0x107 menu-written list. To recover it, diff
  Backup.bin before and after toggling the setting on the real body
  (`re/backup/whitelist/diff_backup.py`), or follow the av-cam handler of 0x4085.
* **Does it remove the need for a lens model?** Partly. It lets the shutter release when the body has decided
  that there is **no lens** (Stage A, P0.9 high). That is enough for UI/menu work and capture-pipeline tests
  without lens data. It does not help in today's default state (detect low, power never acknowledged), because
  that state is an error, not "no lens". Lens-dependent features (AF, zoom, aperture display, aberration
  correction, PZ lever) still need Stage C.

---

## 7. Open questions
1. Polarity/pull of P0.9 on the board (code: low = attached). Is there a debounce or IRQ on it, or is it polled
   only from `436b8b10`?
2. Exact semantics of DMA request numbers 12/13 and of the peripheral-mode descriptor (count in bits [20:8]) in
   the 0xF0102000 DMA.
3. Who sends the first packet after the handshake, and the order of init commands. Which block carries the
   capability bitmap.
4. Layout of block 7 (lens info), and of blocks 5/6 (periodic status), including where focal length and F-number
   live.
5. The av-cam → userland notification that drives the "attach a lens" caution, and the Backup id of
   'Release w/o Lens'.
6. Meaning of P7.11 and of the two power classes (P3.24/P3.26: probably the motor supply voltage or current limit
   for PZ / non-PZ lenses [?]).
7. The 1.5 Mbaud switch (`+0x3C`, "UART_HZ NG") and the lens firmware-update path (`FirmUpReq`, `CAIF CASND
   LUPDT_*`); neither is needed for a static lens.

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

---

## 8. Implementation status

Stages A and B of §5 are implemented. Stage C (UART lens HLE) is not.

* **Machine option `lens=none|ekit`** (default `none`).
  * `cxd90014-gpio` property `lens-attached`: P0.9 idles **high** (empty mount) unless set.
  * The GPIO model has new named outputs `"out"` (32 per port) that report **output-latch** changes. The latch is
    used, not latch AND direction: av-cam drives P5.4 with its +0x00 direction bit clear (observed `dir5 = 0x4000`,
    `out5 = 0x4010`), so the meaning of +0x00 is not settled [?].
* **`lens=ekit`**:
  * P0.9 = 0.
  * Sub-micro HLE: CPU slot-B TX TLV 0x0A data[1] bit0 sets RX TLV 0x03 data[3] bit0, and clearing it clears the bit.
  * Board: P0.5 := latch(P5.4) and P2.10 := latch(P2.11), each 1 ms after the latch changes. The echo is not
    switched off when P2.10/P2.11 are muxed to UART1, because no pin-mux register is modelled.

Observed (unmodified firmware, pristine NAND, no SD; BL Info at PA 0x83D707C8):

| run | state | `+0x10` | `+0x38..3B` | screen |
|---|---|---|---|---|
| before (P0.9 = 0, no power ack) | 0xFF | – | `00 01 01 00` | shooting OSD. A shutter press shows nothing, not even "NO CARD" |
| `lens=none` | **0** (idle) | 0 | `00 01 00 00` | shooting OSD. A shutter press shows "NO CARD", so the release path runs |
| `lens=ekit` | **1** | **2** | `00 01 01 01` (power acknowledged) | shooting OSD with "NO CARD" and "±0.0" |

* **No "attach a lens" caution was seen** in any run without SD, before or after this change. That includes the
  pre-change first-boot flow (area → date/time → PlayMemories → shooting OSD). The README's earlier "lens warning"
  step could not be reproduced at `cbe7bd3`. With SD the boot stays black at 45 s on the pre-change build as well,
  so the SD + no-lens case (where the caution would be expected) was not checked.
* **How far `lens=ekit` gets.** Trace on `436b7a98` (the §2.4 sequencer): it is **event-driven, not polled every
  5 ms**. Event 1 → P5.4 := 1. The echoed P0.5 edge (GPIO IRQ 126) → event 2 → P2.11 := 1, and P2.10 is echoed.
  After that the sequencer is **never called again**: no event 3, no `436b7a44` (UART1 open), no `4399387c`
  (TX DMA), no "POWER SEQ TIME OUT". P5.4 stays 1, UART1 CR = 0x300 and DMACR = 0. The source of the next event is
  not known: P2.10 has no pin interrupt in the model (only p0.0–16 and p1.0–6 do), or the event may come from a
  timer. That needs RE before stage C.

---

## 9. Handshake completion and stage C spec

Method: IDA 9.4 on a copy of the lens IDB (`C:\a5000\emu\lens2\`), plus runs of the unmodified firmware with
`lens=ekit` (pristine NAND, no SD). Runs used `-trace cxd4115_*`, the a5ktrace plugin and gdb breakpoints that only
read memory. One experiment also wrote a timer register from gdb to imitate the hardware behaviour proposed in §9.1
(that is a debugger poke, not a model change). Facts carry their evidence; guesses are marked [?].

### 9.0 Corrections to earlier sections
* §8: the second handshake event is **not** the P0.5 edge (GIC 126). It is the expiry of a 5 ms software timer.
  The handshake code never waits on a pin interrupt (§9.1).
* §1.3: the two DMA channels are the other way round. **ch0 (`4399387c`) is RX** (UART1 → 512-byte ring) and
  **ch1 (`43993bd4`) is TX** (packet buffer → UART1). Evidence in §9.4.
* §3.2: both lengths in the block table **include the id byte**. A = body→lens block length, B = lens→body block
  length (§9.5).

### 9.1 What produces the third event: timer ch10 one-shot, and the START bit must self-clear
* `436b7a98` (handshake) ends every call that has not finished with `43a4a7ac(5000, 43a4a861)` →
  `43ae2088(us, cb)`. That call programs **cxd4115 timer channel 10** (0xF2403A00, GIC **163** = 153 + 10) as a
  one-shot with a 5000-count compare: `439193b0(10, {5000, 2, 1, 0}, cb)` + `43919404(10)`. The timer is registered
  at init by `43ae206a` (IRQ 0xA3 = 163 and 0xA8 = 168, channels 10 and 15). On expiry, ISR `4391926c(10)` writes
  CLR (+4) bit0 and calls the callback. `43a4a860` → `436bbeb0(255)` posts the event that runs `436b8b10` →
  `436b7a98` again.
* av-cam timer driver (`43919xxx`, base 0xF2403000 + 0x100·ch). The setup sequence writes:
  `CTRL[2:0] = cfg[1]` (2 here), `CTRL[5:4] = cfg[2]` (1 = one-shot), `CTRL[16] = cfg[3]`, `CMP = cfg[0]`,
  `CTRL[8] = 0`. The start sequence writes: CLR |= 1, CLR |= 0x10, `CTRL[8] = 1` if a callback is registered, and
  CTRL |= 0x1000 (START). The trace shows 0x0, 0x2, 0x12, 0x12, CMP=0x1388, CLR=1, CLR=0x10, 0x112, 0x1112.
* **Both `439193b0` (setup) and `43919404` (start) return -3 ("busy") without touching the channel when CTRL bit12
  (START) reads 1** (`439194b0` = `UBFX CTRL,#12,#1`, checked at `439193c4`/`43919414`). Nothing in the ISR, in the
  callback or in the driver clears bit12 after a match. So on the real chip, **a one-shot match (CTRL bit4 set)
  must clear CTRL bit12 in hardware**. Without that, no one-shot user could ever re-arm.
* Run-time proof (plugin on `439193b0`/`439193f4`/`43919404`/`4391944e`): the first arm works and fires
  (`cxd4115_fire ch10 match ctrl=0x1112`). The second arm, from the callback-driven call, **returns -3 at
  `439193f4` and `4391944e`**, because the model's `chan_tick` sets `running = false` but leaves `ctrl` = 0x1112.
  So the "event-driven" behaviour in §8 is really "the timer could not be re-armed".
* The same bug hits **channel 11** (GIC 164). It is the `[Imager]PowerOn/PowerOff` wait timer (`436b0a1c`, called
  from `436af414`/`436af632`, logs "PowerOn Wait No.xx TimerError!!"). Its second arm also returns -3 in the trace.
* **Experiment.** A gdb breakpoint at `439193b0` cleared CTRL bit12 of ch10/ch11 before every arm, which imitates the
  proposed hardware behaviour. The handshake then **completes**: `436b7a44` (UART1 open, 750 kbaud) runs,
  `436b4908(1)` runs, the body sends its first packet (§9.6), and the RX ring is armed. With no lens answering, the
  firmware logs `[BL]SYNC TIMEOUT` → `DETACH RETRY count:1..4` → `FORCE LENS DETACH count:4` (av-cam log ring, PA
  ~0x830E6000) and ends in BL state **0xF2 (242)**, with +4 = 0xF1. `POWER SEQ TIME OUT` never appears.

**Spec (timer model, `cxd4115_timer.c`):** when a channel matches and CTRL bit4 (one-shot) is set, clear CTRL
bit12 in the register value (so a read of +0x00 shows START = 0) as well as stopping the counter. Keep the pending
flag and the IRQ as they are now. The kernel's use (it writes 0x0112 and then 0x1112 itself every time) is
compatible with this. [?] Whether a match with bit4 = 0 and bit8 = 1 also auto-stops is not needed by anything
seen here.

### 9.2 GPIO direction register, pin mux, interrupt lines
* **+0x00 bit = 1 means output.** av-cam's read helper `43ac774c(port, pin)` returns the output latch (+0x40) when
  the +0x00 bit is set (`43917cd4`), and otherwise the pad input (+0x10). The UART open table also sets dir = 1 on
  TXD and dir = 0 on RXD.
* **P5.4 is driven with dir = 0.** Nothing in av-cam writes the P5 +0x00/+0x30 bits for pin 4. At UART open the
  run shows `P5: +0x00 = 0x4000, +0x30 = 0x4040, latch = 0x4010`. Most likely the boot loader or kernel board code
  makes P5.4 an output, and the model's all-zero reset does not do that [?]. Keep the current rule: the board echo
  follows the latch, not latch AND dir.
* **Pin configuration call `43ac76be(port, pin, cfg)`, cfg = {u8 mode, s32 dir, u8 ien, s32 pull}:**
  `mode` → GPIO +0x34 (1) or +0x38 (0). `dir` ≥ 0 → +0x04/+0x08, and then `ien` → +0x24/+0x28. dir < 0 → input
  enable cleared. `pull` ≥ 0 → pad +0x48 (clear), then pad +0x54 (pull = 1) or +0x58 (pull = 0). pull < 0 →
  pad +0x44 (set) and pad +0x58 (clear) [? pull enable / pull select].
* **Second register bank ("pad/function") at 0xF2912000 + 0x100·port**, triples (value / +4 set / +8 clear) at
  +0x00, +0x10, +0x20, +0x30, +0x40, +0x50. Accessors are `43917d78`…`43917e94`. Today the emulator backs this
  range with a plain RAM-like `io-f0` region, so set/clear writes are not folded into the value.
* **UART1 mux (`43993910`, table 0x43ED70E4; close `43993b44`, table 0x43ED70AC).** Both tables hold 28-byte entries
  `{port, pin, mode, dir, ien, pull, action}`. Port and pin are patched at boot from Backup (P2.11, P2.10). Action 1
  → `43ac7778` → pad +0x04 (set) for ports in mask 0x1BFDE. Action 0 → `43ac7784` → pad +0x14 (set) for ports in
  mask 0x3F76E.

  | pin | open (UART) | close (GPIO) |
  |---|---|---|
  | P2.11 (TXD) | mode 0, dir 1, ien 0, pull 1, pad+0x00 bit set | mode 1, dir 1, ien 0, pull -1, pad+0x00 bit set |
  | P2.10 (RXD) | mode 0, dir 0, ien 1, pull 1, pad+0x10 bit set | mode 1, dir 0, ien 1, pull 0, pad+0x10 bit set |

  **The bit that switches the pin between UART and GPIO is GPIO +0x30 (the "mode/pull" triple): 0 = peripheral
  function, 1 = GPIO.** The pad bits are set in both tables, so they do not select the mode. Run values: after
  close, P2 +0x30 = 0xC20 and pad 0xF2912240..58 hold 0x800/0x400. After open, P2 +0x30 = 0x020. Before the first
  open, P2 +0x30 bits 10/11 are already 0, only because the model's reset value is 0 [?: real reset/boot value].
* **Interrupts.** The handshake needs none. P0.5 gets GIC 126 (121 + 5), set up by `43993ad4(1)` only when cfg byte
  `43CC5B30[10]` == 1, and its handler is null (§1.1). `43ac7784(0, 5)` is a no-op for port 0 (not in mask
  0x3F76E). **No GPIO IRQ is needed on port 2.**
* **Spec (board echo, for `lens=ekit`).** Keep P0.5 := latch(P5.4) after 1 ms. Make the P2.11 → P2.10 echo part
  of the lens model's state instead of a fixed wire: echo only while the lens is in "handshake" state. The lens
  leaves that state when P5.4 falls after P2.10 has been echoed high (step 2 → 3 of §2.4). After that, P2.10 is the
  lens TX line (UART RXD of the body) and must not follow P2.11. Optionally, gate the echo on GPIO P2 +0x30 bit
  11 == 1 as well, once the reset values are settled [?].

### 9.3 Sequence after the handshake (observed with the timer fix, plus static)
1. `436b7a98` step 2 → `436b7a44`: `43a4bc60` (reset the TX/RX slot tables, `439931bc`), `43993910` (pins to UART,
   clocks, `43919d70(1)`, `43993798(0)` = 750 000 baud), `43a4bc64`/`43a4bd18` (pick an RX slot), then
   **`4399387c(PA 0x83CC96F0)` = arm the RX ring** (ch0, 512 bytes). P5.4 := 0. P0.5 is expected to fall.
2. The step-3 timer expires → `436b4908(1)` (comm enabled) → the main handler (state 1, sub-state at BL+8 = 0)
   sends **block 1** (the body's capability bitmap, `436b7c34`) → sub-state 15.
3. Each request goes through `436b5188(blocks, cb)` → a queued packet. Event 2 (`436b5618`) copies it to the TX
   buffer (PA 0x83CC70B0, `43992bf0`) and starts **TX DMA ch1** (`43993bd4`), which also drives **P5.4 := 1**.
   - TX DMA done → `43993828` → ch10 450 µs (`dword_43ED711C` = 0x1C2) → `43993c7c`: TX DMA off, ch1 stopped,
     UART flush (`4391a0dc`), ch10 re-armed for 300 µs with the callback passed to `43993bd4` (`43a4a52b`), then
     **P5.4 := 0** (tail call `43993ad4(0)`). So P5.4 is high for the body's whole transmission + ~450 µs. The
     0x1C2 value is set by `43993798` for 750 k (mode 0); mode 1 uses 225.
   - 300 µs after P5.4 falls, `43a4a52b` posts event 3 → `43a4a680` → `436b4ef0`, `436b4a1c(0)`.
   - Event 7 (`436b4e38`) arms the **sync timeout**: ch15, `43a4a7b0(0xE4E1C, 43a4a565)`, mode 7 [? clock; ~1 s
     if 1 MHz]. Its expiry runs `436bbe60(-1)` → parser `436b61d8(-1)` → `[BL]SYNC TIMEOUT` when a request is
     pending.
4. **RX path.** `43a4a730` runs in IRQ context (lr = kernel IRQ return 0x3F40127C). It has no static caller [?:
   registered at run time, most likely on the VD interrupt, see §3.3]. It calls `43993840`: received = (ch0 **+0x04
   current dst pointer** − ring start) + up to 16 bytes read from the UART RX FIFO by PIO (`4391a006`) and appended
   at that pointer. Then ch0 is stopped. If 1..512 bytes arrived, it moves to the next RX slot, re-arms ch0 and
   posts `436bbe60(slot)` → `436b61d8(slot)`. Otherwise it only re-arms. **One RX slot = one packet.** The parser
   rejects the slot with DATASIZE_NG when the header length ≠ the received count.
5. Init order (state 1, sub-state = BL+8; class-1 path through the sub-micro). The handlers are in `436b8b10`:

   | sub | body sends (A bytes) | waits for lens block (B bytes) | then |
   |---|---|---|---|
   | 0 | handshake, then **[01 + 32-byte body bitmap]** (33) | – | 15 |
   | 15 | – | **01** (33) → copied to lensInfo+0 (`436b7ca0`) | sends [07, x] (2) → 16 |
   | 16 | – | **07** (35) → lensInfo+33 and +278 (`436b7cd8`) | lensInfo[35] bit1 set: sends [0C, 02] → 23 (baud change). Otherwise `436b7e5c`: class 1 → P3.24/26 mode 1, wait 100·tbl[64] µs; class 2 → mode 2, wait 100·tbl[65] µs → 10 |
   | 10 | [0B, 0x60, 1] (3) (`436b767c(96,…)`) | – | 21 |
   | 21 | – | **0B** (3) present | lens supports 50 → [32 …] (8) → 30; supports 61 → [3D, 0×23] (24) → 31; neither → clear lensInfo+313, send [08 …] (9) (`436b89ec`) → 17 |
   | 30 / 31 | – | **32** (8) → lensInfo+270 / **3D** (64) → lensInfo+313 | → 31 or send [08 …] → 17 |
   | 17 | – | **08** (202) → lensInfo+68, `436b74b4` | sends [09 …] (5) (`436b8970`) → 18 |
   | 18 | – | **09** (12) startup info (`436b7ef8`) | sends [0D, class] (2) (`436b7768`) → 24 |
   | 24 | – | **0D** (2) present (`43a4a890`) | sends [10, flags] (2) (`436b787c`) → 25 |
   | 25 | – | **10** (2) (`436b7c60`): data[0] bit0 = 0 is OK | sends [0A + 16 bytes camera info] (17) (`436b8084(1)`) → 19 |
   | 19 | – | **0A** (17) (`43a4a8f2` → `436b70cc`) | BL+8 = 1 (done), posts (5, 0x2000/0x2001) |

   The lensInfo layout follows from the copy lengths (B, id included): +0 blk 1 (33), +33 blk 7 (35), +68 blk 8
   (202), +270 blk 50 (8), +278 blk 7 copy (35), +313 blk 61 (64).

### 9.4 DMA (0xF0102000 unit, channel k at +0x100 + 0x20·k), as programmed for the lens
Registers read back right after `439174d0` (the run used the current model, which stores the writes):

| ch | role | +0x00 | +0x04 | +0x0C cfg | +0x10 ctl |
|---|---|---|---|---|---|
| 0 | **RX** UART1 → ring | 0xF2001000 (UART1 DR) | 0x83CC96F0 (ring) | 0x9940B200 | 0x0100D018 |
| 1 | **TX** buffer → UART1 | 0x83CC70B0 (buffer) | 0xF2001000 | 0x1609900C, then 0x9609900C | 0x01000B40, then 0x0100CB40 |

* **+0x00 = source, +0x04 = destination**, the same as for entry_pre's memory copy and the existing model. The
  driver's descriptor is `{src, dst, cfg}`, and `43917944` writes it to +0x00/+0x04/+0x0C. The earlier "TX"/"RX"
  naming in §1.3 was swapped. Proof: ch1 copies the packet that `43992bf0` has just placed in 0x83CC70B0 (§9.6),
  and `43993840` measures RX progress through ch0 +0x04.
* cfg (+0x0C, packed by `439174d2`): **bits[11:0] = count in source units**: ch0 = 0x200 = 512 bytes, ch1 = 0x00C
  = 12 words = 48 bytes. **bit31 = completion IRQ enable** (`43917a4c`). The other fields [?] come from the
  driver descriptor byte {bit0 src-increment, bit1 dst-increment, [4:2] src width, [7:5] dst width}: ch0 descriptor
  0x65 (src fixed, dst incr, src width 1 = byte [?], dst width 3), ch1 descriptor 0x2E (src incr, dst fixed, src
  width 3 = word [?], dst width 1).
* Channel config (`439173f8`, byte 0 bits[4:2] = transfer type, byte 1 = source request line, byte 2 =
  destination request line). ch0: type 2, src request **12** (UART1 RX). ch1: type 1, dst request **13** (UART1
  TX). [?] Which bits of +0x10 carry the request numbers: compare 0x0100D018 and 0x01000B40.
* Callbacks: ch0 `nullsub_344` (does nothing; cfg bit31 is set). ch1 `43993828` on completion, through GIC
  173 + 1 = **174** (unit status +0x00 bit1, ack +0x08). Unit status read 0x41/0x43 in the run (ch0, ch1, ch6 bits).
* PL011 side: `4391a08c(1, 1, 0)` after arming ch0 = DMACR RXDMAE on, DMAONERR off. `4391a040(1, 0/1)` around ch1
  = DMACR TXDMAE off/on (standard PL011 bit meanings [?]).
* **Why the current model breaks this:** `cxd90014-mdma` copies at once on start, taking the word count from
  cfg[11:0]. That turns ch0 into a 2048-byte copy *from* the UART1 register file *into* the RX ring: the run shows
  the ring filled with the PL011 register image (0x90 = FR at +0x18, IMSC 0xFF0 …). It also makes ch1 write 48
  bytes over UART1's registers.

**Spec (mdma peripheral mode).** Treat a channel whose source or destination is 0xF2001000 as peripheral-paced.
* TX (dst = UART1 DR): hand `count` bytes (word source: 4·count bytes, little endian) to the lens model. Raise
  done (+0x00 bit k, GIC 173 + k) after the serial time (10 bits/byte at 750 kbaud ≈ 13.3 µs/byte), or at once if
  that is simpler.
* RX (src = UART1 DR): do not copy. Keep the channel armed. As the lens model emits bytes, write them to
  dst + n and **advance +0x04 (current destination) to dst + n**. Stop at `count` bytes. Done for ch0 (GIC 173) only
  reaches a no-op callback; the firmware never waits for it.
* Leave the memory→memory path unchanged.

### 9.5 Frame and block rules for the lens side (static `436b61d8`, block table §3.2)
* Frame: `F0 | u16 len LE | type | b4 | blocks | u16 sum LE | 55`, sum = Σ bytes[1 .. len-4]. Checked in this
  order: header, footer, checksum, len == received count, `(type & 7) ∈ 1..3`.
* Blocks are walked from byte 5. Each block is `{id, data}` and its total length = table **B** of that id (id
  byte included). Ids 5 and 6 are variable (`436b7110`/`436b7128`). An id with A or B = 0 aborts with
  `MOUNT_ERR_ANALYSE_NG`.
* **Reply to a request:** type 2. **Byte 5 (the first block id) must equal the pending command id** (BL comm
  struct +0x28). Then the registered callback runs. Byte 4: not checked for type 2 [?]; echo the request's byte 4.
* **Capability bitmap = block 1.** `436cc6b0(id)`: id 1 is always allowed. For other ids,
  `lensInfo[1 + ((id-1)>>3)] >> ((id-1)&7) & 1` must be 1, and the body's own per-mode table (`dword_43EDCE68` +
  32·`byte_43E6452E`) must allow the id too. So the lens's block 1 = `01 | b0 | 32-byte bitmap`, where bit (id-1)
  of the bitmap = "id supported" (lensInfo[0] = the id byte 01). The body sends the same form with ids 1..0x3D
  set (§9.6).
* Block 7 (35 bytes) → lensInfo[33..67]: [34] bits1:0 = **power class** (1 or 2, else LENS_TYPE_UNKNOWN), [35]
  bit1 = 1.5 Mbaud capable (use 0), [38] = firmware error (must be 0, else 241 LENS FIRMERR). [?] Other fields used
  later: [43..44] u16 compared with 0x8019 and [62] bit0 (`436b767c`), [59] bits 2 and 6 (`436b787c`,
  `436b89ec`). The lens id / focal length / F-number are **not decoded**. They are in blocks 7/8/50/61 or in the
  periodic blocks 5/6 [?].
* Block 9 (12 bytes): pointer v = block start (id at v[0]). For a class 2 lens, v[1] & 3 selects the drive mode
  (1 → 5, 2 → 2). **v[8] and v[9] must be 0** (or camera power state 0xC06), else "SYNC START UP INFO ERR".
* Block 16 (2 bytes): v[1] bit0 = 0 is accepted. Blocks 0x0B (3), 0x0D (2), 0x0A (17) only need to be present (the
  body copies 17 bytes of 0x0A into `436b70cc`, content [?]). Block 8 (202 bytes) goes through `436b74b4`, which
  calls a hook and then checks the capability of id 59 [?: zero-filled content untested].

### 9.6 First packet on the wire (captured)
At `43993bd4` (ch1 arm), TX buffer PA 0x83CC70B0, ch1 count 12 words = 48 bytes:
```
F0 29 00 02 00 01 FF FF FF FF FF FF FF 1F 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 44 07 55  + 7 x 00 padding
```
len 0x29 = 41, type 2, b4 = 0, block 1 = {01, bitmap ids 1..61}. The sum Σ bytes[1..37] = 0x0744 checks.
Identical on all 4 retries. **A minimal lens reply** (type 2, block 1, 41 bytes):
`F0 29 00 02 00 | 01 b0 <32-byte bitmap> | sum16 | 55`. Its bitmap needs bits for ids 1, 5, 6, 7, 8, 9, 0x0A,
0x0B, 0x0D, 0x10 (plus 0x0C only if 1.5 M is offered). Leaving 0x32 and 0x3D clear skips sub-states 30/31.
Value of b0: [?] (use 0).

### 9.7 Lens HLE (stage C) checklist for the code agent
1. Timer fix (§9.1). Without it nothing after the handshake runs.
2. mdma peripheral mode for 0xF2001000 (§9.4). PL011 at UART1 needs no IRQ (callbacks are null). The RX FIFO can
   stay empty if all bytes go through the ring.
3. Lens state machine: idle → echo H1/H2 (§9.2) → UART. In UART mode, parse body frames from ch1 and answer each
   request with one type-2 frame. Start the reply after the body's frame has fully left (P5.4 is high during TX,
   and the reply can go out after P5.4 falls [?]). Finish it well inside one VD period (the poll in `43a4a730`
   takes whatever has arrived). Real timing [?]: replying ~0.5–1 ms after P5.4 falls is a safe target.
4. Replies per §9.3 table / §9.5. The content after the startup sequence (periodic type-1 frames with blocks 5/6,
   AF/zoom/aperture) is still undecoded.

### 9.8 Open questions
1. The IRQ source that calls `43a4a730` (RX poll), and so its period (VD [?]). Find it with a plugin probe that
   also prints the GIC IAR, or by finding where av-cam registers the pointer.
2. ch15 / mode 7 timer clock (the sync timeout is 0xE4E1C counts).
3. Meaning of the timer CTRL bits 8 and 16 and of bits[2:0] above 3 (the model uses `ctrl & 3` as the prescaler).
4. The GPIO/pad reset state that the boot loader leaves (P5.4 direction, P2 +0x30 bits). Needs a register snapshot
   from a real camera or the kernel board setup.
5. Content of blocks 8, 10, 50, 61 and of the periodic blocks 5/6 (focal length, F-number, lens id). Next step: with
   §9.1 and §9.4 implemented, capture each body request in turn (gdb on `43993bd4` dumping PA 0x83CC70B0) while
   replying minimally, and decode the consumers of lensInfo+68/+270/+313.
6. Exact bit layout of DMA +0x10 (request lines, type) and of cfg bits[30:12].

---

## 10. Implementation notes (stage C, emulator)

Implemented on branch `lensc`. The §9.7 checklist is done; the lens is recognised and the whole startup
handshake runs. Files: `hw/misc/cxd90014_lens.c` (new, `a5k-lens`), `hw/dma/cxd90014_mdma.c` +
`include/hw/dma/cxd90014_mdma.h` (peripheral-paced mode), `hw/arm/sony_a5000.c` (wiring, `lens=ekit`).

### 10.1 What runs
* **Timer fix (§9.1)** was already in the base (`cxd4115_timer.c`: a one-shot match clears CTRL START,
  commit `9c364ce`); no change needed. The handshake completes and `436b7a44` opens UART1.
* **mdma peripheral mode (§9.4).** `cxd90014_mdma_set_periph(dev, 0xF2001000, ns/byte, tx_cb, pull_cb, op)`.
  A channel whose `+0x00` (src) or `+0x04` (dst) equals the UART1 DR is peripheral-paced:
  * **TX** (dst = DR, ch1): the `count·width` bytes are read from the source and handed to the lens; a timer
    raises the channel's done bit (GIC 173+ch) after the serial time. Width from cfg[17:15] (1→1, 2→2, 3→4 B).
  * **RX** (src = DR, ch0): stays armed; the lens writes reply bytes at `+0x04` and advances it (what
    `43993840` measures), done when the count is used up. The memory→memory path is unchanged.
  * The driver read-modify-writes `+0x10`, so the model acts only on a bit0 edge.
* **`a5k-lens` state machine.** OFF (no supply) → HS (echo P5.4→P0.5, P2.11→P2.10) → UART (when P5.4 falls
  after P2.10 has been echoed high, §2.4 step 2→3). In UART mode it parses each body frame (§9.5 rules) and
  answers every requested block in order with its block-table **B** length. Block 1 = capability bitmap (ids
  1,5,6,7,8,9,0x0a,0x0b,0x0d,0x10); block 7 `[1]` = power class (`power-class` prop, default 1); blocks
  9/10 carry the minimum (`[8]=[9]=0`, `[1]`bit0=0); all other blocks are zero-filled (presence only).
* **Reply timing.** A reply is marked *due* `LENS_REPLY_NS` (0.6 ms, H) after the body's P5.4 falls, then
  copied into the RX ring at the body's **next read of the RX channel `+0x04`** (a "pull" callback). This
  keeps the delivery in the vCPU's own MMIO order: writing the ring from the reply timer thread raced the
  body's `43993840` (read `+0x04` → stop ch0 → re-arm), and bytes written in that window were lost, which
  showed up as `[BL]SYNC TIMEOUT` at random init steps. With the pull, every init block is taken first try.

### 10.2 Observed (unmodified firmware, pristine NAND, no SD; BL Info PA 0x83D707C8)
| run | BL state | init blocks answered | screen |
|---|---|---|---|
| `lens=none` | **0** (idle, no lens) | – | shooting OSD, "NO CARD" on shutter; unchanged from §8 |
| `lens=ekit` | **0x05** (recognised, communicating) | 01, 07, 0b, 08, 09, 0d, 10, 0a (full §9.3 sequence) | shooting OSD; MENU and PLAY work |

`[BL]` log on `lens=ekit`: one `SYNC TIMEOUT` / `DETACH RETRY count:1` on the very first power-up cycle (before
the lens' first init completes), then it recovers and stays at state 0x05 — no `count:4` / `FORCE LENS DETACH`,
no `LENS FIRMERR`, `LENS_TYPE_UNKNOWN` or `POWER SEQ TIME OUT`.

### 10.3 Still "F--"
The live-view F-number, focal length and AF/zoom status are **not produced**. Two reasons, both pre-existing and
documented: (a) the aperture / focal / lens-id byte layout inside blocks 7/8/5/6 is undecoded (§9.5, §9.8 Q5);
(b) the periodic type-1 Info1/Info2 frames that carry live aperture are VD-driven and were never seen after the
startup sequence in the emulator (no live-view ISP path, so no VD-rate lens polling). Fabricating aperture bytes
was deliberately avoided (wrong values risk `LENS FIRMERR` / `LENS_TYPE_UNKNOWN`). Decoding (a) and driving (b)
are the remaining work for a real F-number display.

### 10.4 Properties / options
`-M lens=ekit` builds the `a5k-lens` wired to mdma unit 0 and the GPIO. `a5k-lens` props: `power-class` (block 7
`[1]`, default 1), `drive-mode` (block 9 `[1]`, class-2 only). `-global a5k-lens.power-class=2` was exercised
(class-2 path through `436B7E5C`): the init sequence is identical (the firmware still reaches state 0x05).

---

## 11. Aperture (F-number) source + encoding (2026-11-16)

Goal: let Track B read the **live lens aperture** so the native shooting OSD
(`app/native/uicore/src/ui/screens/shooting_osd.cpp`, the `if (s.apertureX100 > 0)` branch)
shows a real "F2.8" instead of the "F--" placeholder (§10.3). Method: static RE of the repo's
device-RE ledger plus one **device-verified** SDCom capture (no new device or QEMU runs). Facts
carry evidence; `[?]` = unproven.

### 11.0 Answer in one line
The live F-number for the OSD is **candidate (a): an SDCom camera parameter** — **param id 267**,
returned by message **0x1002 (CameraGetParameters)** as an **Int**, encoded as **F × 100**
(e.g. 180 → F1.8). This is already device-measured. The emulator path is **candidate (c)** (the
lens-serial frame), and is still the undecoded piece (§11.5).

### 11.1 Source (a) — SDCom param 267, device-verified
* **Evidence (real a5000, fw 1.10):** `re/NOTES.md` §10.8 "카메라 설정 조회 (CameraGetParameters
  0x1002) — 실기 성공". With the body set to M / AF-S / 1/640 / **f/1.8** / EV0 / ISO200, the 0x1002
  reply (116 params) contained **`267 = 180` (= f/1.8)** and `268 = [1,640]` (= 1/640). Logged in
  `re/scalar/camera_params.tsv:12` (`267  <no name>  180`) and `268  <no name>  [1,640]`.
* **Message:** `0x1002 CameraGetParameters` (`re/scalar/message_ids.tsv`; 146-id table, NOTES §9/§10).
  Queue/component = the standard SDCom path `connect(0x1540432, 2)` (TRACKB_RUNTIME; NOTES §10.7).
* **Param type:** Int (SDCom getType == 1). Read with `getFirstParam/getNextParam(msg, true)` then
  `SDComMsgIntParam::getData()` = the int at object `+0x0c` (object size 0x10; NOTES §10.9 rule 1,
  `camera_sdcom.cpp:47`). 267 is a scalar Int, not an array (contrast 268 = IntArray type 5).
* **Name table:** `CommandProcessorCameraSetParameters::s_ADParamKeyConvTable` (20-byte rows:
  {id, internal-key, type, idx, value-table symbol}; NOTES §10.8). 267 has **no internal key / no
  value table** in that table (hence it is a raw engine-reported value, not a menu enum). That is why
  a grep for "aperture" misses it — it is id-only.
* **Ownership:** NOTES §10.8 — **no ownership check**. If the camera is open (global ParameterEntry
  state 1 or 2, which the stock shooting screen holds), any SDCom client may query 0x1002. A second
  client (our UI) can read it with ILCE Remote / the stock app still running.
* **Already wired into the UI model:** `app/native/uicore/src/model/catalog_gen.cpp:123`
  `{ SET_APERTURE, "aperture-x100", 267, SK_RO_INT, 0,0,0,0 }` and `:124` `{ SET_SHUTTER,
  "shutter-speed", 268, SK_RO_ARR2, … }`. The model already expects param 267 as a read-only int and
  routes it to `CameraState::apertureX100`. The piece that is **missing** is the adapter read (§11.3).

### 11.2 Encoding — raw int = F × 100 (no APEX)
* **Formula:** displayed `F = apertureX100 / 100.0`. The engine returns the human F-number ×100
  directly; there is **no** `2^(av/2)` APEX conversion to do on our side.
* **Evidence:** measured `267 = 180` at a known **f/1.8** (NOTES §10.8). 180 / 100 = 1.8. The OSD
  already decodes exactly this (`shooting_osd.cpp:347`): `a%100==0 → "F%d"` else `"F%d.%d", a/100,
  (a%100)/10`. So 280 → "F2.8", 400 → "F4", 320 → "F3.2". No sign, always > 0 when valid; 0 = unknown
  (hide F), which matches `camera_state.h:40` `apertureX100  // 0 unknown (e.g. 180 = F1.8)`.
* Any APEX→F conversion happens **inside av-cam** before it populates param 267 (the lens reports in
  its own units over the serial link; av-cam converts). Our adapter consumes the already-converted
  F×100, so it needs no table. `[?]` The exact av-cam conversion is not needed for the device adapter;
  it only matters for the emulator (§11.5).

### 11.3 Read recipe for the native Track-B adapter
Target: `app/native/platform/a5000/camera_sdcom.cpp`. Today `SdcomCamera::readInfo` only fetches the
two side-effect-free M0 values (0x000d firmware ver, 0x0009 system state); `readState()` falls through
to the base default (`platform.h:68`, returns false), so `apertureX100` stays 0 and the OSD shows
"F--". Implement `readState()` as a 0x1002 query that reuses the existing `getInts`-style loop:

1. Precondition: `connected_` and the camera is open (ParameterEntry 1/2). On the stock shooting
   screen it is already open; our own `0x1000 OpenCamera` is the M1 step and is **not** needed just to
   read. Do **not** send OpenCamera from the read path (it changes engine state; keep M0 read-only).
2. Build `SDComMsgUtil::createNewMessage(0x1540432, SDCOM_MSG_REQUEST, tx)`,
   `setMessageId(msg, 0x1002)`, `sendSync(msg, &rep, 5)` (same shape as `getInts`, `camera_sdcom.cpp:88`).
3. Iterate params (`getFirstParam/getNextParam(rep, true)`, delete each copy). For the Int params
   (`getType()==1`): **id 267 → `apertureX100 = getData()`** (already F×100, store verbatim).
   Also id 266 → EV, 269 → ISO, 263 → exposure-mode, etc. (catalog_gen ids).
4. For **id 268** (shutter): it is an **IntArray (getType()==5)**, a `[num, den]` pair, not an Int —
   the current `getInts` loop drops type!=1, so shutter needs the array accessor (out of scope here,
   but note it so aperture and shutter land together).
5. Brick-safety (CLAUDE.md §1, NOTES §10.9): object sizes statically checked (`SDComMsgIntParam`
   0x10), disconnect in the crash handler, `SD_DRYRUN=1` dry pass first. 0x1002 is a pure read, so it
   is runtime-only / reboot-safe; no Backup.bin, no flash.
* **Latency note:** values settle asynchronously after a *shift* (0x1019/0x101a) — ~2 s, poll 0x1002
  (NOTES §10.10). For passive live-view display there is nothing to wait for: just poll 0x1002 at the
  OSD refresh rate and show the latest 267.

### 11.4 Alternatives (b)/(c) and why (a) wins for the device
* **(b) im.elf engine memory offset** (the [[mf-focus-distance]] +148 precedent). The 0x1002 reply is
  av-cam reading its own `ParameterEntry` store; the F×100 lives in that engine object too, so a
  focusmem-style read-only peek is *possible*. But it needs an object-resolution + offset that is **not
  decoded**, carries pointer-chase risk, and is strictly worse than (a), which is device-verified and
  already modelled in the catalog. Use (a); keep (b) only as a fallback if an SDCom client cannot
  connect. `[?]` offset unknown.
* **(c) E-mount lens-serial byte.** The lens reports aperture over UART1 in the §3/§9 frame
  (blocks 7 "lens info" and periodic 5/6). This is the **ultimate** source, but the aperture byte
  layout is undecoded (§9.5, §9.8 Q5) and on a real body the adapter should never parse the raw lens
  link — av-cam already does that and exposes the cooked value as param 267. (c) matters only for the
  **emulator** (§11.5), where av-cam itself runs and needs the lens to feed it bytes.

### 11.5 Emulator lens-model spec (for a follow-up code agent — do NOT implement here)
Why the emulator can't reuse (a) directly: in QEMU the **real av-cam runs**, so param 267 is only
populated if the emulated lens feeds av-cam the aperture over the serial link. `cxd90014_lens.c`
currently zero-fills blocks 7/8/5/6 (deliberately, to avoid `LENS FIRMERR`; §10.3), so av-cam has no
aperture and 0x1002 returns 267 = 0 / "F--". To make a mounted virtual lens report a plausible F:

1. **Decode the aperture field first (open RE, §9.8 Q5).** With the §9.1 timer fix + §9.4 mdma
   peripheral mode already in place, capture each body request (gdb on `43993bd4`, dump PA
   0x83CC70B0) while replying minimally, and trace which bytes of **block 7** (lensInfo+33, 35 B) and
   the **periodic type-1 blocks 5/6** (focus/zoom/aperture status) av-cam reads when it computes
   param 267. Expected: a fixed min/max-F in block 7 (lens capability) and the **current av** in the
   periodic 5/6 frames. The lens most likely reports **APEX av** (av-cam converts av→F×100); confirm
   the unit by setting one known byte and reading back 267 over SDCom in the emulator.
2. **Populate it in `cxd90014_lens.c`.** Add an `aperture` (and `focal-length`) property to
   `a5k-lens` (alongside `power-class`/`drive-mode`, `lens_props`), fill the decoded offset in
   `lens_put_block` for block 7, and — the harder half — start emitting the **periodic type-1
   Info1/Info2 frames** (blocks 5/6) that §10.3(b) says were never seen after startup. Those are
   VD-driven; the emulator has no live-view ISP VD, so the lens model must synthesize them on a timer
   (or on each 0x1002) so av-cam keeps a current aperture.
3. **Fidelity guard:** a wrong value risks `LENS_TYPE_UNKNOWN` / `LENS FIRMERR` (§9.5) — only set
   bytes whose consumer is confirmed; keep block 7 [1]=power-class, [2]bit1=0, [5]=0 as they are.
4. Then host/emulator OSD testing can verify the full chain lens-frame → av-cam → 0x1002 param 267 →
   `apertureX100` → "F2.8". (On the host-only build, `fake_camera.cpp` already fakes 267 via
   `A5K_FAKE_APERTURE`, so the UI path is testable without any lens model.)

### 11.6 Confidence
* 267 = aperture, 0x1002, Int, F×100, device-measured 180=f/1.8: **high** (NOTES §10.8 실기 성공,
  `camera_params.tsv`, catalog_gen).
* "no ownership check, read while stock app open": **high** (NOTES §10.8).
* OSD decode `F = x100/100`: **high** (shooting_osd.cpp source).
* Adapter recipe (readState via 0x1002): **high** for aperture Int; **medium** for shutter 268
  (needs IntArray accessor, not yet in `getInts`).
* (b) engine-memory offset exists but offset unknown: **low**.
* (c) emulator: lens frame carries aperture — **high** that it does; the **byte offset/unit is
  undecoded** — **low/`[?]`**. Periodic 5/6 emission in the emulator is unbuilt.

### 11.7 Emulator decode + measurement (2026-11-16): why the virtual lens cannot light the F-number yet

Follow-up to §11.5 (the "do NOT implement here" spec). Method: a run of the unmodified firmware
with `lens=ekit` (pristine COW NAND, no SD), the `a5k_lens_*` trace, HMP `xp` reads of av-cam memory
(PA = VA + 0x40000000), IDA 9.4/Hex-Rays on `lens_avcam.i64`, and one black-box mapping experiment
(a debug build that filled the block-8 reply with an incrementing pattern; reverted). All facts carry
evidence; `[?]` = unproven.

**Finding (high confidence): the live F-number is produced only by the live-view / AE path, which does
not run in the emulator. A lens-model change alone cannot make av-cam report param 267 or show an F on
the OSD today.** Three independent measurements:

1. **av-cam never polls the lens after startup.** With `lens=ekit` the body requests exactly the eight
   startup blocks — `01 07 08 09 0a 0b 0d 10`, all **type 2**, once per power-up cycle (16 frames =
   8×2 cycles, matching the one DETACH-RETRY recovery of §10.2) — and **never a periodic type-1 frame /
   block 5 or 6**. Unchanged after MENU, and after a shutter half-press (S1) **and** full-press (S2):
   still zero periodic frames. Evidence: `-trace a5k_lens_*`, counting `a5k_lens_block` / `a5k_lens_frame`.
   The protocol is request→reply, and av-cam's RX poll (`43A4A730`) runs only right after it sends a
   request, so an unsolicited pushed 5/6 frame would never be read. Synthesising periodic frames on a
   timer (the §11.5-step-2 "harder half") is therefore **dead in the current emulator** — av-cam has to
   initiate, and it only does so on live-view VD.

2. **The OSD "F--" is driven by the AE path, not by any startup block.** The stock shooting OSD renders
   fine (panel console 0): aperture scale `F1.4 2.8 5.6 11 22` with a positioned marker, and the big
   `F--` placeholder at bottom-left. av-cam exposes the F-number as a rational in a "camInfo" struct
   (dump `sub_435EB4A8`): **`fNumber` valid@+12, numerator@+16, denominator@+20** (also
   `maxApertureValue` num@+28/den@+32, `focalLength` further on). Param 267 = `100 * num/den` (F×100,
   matches the device 180 = f/1.8). camInfo.fNumber is filled by the exposure/AE float code
   (`sub_4372A38C` and the YC "CalcParam/SetParam" jobs, strings `DCV::job_YC_*`, `fno:%d`), which is
   the live-view pipeline. That pipeline does not run (README: live view stops before YC state 2), so
   fNumber stays invalid and the OSD stays `F--`.

3. **Populating the startup capability table does not help.** `sub_436B7E5C` (block-7 power-class path)
   reads a u16 at `word_table[64]` (class 1) / `[65]` (class 2), `word_table` base `0x43CC5B30`
   (`[64]` = `0x43CC5BB0`), and computes `100 * value`. That table is fed by **block 8** (202 B,
   requested at startup, handler `sub_436B74B4` → an indirect parse hook, then the id-59 capability
   check). Mapping experiment: filling block 8 with `o[k]=k` made the previously-all-zero table read
   `table[64]=0x96=150`, i.e. `table[64] <- block8[150]` (the value equals the offset); detection still
   reached BL state `0x05` (no `LENS_TYPE_UNKNOWN` / `LENS FIRMERR`). But with the table thus populated
   the OSD **still showed `F--`**, and param 267 was still unreachable. So block 8 feeds a startup
   capability word (likely a max-aperture or focal capability used for scale limits [?]), **not** the
   live F-number. Block 7 does not carry the live aperture either (consistent with §9.5). `xp`:
   `0x83D707C8[0] = 0x05` (BL state), `0x83CC5B30` region all-zero on the stock (zero-filled) block 8.

4. **SDCom read channel is blocked independently.** From a guest root shell (native root, `/dev/uipc`
   present), `SDPROBE 2 0x1002` returns `connect(0x1540432,2) = -5` for every component id, at both high
   and low host load. `-5` is `SDComIPC::connect`'s `pthread_cond_timedwait == ETIMEDOUT` (110): the
   connect message reaches the daemon queue but **no reply arrives in 5 s**. The ScalarDaemon only
   accepts when the Scalar engine is active (state 5/9; NOTES §10.7), which the emulator never reaches
   (live-view incomplete). So even if an aperture existed, verifying it over SDCom 0x1002 in the
   emulator is not currently possible. (The real device accepts because ILCE Remote has the engine
   live.) Verification route for the emulator is therefore the **panel OSD screendump**, not SDCom.

**Consequence / revised plan.** The §11.5 lens-model spec is correct about the *source* (periodic 5/6)
but its end-to-end payoff is gated on the **live-view / AE pipeline** (the ⬜ Sensor/ISP live-view item),
not on the lens bytes. Order of work to actually show an F in QEMU:
  (a) model enough of the live-view VD so av-cam runs its AE loop and *requests* periodic lens frames
      (or reaches Scalar-active so SDCom 0x1002 answers);
  (b) only then decode the aperture byte inside block 5/6 **dynamically** (vary the byte, watch
      camInfo.fNumber +16/+20 or param 267) — static decode is blocked by av-cam's accessor-indirected
      lens-info store (`sub_436B6F74` writes fields by index, not at a fixed struct offset);
  (c) drive it from the `a5k-lens` `aperture` / `focal-length` properties added in `cxd90014_lens.c`
      (declared now as the stable knob; inert until (a)/(b)).

**Confidence:** findings 1–4 **high** (direct trace / `xp` / screendump / source). "block8[150] →
cap-table[64]" **high** (pattern-equals-offset). Semantics of that capability word (aperture vs focal)
**low/`[?]`**. "a lens-model-only change cannot show F today" **high**.

> **Correction (2026-11-16, `docs/RE_LIVEVIEW.md`):** finding #2 above ("that [AE] pipeline does not run")
> and the "Consequence / revised plan" are based on the then-current, now-stale claim "live view stops before
> YC state 2". On the current emulator HEAD (commit `9d0cad7`, SYSV master V clock) **YC state 2 is reached
> and the AE float path runs** (`437737D4` ~3/s, `4372A38C` 128–180 hits, VPH ch51 programmed), measured with
> both `lens=none` and `lens=ekit`. The OSD is **still "F--"** with the pipeline running, so the F-number is
> **not** gated on YC state 2. The remaining gap is specifically the **periodic lens-aperture request
> trigger** — av-cam issues no periodic (type-1 / block 5/6) lens request even with live-view VD active, so
> the lens link is idle after startup and `camInfo.fNumber` never gets a current av. See `RE_LIVEVIEW.md` §3–§4
> for the precise location and the follow-up spec.

### 11.8 Decoded trigger for the periodic 5/6 request (2026-11-16): av-cam's AE control/apply job, not a lens byte

Follow-up to §11.7 / `RE_LIVEVIEW.md` §3–§4 (the open "what makes av-cam issue a periodic type-1 request"
question). Method: IDA 9.4 / Hex-Rays on `lens2/lens_avcam.i64` (the av-cam process image) and a dynamic
`a5ktrace` run of the unmodified firmware (`lens=ekit`, pristine COW NAND, no SD) with per-function probes at
the av-cam VAs below. All facts carry evidence; `[?]` = unproven.

**The full request chain (static, named).** The periodic lens-aperture (Info1/Info2) request is built here:

```
<AE control/apply job>  sub_43853DA8
  -> sub_4385EC74 -> sub_4385EC78 -> sub_4380C5A8   (AE result -> lens-aperture driver)
       -> sub_4380BD34 / sub_436CFB1C -> sub_436CFB70 / sub_43A4C60E
            -> sub_436B5188  (enqueue one type-1 lens request; first byte = block id 5/6)
```

* `sub_436CFB70` picks one of two request templates `off_436CFCE8` / `off_436CFCEC` (selected by
  `sub_436BB21C` = 1.5 Mbaud/class check and `sub_436BB3B4`); the reply blocks 5/6 are parsed by
  `sub_436B7110` / `sub_436B7128` → `sub_436B6F74` (the accessor-indexed lens-info store of §11.7 step (b)).
  So this is the Info1/Info2 periodic path. The exact template id byte in `off_436CFCE8/EC` is `[?]` (not
  dumped), but the parse consumers confirm blocks 5/6.
* `sub_4380C5A8` is the per-frame AE→lens coupler. Its own early-return gate is
  `if (byte[0x43E44162]==0 && byte[0x43E44177]==0) return cb(...);` — i.e. it needs one AE-state flag set to
  proceed to the request (`[0x43E44162]` = AE-ctx+0, `[0x43E44177]` = AE-ctx+0x15; base ptr at `0x43EDC008`).

**The real gate: AE job-state == 1.** `sub_43853DA8` runs only from the per-VD AE job-slot handlers
`sub_43A89734 / sub_43A89824 / sub_43A898A6`, each of which dispatches on the AE job-context state byte
`*(ctx+35)` (reader `sub_43A6EAFC`) — **case 1 → `sub_43853DA8`**. Those handlers are registered as AE
job-slot callbacks by the big initializer `sub_437CB26C` (`sub_43A6C296(slot, fn)` ×hundreds; the three
handlers live at data slots `0x43EE10E0 / 0x43EDECD4 / 0x43EE0754`). They are invoked when the matching AE
job is scheduled by the AE sequencer.

**Dynamic measurement (`lens=ekit` boot, whole run).** Probe hit counts (`a5ktrace fn=`):

| probe (av-cam VA) | meaning | hits |
|---|---|---|
| `4372A38C` | AE float / metering | **360** |
| `437737D4` | AE job (metering) | **195** |
| `436B5188` | lens request enqueue | 10 — **only** block ids 01 07 08 09 0a 0b 0d 10 16 (startup); **no 05/06** |
| `43A6EAFC` | AE-state getter | 254 |
| `4380C5A8` | AE→lens-aperture driver | **0** |
| `43853DA8` | AE control/apply job | **0** |
| `43A89734 / 43A89824 / 43A898A6` | AE job-slot handlers (→43853DA8) | **0** |

HMP `xp` at the same time: `[0x83E44162]=0x01` (AE-ctx+0 flag already **set** — so `sub_4380C5A8`'s own gate
would pass), `[0x83E44177]=0x00`, BL state `[0x83D707C8]=0x07` (lens fully recognised, past the 0x05 of §10.2,
all 8 startup blocks answered).

**Conclusion (high confidence).** The AE metering pipeline runs hard and the lens is fully attached, yet
`sub_43A89734/824/8A6`, `sub_43853DA8` and `sub_4380C5A8` are **never executed**: av-cam never schedules the
AE **control/apply** job in the emulator's current live-view state, so the type-1 block 5/6 request is never
built. Therefore:

* **Candidate (a) REFUTED** — no lens startup-block byte gates this. All startup blocks complete, the lens is
  recognised (BL 0x07), `sub_4380C5A8`'s own flag is already 0x01; the block is entirely inside av-cam's AE
  scheduling, independent of the lens reply bytes. (Consistent with §11.7 #3: populating block 8 did nothing.)
* **Candidate (b) REFINED** — not S1/S2 half/full-press (prior agent + this run show zero periodic frames on
  key events). The trigger is the AE **job-state machine** reaching the control/apply state (`AE-ctx[35]==1`),
  driven by the AE sequencer scheduling `sub_43A89734/824/8A6` — a **live-view / AE-sequencer layer** that the
  emulator does not yet reach.

**Aperture-byte decode (§11.5 step 2 / §11.7 step (b)) stays BLOCKED** — it can only be done dynamically
(vary a 5/6 reply byte, watch `camInfo.fNumber` +16/+20 or param 267), which requires the request to actually
fire. Static decode is blocked by the accessor-indexed store `sub_436B6F74`. So this stops at the spec, per
the fidelity rule: no faithful lens-model byte unblocks it, and forcing an unrequested 5/6 reply (or poking
`camInfo`) would fabricate data the firmware never asked for.

**Next layer to model (for a live-view/AE agent, not a lens agent).** Make av-cam's AE sequencer schedule the
control/apply job — i.e. drive `AE-ctx[35]` to 1 so `sub_43A89734/824/8A6 → sub_43853DA8 → sub_4380C5A8`
runs. Find the AE job-scheduler that posts those job slots (the VD-phase that enqueues the handler at
`0x43EE10E0 / 0x43EDECD4 / 0x43EE0754`) and what makes `AE-ctx[35]` transition to 1 (the AE sequencer state;
writer is pointer-indirected, not a fixed-offset STR in the getter's neighbourhood). Only then decode the
aperture byte dynamically and drive it from the `a5k-lens` `aperture` property.

**Confidence.** Chain (functions/addresses) **high** (IDA + trace). "5/6 never enqueued while AE runs" **high**
(direct probe counts). "AE-ctx[35]==1 gates the apply job" **high** (3 dispatchers, identical case 1). Template
`off_436CFCE8/EC` id byte **`[?]`** (not dumped; parse consumers imply 5/6). Exact AE-sequencer event that
sets `AE-ctx[35]=1` **not decoded** (the deeper layer).

### 11.9 AE-ctx[35] decoded with Ghidra: it is a generic job sub-state; the real gate is the AE-control job never being POSTED (2026-11-16)

Follow-up to §11.8. Method: **Ghidra 12.2 headless + PyGhidra** (oeindev) on `av-cam.bin` (raw image,
image base `0x43541000`, `ARM:LE:32:v7`), decompiling and disassembling the exact VAs below. All facts are
backed by the decompiled C / raw disasm; `[?]` = unproven, `[H]` = hypothesis.

**1. `AE-ctx[35]` is `job+0x23`, a generic job sub-state — not an AE-specific flag.**
`sub_43A6EAFC(job) = *(u8*)(job+0x23)` (confirmed: `return *(undefined1 *)(param_1 + 0x23);`). The matching
initialiser `sub_43A6CB24(job)` does `*(u8*)(job+0x23) = 0` (job creation resets the sub-state to 0). The
getter `43A6EAFC` is read by **~30 handlers** across the `0x43A8xxxx` module-handler cluster **and** by the
live-view EE-stage core `FUN_43546E40` — i.e. `+0x23` is the common "step" byte of av-cam's **job state
machine framework**, reused by every sequence module, not a dedicated AE register.

**2. The three handlers are three independent job state machines (raw disasm).**
```
43A89734: push{r4,lr}; mov r4,r0; bl 43A6EAFC; cmp r0,#0xb; bhi ...; tbh [pc,r0]   (states 0..0xb)
43A89824: push{r4,lr}; mov r4,r0; bl 43A6EAFC; cmp r0,#0x9; bhi ...; tbb [pc,r0]   (states 0..9)
43A898A6: push{r4,lr}; mov r4,r0; bl 43A6EAFC; cmp r0,#0xa; bhi ...; tbh [pc,r0]   (states 0..0xa)
```
Each takes the job in r0, reads `job+0x23`, and jump-tables on it. `43A898A6` **case 1** is the apply path:
`ctx=*(job)` (`sub_43A59C60(job)=*job`), `43A59F86(ctx,0xc5)`, … `sub_4385EC74(ctx)`,
`43893F98(4,0x2a, <fNo num/den from ctx+0x11A6/0x11AE/0x11BA>)`. `43A898A6` **case 0** posts an async
request (`sub_438948B4(obj,template,job,0)`) and returns 0 → the job self-advances to state 1 on that
request's completion, then case 1 (apply) runs. `sub_4385EC74` inlines `sub_4380C5A8` (the AE→lens-aperture
coupler) whose gate is `*(char*)g==0 && *(char*)(g+0x15)==0` — i.e. the `[0x43E44162]`/`[0x43E44177]` flags
of §11.8. **Confirmed the §11.8 chain end-to-end.**

**3. The apply job consumes PRE-COMPUTED exposure — it does NOT need real pixels.** Case 1 reads the f-number
rational straight out of `ctx+0x11A6/0x11AE/0x11BA`, which the metering copier `sub_437737D4` (runs ×195,
§11.8) already fills from the AE-float result into the engine context. So **the apply stage itself is not the
"deep, needs a real ISP image" case** — the exposure numbers already exist when the apply job would run.

**4. Registration + framework (how the handlers hang off a module).** `sub_437CB26C` is a flat initialiser
that calls `sub_43A6C296(slot, fn)` a few hundred times; `43A6C296 → 437C3AAC` writes `slot[0]=<type const>,
slot[1]=0, slot[2]=fn` (20-byte slot, **fn at slot+8**). Resolving its literal pool: the three handlers land
in **one module object** (call it module-C, runtime base `0x43DC6AE0`, held at pointer-slot `0x43EE1314`),
at slot indices **8 / 10 / 11** (`base+0xA0`=734, `+0xC8`=824, `+0xDC`=8A6). A job is dispatched by looking
the module up in a **17-entry registry** (`sub_437C7038(idx<0x11) = *(regbase + idx*4)`) and calling
`module->slot[event].fn(job)`; jobs are posted onto a task queue by `sub_437C76EC/437C7754/437C74D0`
(each: template copy `43A6C240`, `43A6CB24(job)` state=0, then `438948B4(...,job,0)`). `sub_43893DD4`
(called with 0/4/5/8/9/10 inside the per-VD frame routine `sub_4376C1E4`) is a **timing profiler**, NOT the
dispatcher — do not mistake those ids for event dispatch.

**5. The actual gap: the AE-control job is never POSTED.** Prior trace: handlers 734/824/8A6 get **0 hits**
while metering runs hard. Since a job's `+0x23` is 0 at creation and only advances once the handler runs,
`AE-ctx[35]` never even reaches state 0's code — meaning **no AE-control job is enqueued at all**. The poster
is the engine's live-view **AE/EE sequence**: the EE-stage core `sub_43546E40` is itself a `job+0x23` state
machine (`EE stage = *(u8*)(img+0x23)`, §`RE_LIVEVIEW_PIPELINE.md`) that posts the next-stage jobs through the
same framework. It reaches the metering stage (so `437737D4` runs) but **does not advance to the control/apply
stage** in the emulator, so the module-C job is never posted.

**Classification — NOT the "fabricate metering" deep case, but gated behind the live-view image/EE sequence
(lean: STOP AT SPEC).** The trigger for `AE-ctx[35]=1` is not a standalone device IRQ/register. It is:
*engine live-view AE/EE sequence advances to its control stage → posts the module-C AE-control job (state 0)
→ that job's state-0 async request completes → `job+0x23` becomes 1 → apply (case 1) runs → camInfo.fNumber /
param 267 populated.* The one unresolved link is **which EE-sequence predicate stalls between the metering
stage (reached) and the control-job post**. Statically that predicate lives inside the giant `sub_43546E40`
and its HS_Sequence job graph (`4376FB3C`, list `0x43D88F44`), coupled to the live-view image dataflow that
the emulator does not yet generate (the synthesized-ISP "deep" item of `RE_LIVEVIEW.md` §5 /
`RE_SENSOR_ISP.md` §4.b). **No small faithful device change drives `AE-ctx[35]=1` in isolation**, and
force-posting the job or poking `job+0x23` would fabricate sequencer state the firmware never produced
(fidelity violation) — so **no emulator change is made this pass.**

**Precise next layer (dynamic, for a live-view/engine agent — needs the a5ktrace harness, ~130 s boot).**
Probe `sub_43546E40` entry and log `*(u8*)(param_1+0x23)` (the EE stage) each VD, plus the stage-case
predicates that `return 5` (abort/stay). Find the exact stage where the sequence stalls after metering. If
that stalled predicate is a bare **completion/flag** an engine/ISP device fails to raise (the OPD/IDT
"IRQ-but-no-data" pattern), it is **tractable** — raise that one signal and the control job posts. If it reads
**image content / statistics**, it is **deep** — it needs the synthesized ISP front-end, and the F-number
remains correctly blocked until then. The `a5k-lens aperture` prop and the aperture-byte decode of §11.5/11.7
stay blocked behind this regardless.

**Confidence.** `+0x23` = generic job sub-state, getter/setter, 3 independent state machines, apply consumes
pre-computed exposure, module-C slot mapping, 17-entry registry, `43893DD4`=profiler: **all high** (direct
Ghidra decompile / raw disasm / pool resolution, this pass). "AE-control job never posted (not merely
state-stuck)" **high** (0 hits + `+0x23`-init-0 semantics). "Gate lives in the live-view EE sequence advance"
**high** (43546E40 is the poster layer and is an `img+0x23` machine). "Deep vs tractable at the EE-predicate
level" **undecided** — that is exactly what the dynamic next step settles.

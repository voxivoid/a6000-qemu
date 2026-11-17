# Audio (microphone, speaker, operation beep): static + dynamic RE (fw 1.10)

Scope: what the a5000 firmware uses for audio, who owns it, what happens in the emulator today, and the
smallest model that keeps audio from ever blocking (plus an optional path to host sound).
Addresses are `av-cam.bin` VAs (VA 0x43541000 = PA 0x83541000) unless a module is named. `[?]` = not proven.
Method: Hex-Rays on a private copy of the av-cam IDB (`C:\a5000\emu\audio\`), libObj.so disassembly, string
cross-references, and three boots of the unmodified firmware with the `a5ktrace` plugin plus a scratch
memory-access plugin. Every boot used a pristine NAND clone.

---------------------------------------------------------------------------------------------------
## 0. TL;DR

1. **There is no Linux audio driver.** The kernel has no ALSA/OSS code, and no kmod in `/usr/kmod` touches the
   audio block. Android's `libaudio.so` is `scalar::AudioManagerHardwareDiadem`. It only forwards parameters
   (`microphone-enable`, `microphone-wind-noise-reduction`) to the engine over SDCom and has no PCM path.
   **All audio is owned by av-cam** (the µITRON engine):
   * "ADF" = audio data flow: paths, system sound, pins. Code ≈ 0x43555000–0x4358xxxx.
   * "AVIO" = audio/video I/O manager: 0x43592000–0x4359Bxxx.
   * "AIP"/"AudioIp" = the hardware driver: 0x438A5000–0x438BD000.
2. **The audio hardware is an on-chip block "AudioIp" at PA 0xF3420000–0xF3423FFF.** No IRQ was found for it:
   no `ena_int` call in the audio code. Its periodic handler runs from the AIP task at frame rate and reads
   the DMA position registers. So nothing waits on an audio interrupt.
3. **External codecs are compiled in but disabled on this model.**
   * The "ABB" external-codec SIO path is gated by `sub_438B87DC()`, a stub that returns 0.
   * "HIBIKI" (an SIO-controlled codec class in AVIO) has stub getters that return 0 for channel, CS port and CS
     bit.
   * The AVIO debug help lists the routing modes `"Mode [0:Off 1:Outside(ABB) 2:Inside(PiroshkiOnly)]"`. The
     a5000 therefore uses the **internal ("Piroshki") converters** [?: inferred from the stubs, not proven
     electrically].
   * In the emulator, no audio SIO transfer was ever issued (the probe on the AudioIp SIO sender `438B86B0`
     never fired).
4. **In the emulator today audio initialises fully and nothing stalls.** The catch-all register file under
   0xF3420000 reads back what was written. The av-cam log shows `[ADF][INIT]AUDIOIP INIT COMP` (probe
   `435619B8` hit), `[ADF][SYSND] PS ON`, and 16 system-sound table and volume registrations. The only
   polling loop in the audio bring-up is the audio clock generator 0xF2A01000, which is already modelled
   (`clkgen`).
5. **No beep request reaches av-cam in the tested UI flows.**
   * Arrow/OK keys in the first-boot dialogs call `HighlightManager::playDirectionBeep`, which is an empty
     function in this build (libObj `0x37C3D4`: `push; add r7; pop`).
   * `AppWidgetBase::beepPlay`, `LkmBeepIF::ModelExecute` and every `ObjSystemSound` "BeepPlay" command were
     never called.
   * Real beeps (AF lock, self-timer, caution, rec start/stop, shutter sound) need shutter, half-press and
     recording paths that are not wired yet.
6. **Minimal plan:** keep the register-file behaviour (it already works). Later, add a `cxd90014-audioip`
   model with three ring-buffer DMA channels that advance their position registers in real time, and send the
   playback channel to a QEMU `audiodev`. Real movie audio additionally needs the **SA DSP** codecs
   (`sa_aaclcenc_nr.bin`, `sa_dd51enc_nr.bin`, …), which are not emulated. That is a separate blocker.

---------------------------------------------------------------------------------------------------
## 1. Hardware

### 1.1 AudioIp block, PA 0xF3420000 (16 KiB)
Evidence: every literal or `MOV32` in the AIP driver (`av-cam.mmio.tsv`: pages 0xF3420000/1000/2000/3000,
104 references). The functions are single-register accessors at 0x438B5E00–0x438B7640 and
0x438BBF00–0x438BC740. The block name comes from the debug strings `[AudioIp]…` / `13AudioIpDfsMgr` /
`10AudioIpLog`.

| Window | Registers (accessor) | Meaning (from the access pattern) | St |
|---|---|---|---|
| +0x0000–0x0010 | `438BC1A0/AC/B8` write +0x00, +0x04, then +0x08/+0x0C/+0x10 together | global config / address triple [?] | H |
| +0x0400 | bit0 R/W (`438BC160/170`) | enable/run [?] | H |
| +0x0410 | R: bit16 (`438BC180`), bit24 (`438BC190`) | status flags; **never read at boot** in the emulator | H |
| +0x0800, +0x0810 | written `1` (`438B60B8`, `438B60AC`), last step of init `438B1E78` | start / reset release [?] | H |
| +0x0C00 | `=1` (`438B7500`), bit0 (`438BC690`) | input-path enable [?] | H |
| +0x0C10 / +0x0C14 | bits1:0, bit8, bits26:16 (11-bit) (`438B6xxx` RMW) | per-channel gain/volume fields [?] | H |
| +0x0C20–0x0C4C | written once at init | path config [?] | H |
| +0x0C50 | 4-bit field, init writes `6` (`438B75E4(6)`) | mode/clock select [?] | H |
| **+0x1000 + 0x400·n, n = 0..2** | generic accessors `438B6030` (write) / `438B6040` (read): `addr = (0x3CD084 + n) << 10` | **three ring-buffer DMA channels** (§1.2) | H (structure S from code) |
| +0x1C00–0x1C60 | R: +0x1C00/04/0C/18 (`438BC514…`), +0x1C14, +0x1C1C&3, +0x1C34&7; W: +0x1C2C bit0, +0x1C30 byte, +0x1C60 bits0/16 | level meters / status (recording-level display) [?] | H |
| +0x2000–0x2444 | DSP parameters written in bulk (`438B6590`, `438B677C`). Reads at +0x2060..+0x206C and +0x2360 (`438B60C4`) are only compared, never polled | processing chain "AUDIO_IP_CONFIG_TYPE_VGS/PR/MF/BANDI/CONS", mixer [?] | H |
| +0x2400–0x2444 | `438B75F0`: +0x404 &=~0x100; +0x408=0x00020001; +0x440=0x09000002; +0x444=0xF30C000C; +0x400 \|=1; +0x404 \|=0x100 | serial-interface / format setup [?] | H |
| +0x2C00–0x2C08 | bits 0,1,4 (+0x2C00), mask 0xFFFFECCC (+0x2C04), bit0 (+0x2C08) | switches [?] | H |
| +0x3000–0x305C | `438B5F50`: 21 words then +0x3000 = byte. Init values 0x18B7, 0xCA4A, 0x21B6, 0x052A, … (boot log) | IIR filter coefficients (low-cut / wind NR) [?] | H |
| +0x3400 | 16 bit-fields packed from a struct (`438BC600`) | filter / ALC control [?] | H |
| +0x3800–0x383C | 16 × 14-bit (`438BC354…4F8`), init 0x3F85, 0x39F2, … | coefficient table [?] | H |
| +0x3900–0x3908 | bits 0, 8, 4, 20, 14:12, 24 (`438BC23C…314`) | control [?] | H |
| +0x3C00–0x3C28 | single-bit switches (`438B5EE4…5F44`, `438BC1CC…`) | path on/off [?] | H |

The full boot-time write sequence is in `docs/runtime/io_after_boot.txt` (lines 557–647). It matches the
emulator run exactly.

### 1.2 The three DMA channels (+0x1000 / +0x1400 / +0x1800)
They are reached only through veneers (offset in r1):

| off | access | veneer → users | meaning [?] |
|---|---|---|---|
| +0x00 / +0x04 | R/W pair | `43AB23EA` (read both), `43AB23A4/A8` (write both) | ring **base** / **size** |
| +0x0C | R/W | `43AB239C` (write), `43AB23DC` (read) | software **pointer** (host write pointer for playback). `438B8804` rejects values that are not 64-byte aligned or outside [base, base+size] |
| +0x14 | R | `43AB23D6` | **hardware position** (current DMA address) |
| +0x18 | W | `43AB2394`, e.g. `0xC0000000` | flags |
| +0x1C, +0x3C | R | `43AB23D0`, `43AB23CA` | status / counters |
| +0x2C, +0x38 | W `1` | `43AB23C2` / `43AB2384` | start / kick |
| +0x30 | W | `43AB238C`: `12` = stop (`438B8D58`, all 3 channels), `124` = run (`438B8F30`, ch1) | mode / enable |

* `438B9460`: copies ch0 base/size to ch1, and ch0 +0x0C to ch1. So ch0 and ch1 share one buffer [?].
* `438B9284`: programs ch0.
* `438B8804`: moves the ch2 write pointer. So ch2 = playback (system sound, movie playback) [?] and ch0/ch1 =
  capture plus monitor [?].
* **Periodic poll:** the AIP task dispatch table (`43AB1732`, entry `43AB077E`) calls `438B3984` →
  `438B1950` at frame rate (≈60 calls/s wall-clock in the emulator, consistent with the 60 Hz SYSV VD [?]).
  `438B1950` reads `+0x14` of all three channels and posts the positions to the ADF (queue 0x69000C). In
  the emulator `0xF3421014` is read about 12 000 times per few minutes and always returns 0. Reader found
  with the memory plugin: `pc=438B6046`.
* Memory for the rings comes from the engine memory map table `dword_43B28F20` (keys 0x690000, 0x690200,
  0x690218, 0x69021D, 0x690228 in `438B1E78`).
* Bus arbitration is set by `AicSetDicArb`: `sub_43A97114(80, …)` = read port, `(108, …)` = write port
  (`438B251C`).

### 1.3 Clocks and reset
* `438BCCAC` (the probe at `438BCCD4` hit at boot, thread `liro-kliro_1`):
  * `0xF29000A4 = 0x100000` (clkrst3).
  * clkgen **0xF2A01000**: `+0x10 = 0`, `+0x00 = (v & 0xFEFFFFC0) | 0x1B`, `+0x160 = 3`, poll `+0x04` bit0,
    `+0x30 = 1`, poll `+0x30 == 0`.

  This is the only busy-wait in the audio bring-up. It is already satisfied by the existing `clkgen`
  regfile model (V). The function sits in the AudioIp module, so this is probably the audio master-clock
  PLL [?].
* `438B251C` (AudioIp HW init):
  * `PblSgSyncSel(19, {1, 2})`: sync-signal-generator selection, ties audio to the system frame timing [?].
  * `PblClkCtl(20, {0x000C0004, 0x00300010})` (`43AB4F98` → clock-register offset 320 of the Pbl clock
    table); `PblClkCtlOff(20)` in `438B23F0`.
* Sample rates offered by an AVIO audio debug command (HDMI-input section of the help text):
  `freq [0:32kHz, 1:44.1kHz, 2:48kHz]` (string at av-cam file offset 0x64E9D5).
* Recording volume range: `0-63`. Speaker volume: `0-15`. Speaker mute masks: L/R/both (`asspmute`).

### 1.4 External devices: present in code, absent on the a5000
| Item | Evidence | a5000 |
|---|---|---|
| **ABB** (external audio codec on SIO) | `438B7714` / `438B7780`: SIO request byte0 = 1, mode 3 or 2, **4 MHz**, 5-byte frames, tx `0x43C10EC5` = `81 00 00 00 00`, tx `0x43C10ECA` = `80 80 00 00 00`. `438B77E8`: generic write. `438B8AEC`: 1 263 158 Hz, 41/49-byte frames. All go through `438B86B0` → SIO server 0x740017. **Channel/CS come from `byte_43E734CE/CF/D0`**. | every caller first checks `sub_438B87DC()`, which **returns 0** (stub) → the ABB state machine logs and stops. `438B86B0` was never executed in the emulator |
| **HIBIKI / HIBIKI2** (AVIO "acl_hibiki", `6Hibiki` class with Input/Output/Power queues) | `43596F40` prints "SIO ch, CS port, CS bit". Its getters `4359424C/58/64` and `43594C2C` are `return 0` stubs. The register dumps in `43598614` print constant zeros | not fitted |
| HDMI audio | hdmi.ko maps "hdmiaudi" 0xF2A20000 (0x2C) (RE_DISPLAY_GPU §6). av-cam `ProcHdmiAudioMonitor` / `ProcPinCtrlHdmiAudio` | output only when HDMI is attached. Out of scope; regfile is enough |
| Speaker amplifier / mute GPIO | AVIO `SetSpMute` (`43593860`) goes through an object vtable, not a fixed pin; not resolved | **open** |

**SIO channel 1 traffic seen in the emulator is not audio.** About 6 000 transfers carry the repeating frames
`00814080 …` / `6704012B …`. The AudioIp SIO sender was never reached, so the owner is another driver [?].

---------------------------------------------------------------------------------------------------
## 2. Software owners and paths

### 2.1 Operation beep (system sound)
```
UI (im.elf / libObj.so)
  AppWidgetBase::beepPlay(id) @0x345164 ─▶ LkmBeepIF helper @0x3865A4 ─▶ ObjSystemSound (libObj, created at boot)
      reads /usr/share/avsystemsound/beeplist.txt + *.bin  (AvbbcFileReader, "FileEnd" seen at boot)
      ExecCmdRsrcAcquired[SysConstraint]BeepPlay[AaLifo]  (resource arbitration, then AVBBC command)
  ──AVBBC queue 0x61003C──▶ av-cam ADF
      AdfCmdSystemSoundCtrl (vtable 0x43E79888) ─▶ AdfSystemSoundRequest:
          REQ BIZ  @0x43566AB4  (APP-type beeps, data in the SDS system-sound table)
          REQ PIN  @0x43566938  (PIN-type beeps: shutter KA/SHA/KASHA, focus lock)
      ─▶ AudioIp playback ring (ch2 [?]) ─▶ internal DAC ─▶ speaker
```
* Boot (seen in the emulator):
  * libObj: `ObjSystemSound_Create`, `InitParam`, `onStarting`, `AvbbcFileReader FileEnd`.
  * av-cam: `SET TABLE` ×16 (`43566F98`), `SET_VOLUME` ×16 (`43567310`), `PS ON` (`43565ED8`).
  * Entering the shooting screen: `ExecCmdGetInfPin`.
* `beeplist.txt` fields: `BeepID_Name, ID, MemoryType (APP|PIN), FileName, RsrcAcsType (AA_RT|AA_FIFO),
  Priority, KindID`.
  * 48 IDs: 0x00 STARTUP … 0x2F RING_CCW. Many have no file (`None`).
  * PIN beeps: 0x01 KA, 0x02 SHA, 0x03 KASHA, 0x04 FOCUSLOCK.
* Sample files: 640–9 280 bytes, all multiples of 64. The first byte of every 64-byte block is a small value
  (0–24), which looks like a per-block header (block ADPCM or a scale byte [?]). The sample rate is unknown
  [?]. The decoding stage (AudioIp hardware, ADF software or SA) is not identified [?].
* The UI paths tested (first-boot dialogs, menus) do not beep at all: `playDirectionBeep` is empty, and
  `beepPlay` / `ModelExecute` were never reached. Beeps worth testing later are AF lock (half-press), the
  self-timer, CAUTION, and REC start/stop. They need the shutter keys and recording to work first.
* The user setting "Audio signals" is persisted by `ObjSystemSound::BackupControl` (`CallBackFuncReqSound`,
  `CallBackFuncSetVolume`), and av-cam keeps its own copy via `AbbSysndBackUpCtrl`.

### 2.2 Movie recording audio (capture)
* Internal stereo mic → internal ADC → AudioIp capture ring (ch0/1 [?]).
* `AvcEncAudioModule` (`TskAvcAudio1Thread2Instance`) then encodes on an **SA DSP**:
  * `FUNCID_CORE_SA_LOAD … codectype`.
  * ODL-loaded `/usr/sabin/sa_aaclcenc_nr.bin` (AAC-LC + NR, MP4), `sa_dd51enc_nr.bin` (Dolby AC-3,
    AVCHD) and `sa_integenc_nr.bin`.
* The AIP also drives an **"LPS"** SA program through mailboxes. `438B48D8` `LPS GetMailBox` calls
  `slp_tsk(-1)` until the SA callback wakes it. With no SA emulation that call **would hang forever**. It is
  not executed during boot or in the shooting UI.
* Wind-noise reduction and the low-cut filter go through `AdfCmdInputCtrl::SetLowCutAudioIp` → AudioIp
  +0x3000 coefficients [?].
* Android only toggles mic enable / wind NR through SDCom (`libaudio.so`).

### 2.3 Playback audio
* `AvcDecAudioModule` / `McAvcDecAudio*`: AC-3 parser and `sa_ac3dec.bin`, `sa_aaclcdec.bin`,
  `sa_bd_lpcmdec.bin`; BGM via `bgmdec_audio_*` (mp3).
* Decoded LPCM goes to `McPinAudioEditor` (copies with DMAC link lists) → ADF pin → AudioIp playback ring →
  speaker, or HDMI audio.

---------------------------------------------------------------------------------------------------
## 3. What happens in the emulator today (3 boots, unmodified firmware)

| Check | Result |
|---|---|
| Audio init | **completes**: `438B251C` (HW init) → `[ADF][INIT]AUDIOIP INIT COMP` (`435619B8` hit once) |
| Busy waits | only clkgen 0xF2A01000 (+0x04 lock, +0x30 self-clear), satisfied by the `clkgen` regfile |
| IRQs | none used by audio |
| MMIO | all AudioIp accesses land in the `io-f0` catch-all regfile (`-d unimp`: ~100 distinct registers, each written once at init, plus the periodic +0x1014 read). No guest error, no timeout message |
| DMA position | `0xF3421014` (ch0 +0x14) always reads 0. The AIP keeps reporting a static position. Harmless while nothing records or plays |
| SIO | no audio SIO transfer (ABB/HIBIKI stubbed) |
| Beep | system-sound tables and volume registered; no request issued by the tested UI flows (§2.1) |
| Movie key | with NO CARD the camera refuses before any audio start (no AudioIp start probe hit) |

---------------------------------------------------------------------------------------------------
## 4. Minimal emulation plan

### 4.1 (a) So that nothing blocks: required now = nothing new
The existing behaviour (regfile at 0xF3420000 + modelled clkgen 0xF2A01000) is already sufficient for boot,
UI, menus and the beep table setup. Keep it.

Optional hardening as a real model, `hw/audio/cxd90014_audioip.c` (`cxd90014-audioip`, PA 0xF3420000, size
0x4000, mapped over the regfile):
1. Register-file semantics for all config windows (read back the last write, reset 0). The boot log is the
   reference.
2. **DMA channels** n = 0..2 at +0x1000 + 0x400·n. On a "run" write (+0x30 = 124 with +0x2C/+0x38 = 1),
   start a timer that advances `+0x14` from `base` through `base+size` and wraps, at
   `rate × channels × bytes/sample`. On +0x30 = 12, stop and leave `+0x14` where it is.
   * For playback (ch2 [?]) never pass the host write pointer `+0x0C`.
   * For capture (ch0/1 [?]) write zeros (silence) into guest RAM as the pointer advances, so encoders see
     real-time data. The `+0x1C`/`+0x3C` semantics are open: read back the last write.
3. Level-meter registers +0x1C00… return 0 (silence). That is the correct "quiet room" reading.
4. Do **not** generate IRQs: none are consumed.
5. The real blockers for movie recording and playback are the SA DSP mailboxes (LPS `slp_tsk(-1)`, codec
   SA open). They belong to an SA HLE, not to this model.

### 4.2 (b) Optional: host sound
* Attach a QEMU `audiodev` (`-audiodev coreaudio,id=snd0 -M sony-a5000,audiodev=snd0`) to the playback
  channel. Read PCM from guest RAM between the old and new `+0x14` on each timer tick and feed
  `AUD_write`. The capture channel can use `AUD_read` from the host mic.
* What is still needed for that:
  * The **sample format** (16-bit LE stereo vs 32-bit slots [?]) and the **rate** (48 kHz expected for
    movies; the system-sound rate is unknown [?]). The 0xF3422400–0x2444 serial/format words (0x00020001,
    0x09000002, 0xF30C000C) and the +0x0C50 mode are the candidates.
  * The way to fix them: trigger a known beep (e.g. the self-timer once the shutter is wired), dump the ch2
    ring and fit the format. Or decode one `Dslr_Emnt_*.bin` file to its length in milliseconds.
* Default must stay silent: no audiodev means the timer still runs and data is discarded (stock behaviour,
  AI_WORKFLOW §1.6).

---------------------------------------------------------------------------------------------------
## 5. Open questions
1. Which DMA channel is playback and which is capture? The ch0/ch1 shared buffer suggests capture plus
   monitor, but this is unproven.
2. PCM format and rate in the rings; meaning of 0xF3422440/0x2444 and +0x0C50.
3. Speaker amplifier enable / mute GPIO: AVIO `SetSpMute` goes through a vtable that is not resolved. Board
   tables (SSITEM) are the next place to look.
4. Format of `/usr/share/avsystemsound/*.bin` (64-byte blocks with a header byte) and where it is decoded.
5. Owner of the SIO channel 1 traffic seen in the emulator (not audio, but undocumented).
6. Whether any UI flow other than AF lock, self-timer, caution or recording issues a beep on the a5000. The
   menu and highlight beeps are compiled out (`playDirectionBeep` empty).
7. The LPS SA program: when it is opened, and what its mailbox protocol is. It must be HLE'd before movie
   audio can work.

## 6. Reproduction notes
* Probes (a5ktrace):
  * av-cam: `435619B8` (AUDIOIP INIT COMP), `43565ED8` (SYSND PS ON), `43566F98` (SET TABLE), `43567310`
    (SET_VOLUME), `43566AB4` / `43566938` (REQ BIZ / PIN), `438B86B0` (ABB SIO send), `438B3984`
    (periodic).
  * libObj, with `libobj=1` and a never-matching `pc=sys_ioctl,r0=0xdeadbeef` to trigger base discovery
    (base was 0x05909000): `+0x2462B0` ObjSystemSound_Create, `+0x345164` beepPlay, `+0x38656C`
    ModelExecute, `+0x37C3D4` playDirectionBeep, `+0x25B634` / `+0x25BC08` BeepPlay exec commands.
* The `0xF3421014` reader was found with a scratch QEMU plugin that registers memory callbacks and filters by
  vaddr (av-cam maps I/O VA = PA). gdb hardware watchpoints did not work reliably on this SMP machine.

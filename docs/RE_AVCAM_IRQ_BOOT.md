# av-cam interrupts, early task graph and the sub-micro link — static RE (fw 1.10)

Target: `av-cam.bin` (LIRO µITRON image, VA 0x43541000 = PA 0x83541000, Thumb-2 body).
Method: Hex-Rays on windev (`C:\a5000\emu\irq_avcam.i64`, a copy of `boot_avcam.bin.i64`), plus
static argument back-tracking at every ITRON/OSAL veneer call site, the ITRON static config tables in the image,
the kernel `vmlinux.elf` IDB, and the runtime log `docs/runtime/io_after_boot.txt`.
`[?]` = not proven. Addresses are av-cam VAs unless noted.

New artefacts:

| file | content |
|---|---|
| `re/ida/av-cam.irq.c` | Hex-Rays of every IRQ handler (depth 2), handler cores, task entries, SIO/UART/ADC drivers, sub-micro link, task 65 |
| `re/ida/av-cam.itron_calls.tsv` | 2 526 call sites of `act_tsk/set_flg/twai_flg/sig_sem/twai_sem/ena_int/osal_*` with the immediate r0/r1/r2 values |
| `re/ida/av-cam.itron_cfg.txt` | decoded static ITRON config: 113 tasks (id, entry, prio, CPU), 35 sems, 30 flags |

Veneers used below (slot table in `av-cam.boot.c`): `act_tsk 4355d364`, `tslp_tsk 4355d460`, `wup_tsk 4355d47c`,
`dly_tsk 4355d498`, `sig_sem 4355d4ec`, `twai_sem 4355d508`, `set_flg 4355d578`, `clr_flg 4355d594`, `twai_flg 4355d5b0`,
`dis_int 4355d8e0`, `ena_int 4355d8fc`, `osal_wai_sem_tmo 4355d988`, `osal_sig_sem 4355d9a4`, `osal_snd_msg 4355d9dc`,
`osal_snd_sync_msg 4355d9f8`, `osal_snd_sync_direct 4355da14`, `rcv_msg_tmo 4355da30`, `reg_msg_queue_cb 4355da4c`,
`reg_sync_msg 4355da84`. There is **no `iset_flg`**: ISRs call `set_flg`/`osal_snd_msg`/`osal_sig_sem` directly.

---

## 0. TL;DR — what to model first (priority order)

1. **Instrument first (cheap, decisive).** av-cam keeps its boot checkpoint in RAM: every `prof` helper stores
   `(line, step)` before sending it on to a logging queue: `4358c814` → `dword_43D3B028/2C` (+`byte_43D3B024..27`),
   `4358c6d4` → `dword_43D3B01C/20`, task-1 `4355deb0`, task-8 `4355df9c`. Read them at PA = VA + 0x40000000 from the
   monitor. Also note: **none of IRQ 201–205 (SIO) is enabled in the log**. `ena_int(201+ch)` is only done when a
   transfer starts (`43918294`), so **no SIO transfer was ever started**.
2. **Engine-main task 65 is stuck before any SIO traffic.** Task 65 is `"Liro Start"` @0x43764494, prio 7, CPU2.
   The IRQs it enables in program order are 83/84, 35–37, 73, 74/75, 128/133/144, and then later 67–70/72, 223, ….
   The log shows the first group enabled and the second group not. So task 65 is blocked between
   `43753194` (sub-micro link init) and `4397bb1c`. Candidates, in program order (§3.3):
   * (a) `Camera::Lens::Initialize` `436e58f8` → ODL lens-aberration load. It makes an `AsyncSend` to userland
     libOnDemandLoader (msg 0xBD017A), then `rcv_msg_tmo(…, TMO_FEVR)` `43a4f00e`. This path runs only if
     UPM_BOOT_FACTOR+8 == 1.
   * (b) DMM `AllocNewWait` `438956bc` (sync to 0x8F0139, timeout −1).
   * (c) SIO-2 driver mutex `osal_wai_sem_tmo(0x7401E1, −1)` in `436d890c`.

   This is the first thing to confirm with item 1.
3. **SYSV frame timer (0xF3402000, IRQ 98 + IRQ 99).** This is the master VD heartbeat. Task 2 "sysv" (`43655944`)
   enables 98. Task 19 "tsk_syshs" (`43655470`) enables 99. The ISRs read the frame counter and post messages to
   every registered client. Model it as a periodic IRQ 98 at the period programmed in `+0x3C` (task 65 writes 64800
   [?] ≈ 60 Hz), a frame counter at `+0x98`/`+0x94`, status at `+0x28` (bits 0..2) and `+0x2C` (signed, 21 bit).
   IRQ 99 can stay idle at first [?].
4. **Display vsync IRQ 101 / 102 (+ IRQ 79).** Task 43 (`43904274`, prio 13) enables 79/101/102. Right after that
   it blocks in `twai_flg(10, 3, TWF_ORW, −1)`. Only the ISRs set that flag: IRQ 101 sets bit 1 and IRQ 102 sets
   bit 2 (`43ac0f4e`/`43ac0ffc`). Raise 101 (and/or 102) once per frame. IRQ 79 = 0xF3440000 status `+0xA0`, W1C
   `+0xA4`.
5. **SIO controller 0xF2405000 + 0x200·ch, IRQ 201+ch** (§4). Complete each transfer: set `+0x00` bit29 "done",
   raise the IRQ, and accept a `+0x00` bit31 write as the ack. Driver callers time out after 5 s
   (`osal_wai_sem_tmo(…, 5000)` @43ac79ca), so a missing completion costs time but does not hang. ch0 = the
   **sub-micro**, ch2 = a peripheral initialised by `436d890c` (NFC per RE_INPUT [?]).
6. **Sub-micro link** (§5). SPI on SIO ch0 plus GPIO handshake lines (p0.0 level; p0.7 / p0.12 / p0.16 edge →
   IRQ 128 / 133 / 144). An emulator HLE must answer the link state machine with 8-byte headers and 128-byte frames.
   The frame layout is not decoded [?]. Until then userland gets zeros/defaults from task 48's cache.
7. **ADC 0xF2406000 (IRQ 215) / 0xF2407000 (IRQ 216)**, 12 channels each, mode 2 (continuous [?]). It is polled by
   TSK_ADC (task 55), which the 10 ms cyclic handler wakes. On scan-end the ISR copies 12 results and `osal_sig_sem`s.
   The results feed `liro_adc` (0x720018 → input.ko keys), so they must read as "no key pressed" levels [?].
8. Work-engine completions, needed once live view or capture runs: the DMA engine 0xF0102000 (IRQ 173–188), RC
   (IRQ 40–50), SPICA (81/82), SA cores (51–66), front end (35–38, 67–75, 83/84), VFX (77/78), timers ch10–15
   (163–168).
9. Kernel-side, keep low or as already modelled: 76 GPU, 146–149 = per-CPU PMU (`pmon_intr`), 255 = L2 PMU
   (`pmon_l2intr`), 206 MS, 232/233 USB vbus/id.

---

## 1. Interrupt map

Static inh table @0x43E94D04 holds 87 × {intno, attr, handler, cpu}. `main_tail` installs all 87 entries; they
are enabled later with `ena_int`. The "en" column says whether the line is in the post-boot enabled set from
`io_after_boot.txt` (Y = enabled). Register addresses are physical == virtual (static 1:1 map).

### 1.1 av-cam owned

| IRQ | CPU | handler → core | block / registers touched in ISR | device (name) | en |
|---|---|---|---|---|---|
| 35 | 2 | `436a6904` | 0xF3120000: status `+0x48`/`+0x4C`, events tagged `0x10000000` → `436a6414/65a8` (16 callbacks, spin 19) | front-end / sensor-interface unit #3 [?] | Y |
| 36 | 2 | `43a465f2` | 0xF3000000 `+0x48/+0x4C` read, ack `43a4601c/43a46014` (+0x50/+0x54 [?]), tag `0x20000000` | front-end unit #1 (SIF/"DFE" path) [?] | Y |
| 37 | 2 | `436a6940` | 0xF3100000, same layout, tag `0x40000000` | front-end unit #2 [?] | Y |
| 38 | 2 | `436ae3ec` → `436ae784` | `*(tbl[0x43B1DC64]+4) \|= 1` | front-end aux [?] | – (ena in `43a49854`) |
| 40–50 | 1 | `438cb598…438cb920` (`438cb508(n)`, n=0..10) | object `+0x14` → status reg, nibble `4·idx`, ack writes `0xF<<4·idx`; spinlock 2 | **RC engine** channels ("[RC]", init `438ca0e0`, ena in `438cbb70`) [? exact base] | Y |
| 51–56, 61–66 | 1 | `4391a4c4…4391a880` | **SA cores** 0xF34A0000 / 0xF34B0000 / 0xF34C0000 / 0xF35C0000. Per core, 3 sources: A = `+0x00/+04/+08`, B = `+0x40/+44/+48`, C = `+0x100/+104/+108`. Reads bytes, writes back to ack, then `osal_snd_msg` of an 8-byte event `{8, 0xC1, src#, regs…}` (msg id 0x930101) | SA DSP mailbox/interrupt (51-53 = SA0 A/B/C, 54-56 = SA1, 61/62/63 = SA2 A/B/C, 64/65/66 = SA3; 62 and 65 only clear) | Y except 62, 65 |
| 67–70 | 2 | `436ac9b4…436aca08` | 0xF3030104 (read, W-back) …, then call registered function pointer | front-end DMA/stat engines [?] | – |
| 72 | 2 | `436aca24` | 0xF302042C (bits 24/25 select path) | front-end [?] | – |
| 73 | 2 | `436adfec` | 0xF3040004 `&0xF`, bit1, write back `&0xF` | front-end [?] | Y |
| 74 / 75 | 2 | `4369e0f0` / `4369e131` | `+0x10` of 0xF3850000 / 0xF3950000 (read + write-back) | two of the three identical engines 0xF3850000/0xF3950000/0xF3A50000 (3rd = 0xF3A50610) [?] | Y |
| 77 / 78 | 1 | `43abda21` / `43abdae7` | 0xF3800008 / 0xF3900008 bits 0,4,8,12,16 | **VFX** effect engines ("ddl_VfxEffectTransfer") | – |
| 79 | 1 | `43904229` | 0xF34400A0 read → 0xF34400A4 write (W1C) | display/VDF hub status [?] | Y |
| 81 / 82 | 1 | `43abc22f/243` → `438da9bc` | object `+0x2C`→`+0x10` status (bits 4,12,16), blocks 0xF2804000/5000/6000 (`438da818`) | **SPICA** image accelerator 0/1 ("spica" task obj, `ModuleSpica`) | Y |
| 83 / 84 | 2 | `436ac411` / `436ac441` | 0xF3C30004 → 0xF3C30008 (8 sources); 0xF3C300F4 → 0xF3C300FC (10 sources) | front-end statistics/DMA block 0xF3C30000 [?] | Y |
| 98 | 1 | `43655a91` | SYSV 0xF3402000: `+0x98` frame counter (read twice for consistency), `+0x94`, `+0x28` bits 0..2, `+0x2C` (signed 21 bit); then `osal_snd_msg` to the sysv task | **SYSV: system vertical sync (VD)** | Y |
| 99 | 1 | `43654d9d` | same block, "[INT SYS HS]" mode/cycle change, writes `+0x2044` | **SYSV HS/mode-change** | Y |
| 101 / 102 | 1 | `43ac0f4f` / `43ac0ffd` | `set_flg(10, 1)` / `set_flg(10, 2)` (+ state updates in 0x438E/0x438F module) | **display output timing ch0/ch1** (panel vs EVF/HDMI [?]) | Y |
| 122 / 123 / 126 / 132 / 134 / 142 | 2/1 | `436fcb85…436fcc51`, `43a22c99` (null), `43a590c3` (null) | GPIO p0.1/p0.2/p0.5/p0.11/p0.13/p1.5 edges (`gpiopin_to_irq`) → event-driven task | buttons / detect lines [?] | – |
| 128 / 133 / 144 | 0/0/2 | `43a3dc1b`→`4369aa78`, `43a3de9f`→`43a3ddde`, `43a3dc1f`→`4369a970` | GPIO p0.7 / p0.12 / p0.16 edges; read level of p0.0 (`43ac774c(0,0)`) | **sub-micro handshake lines** (§5) | Y |
| 135 / 136 | 2 | `436ae355/36d` | only print "dfe_int_handler despite SSITEM_DFE=NONE" | external DFE (absent) — must never fire | – |
| 151 | 2 | `43ac7af7` → `43919bf0(1)` | **UART1 PL011** 0xF2001000: MIS `+0x40`, ICR `+0x44`=0xFF2; callbacks TX/RX/err | UART1 (§5.4) | – |
| 163–168 | 2 | `43ac7a9d…` → `4391926c(10..15)` | **CXD4115 timer ch10–15** 0xF2403000+0x100·ch: `+0x04 \|= 1` (ack) | hw timers | – |
| 173–188 | 2/1 | `43ac7639…` → `439172b0(0..15)` | **16-ch DMA** 0xF0102000 (ch 0-7) / 0xF0103000 (ch 8-15) (`43ac7698`): status `+0x00>>ch`, `+0x0C`, ack `+0x08`/`+0x10` = 1<<ch, ctl `+0x110+0x20·ch` bit0 | memory DMA (entry_pre uses ch6 regs `+0x1C0..+0x1D0`) | 180 (ch7) Y |
| 201–205 | 0/1 | `43ac7989…` → `43917f08(ch)` | **SIO ch0–4** 0xF2405000+0x200·ch (§4) | SPI/SIO | – |
| 215 / 216 | 0/1 | `43ac7535/53b` → `439168ec(0/1)` | **ADC** 0xF2406000 / 0xF2407000: `+0x0C` bit0 status, ack `\|= 0x10` | ADC unit 0/1 | Y |

### 1.2 Kernel / module owned (not in av-cam's table)

| IRQ | owner | note |
|---|---|---|
| 76 | `grm_gles.ko` `sugilite_interrupt` | GPU (RE_DISPLAY_GPU.md) |
| 125 / 129 | `nfc.ko` | GPIO p0.4 / p0.8 (RE_INPUT) |
| 146–149 | kernel `__boottime_pmon_init` @0x4033c2a8 → `request_threaded_irq(reg, pmon_intr @0x40053c1c)` | per-CPU PMU [? numbers inferred from the 4 consecutive lines] — keep low |
| 150 / 152 | pl011 (ttyAM0/1) | 151 belongs to av-cam |
| 153, 155–161 | cxd4115 timers (kernel clock, hwtimer0..3) | modelled |
| 170 / 171 / 172 | ARM7 mailbox / ldec / nand | modelled |
| 189–196 | dmac ch0–7 | |
| 197 / 198 / 199 | sircs overrun / sircs rx / i2c | |
| 206 | ms_drv | must stay low |
| 232 / 233 | fusb vbus / id | |
| 254 | `__boottime_wdt_init` (wdt_intr) | not enabled |
| 255 | `__boottime_pmon_init` +0x80 → `pmon_l2intr` @0x40053754 | L2C-310 event counter — keep low |

---

## 2. Static ITRON configuration (from the image)

* Tasks: table @0x43E90828, 113 × 156 B: `+0x00 id`, `+0x50 entry`, `+0x54 prio`, `+0x58 stksz (0x1000)`,
  `+0x60 CPU (−1 = any)`. Full list in `av-cam.itron_cfg.txt`. About 60 tasks share entry `0x43a96f35`, the
  "EvtDrvnTsk" event-driven task. Each waits on its own message buffer (`tsnd_mbf` 28-byte events,
  `43a96e3c`/`438948b4`).
* Cyclic handler 1 (@0x43E907E4): entry `43916db9`, **period 10 ms** → `wup_tsk(55)` (TSK_ADC).
* Semaphores @0x43E9654C (35 × 36 B); event flags @0x43E96B40 (30 × 32 B, ids 1–0x20); 67 MBF @0x43E95274; 2 MPL.

## 3. Start-up sequence and what each early task waits on

### 3.1 `entry_main` → task 1
`main_tail @4355d240`: `sta_cyc(1)`, 87 × `def_inh`, `act_tsk(1)` (`439b1930`).

**Task 1** `439b195a` (prio 5, CPU0). Each step is bracketed by `4355deb0(0, line, step)` checkpoints.

| step | call | does |
|---|---|---|
| 0 | `43ab3a24(1)` | |
| 1 | `43a52738` | → `4376e8fc`; `osal_wai_sem_tmo(0xC001BE, 50)` (bounded) |
| 2 | `43ab8888→438ca0e0` | RC engine init, `ena_int(40..49)`; sync msg 0x61003C (serviced by av-cam task 44) |
| 3 | `438da818` | SPICA ("spica"), 0xF2804000/5000/6000 `&= ~0x3F`, `ena_int(81, 82)` |
| 4 | `439c1818` | |
| 5 | `439172e8`, `43919d40`, `439188e4` | DMA-engine sems; UART layer; **SIO server**: registers sync queue **0x740017** (cb `439186c0`), per-channel sem 5 s |
| 6 | `43916e50` | **ADC**: queues 0x720018/0x720207, `43916ce0`: `ena_int(215/216)`, mode 2, mask 0xFFF |
| 7 | `43917280` | queue 0x7302F0 |
| 8 | `439b1e0c` | |
| 9 | `4355e56c` | |
| 10 | `act_tsk(110, 50, 51, 52, 57)` | SIO channel tasks ch0..4 (`43ac79f7`, `43ac7a0d`, `…a23`, `…a3d`, `…a53`). Each loops in `439187ec`: `rcv_msg_tmo(chan queue, −1)`, run transfer, wait completion sem (5 s) |
| 11 | `43a52746` → `act_tsk(65)` | engine main |
| 12 | `439bd77c` → `act_tsk(3)` | system init |
| 13 | `ext_tsk` | |

None of these block on hardware. Every poll in this path terminates on RAZ.

### 3.2 Task 3 (`439bd739`, prio 5, CPU1)
1. `439be65e`: the subsystem-init list, about 30 calls with `4358c814` checkpoints. It includes `438dd134` (SAN
   "Mgr"), `438e8a98`, `43acaa8e` (sync to UPM 0x924127 and to 0x61003C), **`43ac5d0c` = sub-micro module init**,
   `439fe294`, `43ab5a56`, `4358c610`, `43a974f8` (contains `osal_wai_sem_tmo(0x680147, −1)` — a mutex [?]), …,
   `43656200` (utimer alarms 0x910040).

   `43ac5d0c` registers sync queues 0x280132, 152, 153, 17E, 18B, 1D1/1D3/1D4, 208, 20C, 229–22C, 23B, 2CB, 2DF,
   2E1, 312, 3C5, 46F, 4BE, 4D3, 0x900140 and callbacks 0x2804C1, 0x9203E2, 0x9241C2 (`43910b28`).
2. `439beb1c`: `act_tsk` of 46 tasks: 2, 19, 4, 5, 6, 10–14, 17, 20–28, 30–33, 35–41, 43–46, 124–127, 48, 55, 60, 56,
   7, 42, 62, 63, 61, 54.
3. `ext_tsk`.

Early tasks that need an interrupt to make progress:

| task (entry, prio/CPU) | role | first wait | woken by |
|---|---|---|---|
| 2 `43655944` (8 / 1) | **SYSV** | `clr_pic(98)`, `ena_int(98)`, `rcv_msg_tmo(0xC2019F, −1)` | **IRQ 98** (`43655a90` posts a message) |
| 19 `43655470` (9 / 1) | **SYSHS** dispatcher | `43655018` (`ena_int(99)`), `rcv_msg_tmo(0xC20219, −1)` | IRQ 98/99 messages; it forwards "interrupt messages" to up to 20 registered clients (`436552cc`) |
| 43 `43904274` (13 / 1) | display output sequencer | `438e8990`: `ena_int(79, 101, 102)`, registers sync 0x660232/25A/295, sync_direct UPM 0x920277; `clr_flg(10)`; **`twai_flg(10, 3, OR, −1)`** | **IRQ 101 / 102** |
| 55 `43916dcd` (33) | TSK_ADC | `sta_cyc(1)`; `tslp_tsk(−1)` | cyclic 10 ms → `wup_tsk(55)`; conversion end = IRQ 215/216 → `osal_sig_sem` |
| 44 `439232bc` (24) | SA manager | `ena_int(51..56, 61, 63, 64, 66)`; `rcv_msg_tmo(0x101, −1)` | SA IRQs / messages |
| 48 `439106c5` (33) | **sub-micro protocol task** | `rcv_msg_tmo(0x2804B6, −1)` | frames from the SIO link (§5) |
| 110/50/51/52/57 | SIO ch0–4 servers | `rcv_msg_tmo(ch queue, −1)` | requests on 0x740017 |
| 8 `439b1a86` | **power-OFF task** — not a boot task | | activated by UPM_CMD_OFF 0x920143 (`4358c54a`). It runs shutdown (`436fcf00`), acts task 9, waits flag 16 (bits 1 and 2), then `dis_int` of every line |

### 3.3 Task 65 — engine main (`43764494`, prio 7, CPU2) — the likely stall
Program order. Brackets mark IRQs it enables; Y/– give the state seen after boot.

```
"Liro Start"; wr 0xF340203C = 64800 (SYSV period [?])
437694c8 [83,84 Y] → 43598b00 (osal_wai_sem_tmo 0xC001BE, 50 ms)
… 436a2964 [35,36,37 Y; 436a8bfc ?] ; 436ade34 [73 Y] ; 4369dfb0 [74,75 Y] ; 43953758 ; 4374daf4
43753194 → 4369afec → 4369cb84   sub-micro link init (§5); 6× sync 0x8F013A (mem info); link task arms 128/133/144 [Y]
osal_snd_sync_msg(0x924127, {0x920111})   UPM boot factor (upm.ko answers from cache — registered at k_upm_init)
438bc968(-2)                              GPIO 0xF2A00000 area
436e58f8  Camera::Lens::Initialize(UPM_BOOT_FACTOR*)
          ├ if bf+8 == 1: 436e5c80 AllocNewWait (sync 0x8F0139, −1)
          │               436e5d14 ODL Load: AsyncSend 0xBD017A → rcv_msg_tmo(…, −1)   ← waits for userland ODL
          └ reg_sync 0x1502FB/FF …
43ae246e → 436dd634 → 436d890c  SIO-2 device init (Backup 0xF9011A…), osal_wai_sem_tmo(0x7401E1, −1) (driver mutex)
436e40d0, 43a4eaa2, Backup_write(3, 0x30131), 43890fac … 43ad722c
4397bb1c → 43983668 [67,68,69,70,72 –]    ← NOT enabled after boot
… 4376877c (ADC init) [215] , 437da54c [223 –] , 43703034 (UPM 0x924127 again), 4374e584 (sync 0x28018b to the sub-micro task) …
```

The enabled set therefore puts task 65 at or after `43753194` and before `4397bb1c`. Because no SIO IRQ is enabled,
it has also not reached a completed SIO transfer. The prime suspects are the ODL wait in `436e5d14` and the DMM
`AllocNewWait`. Both depend on userland (libOnDemandLoader / dmm.ko) answering. Next comes `436d890c` [?].
Confirm via the checkpoint globals (§0 item 1) or by dumping the `kliro_*` thread PCs.

Further down, task 65 sends `osal_snd_sync_msg(0x28018B)` (`4398aeee`). That one is answered by the sub-micro task
48. It needs the link to be alive only if the handler waits for a fresh frame [?].

### 3.4 What userland waits for
im.elf's stage `l` returns once `entry_pre` is done (`liro.ko` completion). The userland stages then issue sync
messages to av-cam queues: 0x28xxxx (upm/input/indctr), 0x720018 (ADC), 0x740017 (nfc), the scalar/VDF queues
(ScalarDaemon/InfraAvbbc/HapiLib). Those registered by task 1 / task 3 (`43910b28`, `439188e4`, `43916e50`) answer
as soon as the init has run. The camera-engine queues registered inside task 65's chain (0x1502xx, 0x660xxx,
0x5Fxxxx …) do not exist until task 65 gets past the stall above. A request to an unregistered queue fails or blocks
on the Linux side [?].

---

## 4. SIO controller (0xF2405000 + 0x200·ch, IRQ 201+ch)
Driver: `439183b8` (request, from the 64-byte 0x740017 message), `43917fa4`/`43918058` (open: config + callback in
`dword_43E0CA74[ch]`), `439190fc` (program), `43918294` (start: `ena_int(201+ch)`, DMA/PIO of buffer
`43918170`, chip-select through GPIO `43ac7734/7740`), `4391810c` (close).

| off | use |
|---|---|
| `+0x00` | W: bit28/27/26/24 config. R: **hibyte bit1 (0x02000000) = error, bit5 (0x20000000) = done**. W `\|= 0x80000000` = clear IRQ, `\|= 0x40000000` = clear error, `&= ~0x20000000` after done |
| `+0x04` | mode: bit31, bit30, bits26-27 (width/phase), bit25, bit24; read bit31 → result −2 |
| `+0x08` | `byte<<24` (divider [?]) |
| `+0x0C` | `(len−1)<<24` |
| `+0x10` | `0x01000000` = enable |
| `+0x14/+0x18` | 16-bit count split into two `<<24` bytes |
| `+0x1C..+0x48` | extra counts/gaps, same byte-in-bit24 style |

ISR `43917f08`: status −1 (error) → `43919024`, ≠0 (done) → `43918e18`, always `43918ffc` (W bit31), then
`cb(0/1/2)`. The channel task waits on its sem with a **5000 ms** timeout (`43ac79a6`). The 64-byte request layout
(`4369c234` and others):

```
+0  u8  channel           +1 u8 err (out)     +2/+3 u8 CS gpio port/pin [?]    +4 u8 =1
+8  u16 =6 (frame/word cfg [?])               +32 u32 =4    +36 u32 dir/flag    +40/+44 u32 SPI mode pair
+52 u32 clock Hz (4 800 000 / 8 250 000 for the sub-micro, 1 250 000 others)
+56 ptr → {tx buf, rx buf, u16, u16, u32 len (8 or 128)}       +60 completion callback
```

## 5. Sub-micro ("front"/power micro) link

### 5.1 Transport = **SIO channel 0 (SPI) + GPIO handshake**, not UART1
* Link driver module 0x4369A000–0x4369E000. Init `4369cb84` (from task 65 → `43753194 → 4369afec`):
  * gets 2 shared buffers via sync 0x8F013A `{11, 21, 0x80000001/2}` (fallback: `liro_malloc(256)`);
  * registers `reg_msg_queue_cb(0x2804C0, 4369b6e0)`;
  * parses the static command tables `byte_43EDA2DD` (80 B), `43ED9E1D` (600 B), `43EDA0DD` (488 B). These are TLV
    `{id, len, …}` lists of sub-micro command/response descriptors [?].
* Link state machine: event-driven task body `4369af28`. Events arrive on queue **0x1502C7** (5-word messages):
  `4369afa0(evt)` from the ISRs, `4369be78` from SIO completions. The task dispatches through `loc_43EA78FC` until
  event 248 (shutdown).
* Handshake ISRs:
  * IRQ 128 (p0.7): `4369aa78` posts event 16 with arg = level of **p0.0** (`43ac774c(0,0)`). This is the micro's
    "request/attention" line [?].
  * IRQ 133 (p0.12): posts event 0.
  * IRQ 144 (p0.16): posts event 308/309 depending on a GPIO level [?].
  * State init `4369cee8`: event 320, reads p0.0, and if it is already set it runs the IRQ-128 path at once.
* SIO transactions issued by the state machine, all sync 0x740017 on **channel 0**:

  | function | clock | mode (+40/+44) | payload |
  |---|---|---|---|
  | `4369c234` | 4.8 MHz | 2 / 2 | 8-byte buffer `0x43E3354E` |
  | `4369c32c` | 8.25 MHz | 1 / 3 | 8-byte buffer `0x43E33546` |
  | `4369c820` | — | — | 128-byte block, completion `4369be78` |

  `4369c120`/`4369c90c` arm 128-byte receive windows (`4369ace0(…,128)`). So **a transaction = 8-byte header
  exchange, then a 128-byte frame in one direction** [? exact roles and byte meanings not decoded].
* Frames are forwarded to **task 48** (`439106c4`, the sub-micro protocol task) on queue **0x2804B6** (128 B,
  `4369b7f0`). Task 48 replies or queues output frames on **0x2804B7** (`439107f0`), which the link reads in
  `4369b620`. Task 48 decodes into the cached state behind the 0x28xxxx sync handlers. It pushes notifications to
  0x2802E0, 0x2803C6, 0x280230, 0x28022D/2E/2F (battery/power, as described in RE_INPUT), 0x280261, 0x28017D,
  0x2801D2 and 0x2804C0. It also talks to UPM 0x9203E1/0x9203E6 (`4391176c/439117c0`, msg 0x920111) and to RTC
  udate 0x90013D.

### 5.2 First exchanges at boot (static order)
1. Task 3 → `43ac5d0c`: sub-micro module init, then Backup reads 0x280003..0x280011, 0x510010, 0x51000B
   (`439108c4`), buffer setup (`43915644`), queue registration, `439105fc` (subsystem inits). No I/O.
2. Task 65 → `4369cb84`: link init (above). The link task then posts event 320, samples p0.0 and arms
   IRQ 128/133/144. **These three are enabled after boot, so this step ran.**
3. The first SIO-0 transfer starts after a handshake event (IRQ 128 edge, or p0.0 already high) or after a request
   from task 48. **None has started** (IRQ 201 never enabled). So the real micro is presumably expected to raise its
   request line at power-on [?].

Emulator recipe [?]: hold p0.0 = 1 and pulse IRQ 128 once after `ena_int(128)`. Answer the 8-byte header and the
128-byte frame on SIO-0 with a canned "power-on status" frame. The frame contents must come from a real-camera
capture or a deeper decode of the TLV tables (`43ED9E1D`/`43EDA0DD`).

### 5.3 What is *not* on the sub-micro link
* **UART1 (0xF2001000, IRQ 151)**: configured by `43993798` at 1.5 Mbaud (or 750 kbaud). Its TX/RX/err callbacks
  are `nullsub`s and it uses polled/DMA output → a high-speed output stream (trace/kikilog-style or an accessory
  port) [?].
* **UART0 / IRQ 150/151/152**: `4355f7d0` installs an av-cam monitor console at 9600 baud with real RX/TX callbacks,
  but only if `4355fe30()` == 0 (debug mode) [?].
* The ADC (0x720018) and the jog are separate (§1, RE_INPUT).

## 6. Other register facts recovered

* **SYSV 0xF3402000** (`43ab37ec(off)` = rd 0xF3400000+off; `43ab51ae` = wr):

  | off | use |
  |---|---|
  | `0x2028` | status bits 0/1/2 |
  | `0x202C` | signed 21-bit value, bit28 = sign |
  | `0x203C` | period, written 64800 |
  | `0x2044` | written by the HS mode change |
  | `0x2094` | sub-count |
  | `0x2098` | **frame counter** (read twice for consistency) |

* **ADC unit u at 0xF2406000 + 0x1000·u**:

  | off | use |
  |---|---|
  | `+0x00` | channel mask (0xFFF) |
  | `+0x04` | bit0 start |
  | `+0x08` | bits0-1 mode (2) |
  | `+0x0C` | bit0 IRQ status, bit4 ack, bit8 IRQ enable |
  | `+0x10` | bit0 busy |
  | `+0x44` | `(div<<8) \| n` (0xA03 / 0xFE03 seen) |
  | `+0x48` | 0x2F |

  Results: 12 channels read through `43ac7528(u, ch)` [offset not decoded].
* **CXD4115 timer ch10–15**: ack = `+0x04 |= 1` at 0xF2403000 + 0x100·ch (same as the kernel channels).
* **UART PL011 (av-cam side)**: MIS `+0x40`, ICR `+0x44`, clock check `+0x? ==63 → 12 MHz else 48 MHz`
  (`43919e24`).

## 7. Open items
* The exact blocking point of task 65 (§3.3). This needs one runtime look at the checkpoint globals or the thread PCs.
* Sub-micro frame format (the 8-byte header semantics, the 128-byte frame layout, the TLV command tables).
* Names and bases of the RC objects (IRQ 40–50) and of the 0xF3850000 family.
* What the display IRQs 101/102 need in 0xF3440000 to be self-consistent (task 43 reads state via
  `438e7a7c/438e7aa0`, which are stubs in this build).
* ADC result register offsets and the "no key" voltage levels.

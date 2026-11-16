# RE: live-view / AE pipeline status and the live F-number gap (measured 2026-11-16)

Consolidates and **re-measures** the live-view control path against the current emulator HEAD. It verifies
that the YC-state-2 blocker of `RE_LIVEVIEW_PIPELINE.md` is **closed** (commit `9d0cad7`), and it corrects the
causal model of `RE_LENS_EMOUNT.md` §11.7 for the live F-number (SDCom param 267 / OSD "F--").

Method: two runs of the **unmodified** firmware 1.10 on pristine COW NAND clones (frozen `qemu-system-arm`
copy), one `lens=none` (machine default) and one `lens=ekit`, no SD. Probes with the `a5ktrace` TCG plugin
(`fn=` set, av-cam VAs; PA = VA + 0x40000000), the `a5k_lens_block` / `a5k_lens_frame` device tracepoints,
HMP `xp` of the VPH/SYSV register files, and panel console-0 screendumps. All facts carry trace/`xp`
evidence; `[?]` = unproven, `[H]` = magnitude is a guess.

---

## 0. TL;DR

* **The YC-state-2 gap is closed.** It was never a missing completion IRQ or missing hardware *data* (unlike
  the OPD/IDT "device raises status but writes no result" pattern of `RE_CAPTURE_COMPLETION.md`). It was a
  **clock-timing** gap: the SYSV "V" ran at 60 Hz while the engine frame runs at 33 367 µs, so the SYSV frame
  counter advanced by 2 per engine frame and the V-phase bit0 was constant at each frame start, which made
  av-cam re-init the camera mode every frame and cancel every queued Image before EE stage 1
  (`RE_LIVEVIEW_PIPELINE.md` §2). The fix (commit `9d0cad7`) makes **SYSV the master V clock** (`V_US = 33367`
  in `cxd90014_vph.c`) and drives the evsel engine frame (source 29) from it (`ext-vd=true`, `vph` gpio-out →
  `evsel` gpio-in 0, `sony_a5000.c:1909/1911`); the panel vsync (IRQ 101/102) keeps its own 60 Hz timer.
* **Verified today, both lens modes:** `437737D4` (YC state 2) is hit **~3/s** (79 hits/25 s on `lens=none`,
  109 hits/~30 s on `lens=ekit`), always from `lr = 435475F5` (the EE-stage-1 call site, matching the
  `RE_LIVEVIEW_PIPELINE.md` §5.3 acceptance). `4376A168` (mode re-init) gets **0 hits** (the storm is gone).
  The chain runs downstream to VPH: `43661BC8` and `438F6CF4` fire per frame, **VPH `+0x144 = 0x01000233`**
  (YUV layer + OSD on) and **ch51 Y-address `0xF3441220 = 0x8CE00000`** (non-zero, the live-view buffer).
* **The AE float path runs.** `4372A38C` is hit 128 / 180 times in the two runs. This **refutes** the premise
  of `RE_LENS_EMOUNT.md` §11.7 finding #2 ("that [AE] pipeline does not run"), which rested on the now-stale
  README claim "live view stops before YC state 2".
* **The live F-number is still blocked — by a *different*, downstream gap.** With `lens=ekit` and the full
  pipeline running, the panel OSD **still shows "F--"**, and the lens serial link goes **completely idle after
  the startup handshake**: 8 frames total, all **type 2** (startup), blocks `01 07 08 09 0a 0b 0d 10` once
  each, last at guest t = 9.83 s; **zero periodic type-1 frames, zero block 0x05/0x06** for the whole shooting
  session. So av-cam never fetches a *current* aperture from the lens, `camInfo.fNumber` never becomes valid,
  and param 267 stays 0 / "F--". Advancing YC to state 2 was **necessary but not sufficient** for the
  F-number.
* **Consequence.** The F-number is **not** a YC-state-2 problem (that is fixed) and **not** a synthesized-ISP
  problem. It is the **lens live-aperture reporting** path: av-cam does not issue periodic lens requests even
  with live-view VD running, so the §11.5/§11.7 "synthesize periodic 5/6 frames" idea is still dead (av-cam
  must *initiate*, and it does not). The remaining work is to find **what triggers** av-cam's periodic lens
  poll — it is *not* the VD we now model (§4). This is a follow-up RE task (needs IDA + dynamic trace); a
  lens-model or register poke alone cannot light F faithfully, so this agent **stops at the spec** (§4).

---

## 1. What was measured

### 1.1 Runs
| run | machine | NAND | monitor | trace |
|---|---|---|---|---|
| A | `lens=none` (default) | COW clone of `state/a5000.nand` | tcp 55100 | `a5ktrace fn=` YC/AE/camInfo probes |
| B | `lens=ekit` | COW clone | tcp 55120 | same + `-trace a5k_lens_block,a5k_lens_frame` |

Both booted to the stock shooting screen (~110 s) with no SD card (A: "NO CARD"; B: lens attached, the
bottom OSD row drops the `30"`/`±0.0` fields).

### 1.2 Probe hit counts (steady shooting screen)
| probe (av-cam VA) | meaning | run A (`none`) | run B (`ekit`) |
|---|---|---|---|
| `437737D4` `yc_state2` | EE stage 1 sets YC output state 2 | **79** (~3/s), lr `435475F5` | **109**, lr `435475F5` |
| `43661BC8` `vph_yuvcall` | `VphUpdateYuvCall` (VDF→VPH) | 79 | (per frame) |
| `438F6CF4` `vph_desc` | VPH YUV descriptor program | 400 (cap) | (per frame) |
| `4376A168` `modeinit` | camera-mode re-init (the old storm) | **0** | (0; no storm) |
| `4372A38C` `ae_float` | AE/exposure float helper | **128** | **180** |
| `435EB4A8` `caminfo_dump` | camInfo field dumper | 0 (not on hot path) | 0 |
| `43A4A730` `lens_rxpoll` | lens serial RX poll | 0 after startup `[?]` | 0 after startup `[?]` |

### 1.3 Register / display state (both runs, HMP + screendump)
* `xp 0xF3440144 = 0x01000233` — VPH panel YUV layer on (bit0) + OSD on (bit1). Matches
  `RE_LIVEVIEW_IMAGE.md` §1.1 "steady live view".
* `xp 0xF3441220 = 0x8CE00000` — AIF ch51 Y start address, non-zero (the live-view plane; alternates
  `0x8CE00000`/`0x8CF34000`). ch51 is programmed every frame.
* Panel OSD (console 0): aperture scale `F1.4 2.8 5.6 11 22` with a marker, shutter scale `1" 1/30 1/250
  1/4000`, and the **`F--` placeholder** at bottom-left in **both** runs.

### 1.4 Lens serial activity (run B, device tracepoints, whole boot)
```
a5k_lens_frame … type 2 … first block 0x01   (and 07,0b,08,09,0d,10,0a)  — 8 frames, last t=9.83 s
a5k_lens_block request block 0x01 / 07 / 08 / 09 / 0a / 0b / 0d / 10     — each once
```
No `type 1` frame, no `block 0x05`/`0x06`, nothing after t≈9.8 s. The link is idle for the entire shooting
session (confirms `RE_LENS_EMOUNT.md` §11.7 finding #1 — but now with the live-view pipeline *running*, which
§11.7 could not separate).

---

## 2. The YC-state-2 gap (closed) — summary with evidence

Root cause and fix are fully derived in `RE_LIVEVIEW_PIPELINE.md` §2–§5 (the EE Image functor, the 2V phase
check at `4376D904`, the VDF frame gate at `4367EC50`). The one-line gap: **SYSV V must tick once per engine
frame, in phase with evsel source 29**, or av-cam's 2V check mismatches every frame and cancels the Image
before EE stage 1 (the only caller of `437737D4` = YC state 2).

Implemented in HEAD:
* `cxd90014_vph.c`: `#define V_US 33367`; `vph_vtick` increments `s->frame` (so SYSV `+0x98` and the `+0x28`
  bit0 phase), pulses IRQ 98, then raises `vd_out`; the 60 Hz `vph_tick` keeps only the panel vsync (IRQ
  101/102) and the ch51 start-bit clear.
* `cxd90014_evsel.c`: `ext-vd` property; when set, source 29 is driven by `ev_vd_in` (gpio from SYSV) instead
  of the local 33 367 µs timer, so source 29 cannot drift against the SYSV counter.
* `sony_a5000.c:1909` `qdev_prop_set_bit(dev, "ext-vd", true)`, `:1911`
  `qdev_connect_gpio_out(vph, 0, qdev_get_gpio_in(evsel, 0))`.

Verification = §1.2/§1.3 above (yc_state2 ~3/s, modeinit 0, ch51 programmed). This also matches
`RE_CAPTURE_COMPLETION.md` §330 ("YC chain runs ≈1 `437737D4`/s") on an earlier binary. **The README "SA DSP"
line's clause "live view still stops before YC state 2" is stale** and is corrected in this commit.

**Not needed for YC state 2** (re-confirmed): the develop/resize IRQs 67–75, VFX 77/78, front-end 35/37,
JPEG/IPU, and the OPD/IDT per-zone result data — none of these are on the path (`RE_LIVEVIEW_PIPELINE.md`
§5.2). This is why the gap was *timing*, not *missing data*: supplying a completion event or plausible stats
(the tractable OPD-style fix) would not have helped, and indeed none was required.

## 3. The live F-number gap (open) — precise location

`camInfo.fNumber` (av-cam camInfo struct: valid@+12, numerator@+16, denominator@+20; param 267 = 100·num/den;
`RE_LENS_EMOUNT.md` §11.7 finding #2) is filled by the exposure/AE float path. That path **runs** now
(`4372A38C` 128/180 hits), yet the OSD stays "F--". The missing input is the **current lens aperture**:

1. The lens serial link is **idle after startup** (§1.4): av-cam issues **no periodic lens request**
   (type-1 / block 5/6) in steady state, so no *current* av reaches av-cam.
2. The **startup** blocks do not carry the live aperture: `RE_LENS_EMOUNT.md` §11.7 finding #3 showed that
   filling block 8 (the startup capability table at `0x43CC5B30`) still left the OSD at "F--"; block 7 carries
   lens capability, not the live av (§9.5).
3. So `camInfo.fNumber.valid` stays 0 and the OSD shows "F--". (I could not independently read the camInfo
   struct: `435EB4A8` did not fire in steady state and its struct base is accessor-indirected, §11.7 step
   (b). The display + the idle link are sufficient evidence.) `[?]` direct camInfo read not obtained.

**Correction to `RE_LENS_EMOUNT.md` §11.7:** its revised plan said the F-number is "gated on the live-view/AE
pipeline (the ⬜ Sensor/ISP live-view item)". That pipeline is now modelled and running (YC state 2 + AE
float), and the F-number is **still** blocked. So the gate is **not** YC state 2; it is specifically the
**periodic lens-aperture request mechanism**, which does not run even with live-view VD active.

## 4. Minimal spec for the follow-up (F-number) — do NOT force now

The faithful chain is lens-frame → av-cam av→F×100 → `camInfo.fNumber` → param 267 / OSD. The only unfaithful
shortcut (poking an aperture into av-cam memory, or forcing a reply to an unrequested block) fabricates
hardware data the firmware never asked for, so it is out.

The real open question is **what makes av-cam issue a periodic lens request** (the TX request builder that
precedes the `43A4A730` RX poll). It is **not** the VD / YC frame we now model (§1: YC runs at ~3/s, lens link
idle).

**DECODED (2026-11-16, Ghidra, `RE_LENS_EMOUNT.md` §11.9).** `AE-ctx[35]` is `job+0x23`, the **generic job
sub-state** of av-cam's job-state-machine framework (getter `43A6EAFC`, init-to-0 `43A6CB24`), reused by ~30
module handlers and by the EE-stage core `43546E40`. The three handlers `43A89734/824/8A6` are three
independent `job+0x23` jump-table state machines registered into one module object (base `0x43DC6AE0`, slots
8/10/11). Handler `43A898A6` case 1 = the apply path (`→4385EC74→4380C5A8`, reading the f-number rational
straight from `ctx+0x11A6/0x11AE/0x11BA` that metering `437737D4` already filled — **the apply consumes
pre-computed exposure, it does not need real pixels**). Case 0 posts an async request and returns 0; the job
self-advances 0→1 on that completion. **The real gap:** the handlers get 0 hits because `job+0x23` is 0 at
creation and only advances once the handler runs — i.e. the **AE-control job is never POSTED**. Its poster is
the live-view AE/EE sequence (`43546E40`, itself an `img+0x23` machine), which reaches the metering stage but
not the control/apply stage in the emulator. **Classification: NOT the "fabricate metering" deep case, but
gated behind the live-view image/EE sequence advancing to its control stage — which is coupled to the
synthesized-ISP dataflow of §5 / `RE_SENSOR_ISP.md` §4.b.** No small faithful device change sets
`AE-ctx[35]=1` in isolation; the precise next step is a dynamic trace of `43546E40`'s stage byte to find which
EE predicate stalls after metering (bare completion flag → tractable; reads image content → deep). Full
evidence + the probe recipe in `RE_LENS_EMOUNT.md` §11.9.

**RESOLVED (2026-11-16, `RE_LENS_EMOUNT.md` §11.8).** The periodic block 5/6 request is built by
`sub_436CFB70 → sub_43A4C60E → sub_436B5188`, reached only from `sub_4380C5A8` (the AE-result→lens-aperture
driver), which runs only via the AE **control/apply** job `sub_43853DA8`. That job is dispatched by the per-VD
AE job-slot handlers `sub_43A89734 / 43A89824 / 43A898A6` **only when the AE job-context state byte
`*(ctx+35)` (`sub_43A6EAFC`) == 1**. A `lens=ekit` trace shows AE metering running hard (`4372A38C` ×360,
`437737D4` ×195), the lens fully recognised (BL state `0x07`, all 8 startup blocks answered), and
`sub_4380C5A8`'s own gate flag already set (`[0x43E44162]=0x01`) — yet `sub_43A89734/824/8A6`, `sub_43853DA8`
and `sub_4380C5A8` are **never executed**, so block 5/6 is never enqueued.

So the two earlier candidates are settled:
1. **Not a lens startup-block / "aperture-controllable" byte (refuted).** The lens is fully attached and the
   startup blocks all complete; the gate is inside av-cam's AE scheduling, independent of the lens reply bytes
   (and §`RE_LENS_EMOUNT.md` 11.7 #3 already showed populating block 8 does nothing). No `lens_put_block`
   change helps.
2. **Not S1/S2 half-press (refuted).** Zero periodic frames on key events (prior run + this run). The trigger
   is the AE **sequencer** scheduling the control/apply job (driving `AE-ctx[35]` to 1) — a live-view/AE layer
   the emulator does not reach.
3. Aperture-byte decode therefore stays blocked (can only be done once a real 5/6 request fires).
4. The inert `a5k-lens` `aperture` / `focal-length` props remain the eventual knob.

This is a **live-view / AE-sequencer** task, not a lens-model task: make av-cam's AE sequencer enqueue the job
slot at `0x43EE10E0 / 0x43EDECD4 / 0x43EE0754` and drive `AE-ctx[35]==1`. Until then there is **no small
faithful change** that lights F, so none is made. (The host-only Track-B build still fakes 267 via
`A5K_FAKE_APERTURE` for UI testing; `RE_LENS_EMOUNT.md` §11.5.)

## 5. What remains for a *real* live-view image (separate from F)

The ch51 plane is programmed but its buffer (`0x8CE00000`/`0x8CF34000`) is all-zero — nothing writes pixels,
because the sensor/ISP produces no image (the resize engine RC reads a front-end stream that is never
generated). This is the `RE_SENSOR_ISP.md` §4.b item and is independent of the F-number. The debug knob
`-global cxd90014-aif.lv-source=bars|ramp|file:PATH.pgm` injects a test luma frame at the RC ch26 completion
(`RE_LIVEVIEW_IMAGE.md`), which is a stand-in, not a faithful ISP image. A faithful live image needs a
synthesized ISP/sensor pipeline — the "deep" case the task flags to stop before.

## 6. Confidence
* YC state 2 reached / gap closed by `9d0cad7`: **high** (yc_state2 ~3/s both runs, modeinit 0, ch51
  programmed, `+0x144=0x01000233`; code wired in `sony_a5000.c`).
* AE float path runs: **high** (`4372A38C` 128/180 hits) — refutes §11.7 #2's premise.
* F-number still "F--" with the pipeline running: **high** (screendumps, both lens modes).
* Lens link idle after startup / no periodic request: **high** (device tracepoints, 8 type-2 frames only).
* F-number gated on the periodic lens-request mechanism (not YC state 2, not ISP content): **high** for "not
  YC state 2" (measured), **medium** for the exact trigger (candidates in §4, undecoded).
* `43A4A730` is the lens RX poll and gets 0 steady-state hits: **low/`[?]`** (probe injected post-startup; the
  device-side idle trace is the solid evidence, not this probe).

## 7. Reproduce
* `S=<scratch>; cp -c state/a5000.nand $S/lv.nand` (COW). Frozen `cp qemu/build/qemu-system-arm $S/qemu-frozen`.
* Run A: `A5K_NAND=$S/lv.nand A5K_QEMU=$S/qemu-frozen ./run.sh -display none -monitor
  tcp:127.0.0.1:55100,server,nowait -d plugin -D $S/trace.log -plugin
  tools/liba5ktrace.dylib,fn=$S/probes.txt,fncap=400,fnwords=6,fnr1w=12,ts=1`. Wait ~130 s, then write the
  probe set (§1.2 VAs) into `probes.txt` (reloaded on mtime) and capture ~25 s.
* Run B (`lens=ekit`): the machine prop goes in `-M`, not as a positional arg — invoke `qemu-frozen` directly
  with `-M sony-a5000,…,nand=$S/lv.nand,…,lens=ekit … -trace a5k_lens_block -trace a5k_lens_frame`.
* HMP: `tools/mon.py 55100 'xp /1wx 0xF3440144' 'xp /1wx 0xF3441220' 'screendump out.ppm'`
  (`sips -s format png out.ppm --out out.png`).
* An unrelated engine session may run concurrently; a mid-boot exit is an oops — relaunch on a fresh clone and
  a distinct port.

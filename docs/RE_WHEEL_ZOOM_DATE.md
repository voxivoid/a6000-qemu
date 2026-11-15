# RE: control-wheel rotation, zoom T/W mapping, blank Date/Time row

Scope: three open input/UI items in the `sony-a5000` machine running the unmodified 1.10 firmware.

1. **Control-wheel rotation** is not wired (`RE_ENGINE_TICK.md` §11/§12 "Not wired: av-cam `JogDev`/`JogTpu`, a TPU counter"). This document locates the counter, the sampling path, and the event chain, and specifies the board wiring.
2. **Zoom T/W** arrives in event0 as 611/610 in the emulator, the reverse of the real camera (610/611). This document shows which is right from the firmware's own key tables and specifies the fix.
3. The **Date/Time preview row** (Setup → Date/Time Setup) is blank. This document rules out the clock/RTC path (live-verified) and locates the real cause in the UI model layer.

Method: static analysis of `av-cam.bin` (LIRO/µITRON image, IDA 9.4 + Hex-Rays on a copy), `libObj.so`, `libInputServant.so`, `viewUnified{2,6}.so`, plus the `com.sony.scalar.sysutil.ScalarInput` constants; dynamic runs of the unmodified firmware with HMP, the gdb stub (physical-memory mode) and the `a5ktrace` TCG plugin. `[?]` = not proven.

Related: `RE_INPUT_I2C_POWER.md` (av-cam owns keys/ADC/jog/RTC, OSAL queues), `RE_ENGINE_TICK.md` §11/§12 (button wiring, ADC ladders, shutter/zoom), `RE_RTC.md` (sub-micro clock, udate), `RE_AVCAM_IRQ_BOOT.md` (SIO/ADC/timer blocks).

---

## 0. TL;DR

* **Wheel rotation = a 16-bit TPU counter at `0xF2404014`** (block `0xF2404000`, av-cam "TPU" channel 0), today caught only by the `io-f0` logging regfile (reads back the last value written, so the delta is always 0 → no rotation). av-cam samples this counter from a periodic (µITRON alarm) task, computes a wrap-around delta, scales it to degrees, and posts a dial event. The event becomes event0 **525 (dial-1 CW) / 526 (dial-1 CCW)**. **Live-verified**: writing `0xF2404014` with the gdb stub makes av-cam emit dial-delta events (§1.4). Fix: give the board a real counter at `0xF2404014` that two host keys increment/decrement (suggested `]` = CW, `[` = CCW).
* **Zoom: the project notes are right (T→610 ISV_KEY_ZOOM_TELE, W→611 ISV_KEY_ZOOM_WIDE); the emulator has T and W swapped.** The firmware's own `libInputServant` `zoom_cb` maps the SLIKEY_ZOOM value 1/2 → **611 (WIDE)** and 0x8001/0x8002 → **610 (TELE)**. The emulator feeds T the value-1/2 ADC bands and W the value-0x8001/0x8002 bands, i.e. T→611(wide), W→610(tele). Fix = swap the two ADC levels in `a5k_key_event` (§2.3).
* **Blank Date/Time row is NOT a clock/RTC bug.** Live-verified: after a normal boot the udate base is valid (flag 1 = UTC, host date, TZ +540, DST 0, area flag 0); `udate_get_localtime` converts UTC→LOCAL and reports TM type 0, so the clock is displayable. The blank row is the `ACT_LabelDate` preview widget in the generic settings menu (`viewUnified2` `sub_2A4C28`), which queries `getModelInfo("@M015"/"@M060"/"@M063"/"@M090", 9)` for year/month/day and **skips `PAS_LabelDate::setDate` when that query fails** (model not prepared / params absent, per `RE_RTC.md` §6 live probes). It is a UI model-availability issue, independent of the sub-micro RTC (§3). Next probe named in §3.3.

---

## 1. Control-wheel rotation (TPU counter)

### 1.1 The hardware: 16-bit counter at `0xF2404014`, block `0xF2404000`

av-cam owns the control-wheel ("jog dial") as C++ classes `JogDev` / `JogFg` / `JogTpu` / `JogVGrip` (RTTI strings `6JogDev`, `5JogFg`, `6JogTpu`, `8JogVGrip` in `av-cam.bin`) plus `Camera::Device::CRingDial` / `CBearFgRingDial`. The wheel of the a5000 is the `JogTpu` variant (counter-based).

`JogTpu`'s vtable (`0x43E7B940`) is `{439C1D12 (arm), 435A6B2C (start/config), 435A6A69, 439C1CBF, 439C1D01, 439C1CC2 (read-delta)}`.

The counter is read by `JogTpu` via a thin accessor:

```
sub_43919B10(ch):  return *(u16*)((ch << 8) + 0xF2404010 + 4)   ; dword_43919B1C = 0xF2404010
```

For channel 0 this is **`*(u16 *)0xF2404014`**. The arm/config path (`439C1D12` → `43AC7ADC` → `4391959C`, and `4391996C`) programs the same block:

| reg (16-bit) | written at boot | meaning (from access pattern) |
|---|---|---|
| `0xF2404000` | 0 | control / enable |
| `0xF2404004` | 4 | mode (event/up-down counter) |
| `0xF2404014` | read, init 0 | **counter value** (the wheel position) |
| `0xF2404018` / `0xF240401C` | `0xFFFF` | compare / reload (full range) |
| `0xF2404028` | `0x3F` | ? |
| `0xF240402C` | read→write 1 | status / start |

Source: `av-cam.bin` `JogTpu` arm/read (`43919656`, `4391996C`, `43919A74`, `43919B10`, `43919B20`, `43919B34`, `43919BBC`) decode the block at `0xF2404000` (literals `dword_43919B1C`=`0xF2404010`, `dword_43919A50`=`0xF2404000`, `dword_43919AC8`=`0xF2404000`, `dword_43919ACC`=`0xF2404018`, `dword_43919AD0`=`0xF2404020`, `dword_43919BE4`=`0xF2404000`, `dword_43919BB0`=`0xF2404008`). The block is `0xF2404000`, only channel 0 is used (all live accesses fall in `0xF2404000..0xF240402F`). It sits under the `io-f0` logging regfile (`hw/arm/sony_a5000.c` `a5k_regfile("io-f0", 0xF0000000, 0x08000000)`), so it is currently unmodeled.

This block is distinct from the CXD4115 timer block at `0xF2403000` (already modeled) and from the ADC at `0xF2406000`.

### 1.2 Sampling: absolute counter → wrap delta → degrees

A periodic av-cam task (driven by the OSAL alarm timer `0x910040`) polls each dial object. For the wheel:

```
sub_435A6468(mgr, counter):             ; iterate the 5 dial entries (44 B each, @ unk_43D403CC)
   for each entry e: sub_439C1F36(e, counter)

sub_439C1F36(e, counter):
   if (*e != 0 && counter == *e):
       (*e->vtbl[12])(e, ...)            ; JogTpu read
       delta = (*e->vtbl[16])(e, prev, new)   ; -> sub_439C1CC2 for JogTpu
       sub_439C1EC6(e, delta, ...)       ; state machine (idle/CW/CCW), prev = new

sub_439C1CC2(a1, prev, cur, period):     ; JogTpu read-delta (vtbl+20)
   v9  = 4355C3A8(0xFFFF, period)        ; normalise the period
   v10 = sub_439C1C76(prev, cur, v9)     ; wrap-around signed delta
   return 439515A4(360, 0, a1[5], 0) * v10   ; scale counter steps -> degrees

sub_439C1C76(a1, prev, cur, mod):        ; signed modular difference (cur-prev) mod (mod+1)
```

`sub_439C1E64`/`sub_439C1EC6` keep `prev` (`a1[7]`) and emit only on a state change, so a counter increment of N detents = a CW rotation of N steps; a decrement = CCW. The scaled delta (degrees) is sent to libObj as an `InfraDialEventDataRecord` on OSAL queue **`0x11B02AA` (18547370)** via `sub_435A6690` → `sub_435A65EC` → `osal_snd_msg (4355D9DC)`.

The dial detent spacing / curve is set from Backup at init (`InfraDial::SetDialEventInterval`, Backup `0x1070100/01/02` = max interval / min interval / response curve, `libObj.so` `InfraDial` ctor `0x710F4C`). This affects event pacing only, not whether events fire.

### 1.3 Event path to event0 525 / 526

libObj's `InfraDial` receives the record on `0x11B02AA` (`InfraDial::eventReceiveCallback` `0x710370`), maps `local_key_code` → `key_code` through its Backup dial-key table, and posts `{key_code, status, ex_info}` (16-byte) to the key sink queue **`0x7A0020`**. `ex_info` carries the signed rotation.

`libInputServant.so` turns that into the uinput event0 code (`ui_libInputServant.so.c` `dial_cb` @ `0x2950`):

```
dial_cb(a1):
   if (*a1 == dial1_status_vcode): v3 = (a1[2] < 0) ? 526 : 525   ; dial-1 CCW/CW
   else if (dial2): v3 = (a1[2] >= 0) ? 528 : 529                 ; dial-2 CW/CCW
   else if (dial3): v3 = (a1[2] >= 0) ? 635 : 634                 ; dial-3
   emit {v3, 1}{v3, 0}
```

The a5000 control wheel is **dial 1**, so CW = event0 **525**, CCW = **526** (`ScalarInput.ISV_DIAL_1_CLOCKWISE = 525`, `ISV_DIAL_1_COUNTERCW = 526`; also `ISV_DIAL_1_STATUS = 521`). This matches the real-camera capture `re/keys_capture3.txt` (`code=525` CW, `code=526` CCW, down+up together) and the project key map (`CLAUDE.md` §5). The press-and-release-together pattern (`{v3,1}{v3,0}`) matches the capture.

### 1.4 Live confirmation (why nothing rotates today, and that the path works)

* av-cam polls `0xF2404014` continuously: at boot ~536 reads, then periodically from the dial task (`a5ktrace` on `pc=0x439C1E64`/`0x435A6690` fires from the `UTIMER*` tasks). Because `0xF2404014` is only the `io-f0` logging regfile, it reads back the last value written (0), so `cur == prev` every time → delta 0 → no event. That is the whole bug.
* **Writing the counter produces dial events.** With the gdb stub in physical mode (`set {unsigned short}0xf2404014 = N`), the dial task picks up the change: `a5ktrace` on `pc=0x435A6690` (the emit to queue `0x11B02AA`) then fires with a non-zero scaled delta in `r2` (e.g. `r2=0x18` = +24 and `r2=0xFFFFFFE8` = −24 after up/down pokes), whereas the idle polls carry `r2=0`. This exercises §1.2→§1.3 end to end.
* Note: the 6-tab main-menu grid is navigated by the 4-way wheel keys (arrows), **not** by wheel rotation, so poking the counter does not move the grid cursor. Rotation is consumed by scrollable lists and the shooting-screen dials. Verify rotation there, not on the tab grid.

### 1.5 Spec — board wiring (`hw/arm/sony_a5000.c`)

**Model a real up/down counter at `0xF2404014`** and wire two host keys to it.

Minimal approach (no new device file): in the machine state add `int16_t wheel_count;` and intercept the single half-word at `0xF2404014`. Cleanest approach: a small `cxd90014`-style device covering `0xF2404000..0xF240402F` whose only live register is the counter at `+0x14`; the other registers echo writes (as the regfile does today). Either way:

* A host key-down for **CW** does `wheel_count += step` (wrap as unsigned 16-bit); **CCW** does `wheel_count -= step`. `step` = 1 by default (one detent per press); with Shift held use a larger step (e.g. 4) for fast scroll. Key-up does nothing (the wheel is relative; the counter just holds its new value).
* Put the handler next to `a5k_adc_key`/`a5k_shutter` in `a5k_key_event`, after the ADC/GPIO devices exist.

**Suggested host keys** (unused by the current map in `README.md`): `]` = `Q_KEY_CODE_BRACKET_RIGHT` = CW, `[` = `Q_KEY_CODE_BRACKET_LEFT` = CCW. (Alternatives, also unused: `.`/`,` or `=`/`-`.) Document them in the README key table and note HMP use `sendkey bracket_right`. The 4-way wheel directions stay on the arrow keys.

**Reset value:** 0 is fine (the firmware reads the counter relative to its own cached `prev`; an absolute reset value is not required). Keep the init writes (`+0x00=0`, `+0x04=4`, `+0x18=+0x1C=0xFFFF`, `+0x28=0x3F`, `+0x2C` start) accepted/echoed so the arm path does not log-spam.

**Acceptance test:**
1. Boot to the shooting screen. Open a scrollable list where the wheel scrolls (e.g. a Camera-settings menu page, or the shooting-mode selector). Press `]` several times → the selection advances one step per press (CW); `[` reverses it (CCW). Equivalent to pressing the 4-way down/up on this list.
2. With `a5ktrace pc=0x435A6690` loaded, each `]`/`[` press logs one emit to queue `0x11B02AA` with a non-zero signed delta in `r2` (`+step`/`−step` scaled); idle frames stay at `r2=0`.
3. If the Track-A APK is installed and in front (so event0 is delivered), `]`/`[` produce event0 `525`/`526` down+up pairs (matches `re/keys_capture3.txt`).

---

## 2. Zoom lever T / W mapping

### 2.1 The firmware's own mapping is authoritative

The zoom lever is a single av-cam key `0x1BF` (`SLIKEY_ZOOM`) whose *value* encodes direction and speed, read from ADC unit 1 channel 3 (ch `0x0F`). `input.ko`'s ADC band table maps the ADC level to the value (`RE_ENGINE_TICK.md` §12.2):

| ADC level band | SLIKEY_ZOOM value |
|---|---|
| 80..244 | `2` |
| 245..358 | `0x8002` |
| 359..442 | `0x8001` |
| 443..514 | `1` |
| 515..1023 | `0` (centre) |

`libInputServant.so` converts that value to the event0 code (`ui_libInputServant.so.c` `zoom_cb` @ `0x2A18`):

```
zoom_cb(value):
   if value > 2:
       if (value - 0x8001) <= 1:  v = 610     ; value 0x8001 or 0x8002 -> TELE
       else:                      v = 767      ; UNKNOWN
   else:
       if value >= 1:             v = 611      ; value 1 or 2 -> WIDE
       else if value != 0:        v = 767
       else:                      v = 645      ; value 0 -> ZOOM_OFF
   emit {v,1}{v,0}
```

`ScalarInput` constants confirm the names: `ISV_KEY_ZOOM_TELE = 610`, `ISV_KEY_ZOOM_WIDE = 611`, `ISV_KEY_ZOOM_OFF = 645`, `ISV_KEY_UNKNOWN = 767`.

So the firmware defines:

| SLIKEY_ZOOM value | speed | direction | event0 |
|---|---|---|---|
| `1` | slow | **WIDE** | **611** |
| `2` | fast | **WIDE** | **611** |
| `0x8001` | slow | **TELE** | **610** |
| `0x8002` | fast | **TELE** | **610** |
| `0` | — | centre/off | 645 |

This agrees with the real-camera capture (`re/keys_capture3.txt`: Tele pressed first → `code=610` repeating; Wide → `code=611`) and with `CLAUDE.md` §5 (T/W = 610/611). **The project notes are correct; the emulator README (T/W = 611/610) is wrong.**

### 2.2 What the emulator does today (reversed)

`hw/arm/sony_a5000.c` `a5k_key_event`:

```
case Q_KEY_CODE_T: a5k_adc_key(s, 3, qcode, down, s->key_shift ? 162 : 478);
case Q_KEY_CODE_W: a5k_adc_key(s, 3, qcode, down, s->key_shift ? 301 : 400);
```

Mapping the levels through the band table + `zoom_cb`:

* `T` slow `478` → value `1` → **611 (WIDE)** ✗
* `T` fast `162` → value `2` → **611 (WIDE)** ✗
* `W` slow `400` → value `0x8001` → **610 (TELE)** ✗
* `W` fast `301` → value `0x8002` → **610 (TELE)** ✗

The `T` key produces a *wide* event and `W` a *tele* event — reversed.

### 2.3 Spec — fix (swap the two ADC levels)

Swap the T and W level arguments so that Tele uses the `0x8001/0x8002` bands and Wide uses the `1/2` bands:

```
case Q_KEY_CODE_T:   /* tele: value 0x8001/0x8002 -> event0 610 */
    a5k_adc_key(s, 3, qcode, down, s->key_shift ? 301 : 400);
    break;
case Q_KEY_CODE_W:   /* wide: value 1/2 -> event0 611 */
    a5k_adc_key(s, 3, qcode, down, s->key_shift ? 162 : 478);
    break;
```

(Slow/fast within each side is preserved: slow = the inner band, fast = the outer band.) Also update the comment block above `a5k_adc_key` and the `README.md`/`README_KO.md` key tables: `T` = tele (level 400 slow / 301 fast, value 0x8001/0x8002, event 610), `W` = wide (level 478 slow / 162 fast, value 1/2, event 611).

**Acceptance test:** install the Track-A APK (or any Scalar app that forwards event0), bring it to the front, and read event0. `T` → event0 `610` (ZOOM_TELE), `Shift+T` → `610` faster repeat; `W` → `611` (ZOOM_WIDE), `Shift+W` → `611`; release → `645` (ZOOM_OFF). This matches `re/keys_capture3.txt` (Tele=610, Wide=611). Without an app in front, verify instead at the av-cam boundary: `a5ktrace` on `osal_snd_msg` `r0=0x7A0020` shows `{0x1BF, value}` with `value` ∈ {`0x8001`,`0x8002`} for `T` and {`1`,`2`} for `W` (the current build shows the opposite).

---

## 3. Blank Date/Time preview row

Reproduced: Menu → Setup → (page 3) Date/Time Setup → the 3-item list `서머타임 / 날짜·시간 / 날짜형식` ("DST / Date·Time / Date format"). The **날짜/시간 ("Date/Time") row shows no value** on the right, where the real camera shows the current date/time as a preview. The date editor itself (opened from that row) works and shows the host time.

### 3.1 Ruled out: the clock / RTC path (live-verified)

The clock data is valid and displayable after a normal boot. Live read of the udate base struct (gdb/HMP, `0x3F088E48`, with `RE_RTC.md` §4 layout):

```
3F088E48: ea 07  0b 0f  0b 1e  03 00   01 00     ; 2026-11-15 11:30:03, flag=0x0001 = UTC
3F088E60: 1c 02  00 00  00 00          ; TZ = 0x021C = 540 min (+9:00), DST = 0, area flag = 0
```

`udate_get_localtime` (`udate.ko` @ `0x508`) converts to local time exactly when `base.flag == 1 (UTC)` **and** `area_flag == 0`, then reports **TM type 0 (LOCAL)**:

```
udate_get_localtime(out, tz, mode):
   copy base -> out ; copy {TZ,DST,area} -> tz
   if (out.flag != 1 || tz.area != 0) return 0     ; (would keep out.flag unconverted)
   ... add elapsed µs, TZ ...
   if (year in [min .. 2037]) out.flag = 0         ; LOCAL
```

Both conditions hold in the emulator (flag 1, area 0), year 2026 is in range, so `out.flag = 0 (LOCAL)`. libObj `M_CLOCK onGetInfo(INFO_CURRENT_LOCALTIME)` (`sub_404FD8` → the switch `case 0` at `~0x405160`) reports the "clock valid" bool (param `0x100`) as **true** exactly when the returned TM type is 0. So the clock *is* reported displayable. **The blank row is therefore not caused by the clock value, flag, TZ, area or the sub-micro RTC** (contrary to the "RTC not modelled" guess in `RE_ENGINE_TICK.md` §11.3).

### 3.2 The real cause: the UI date-label model query

The "날짜/시간" row's value is an `ACT_LabelDate` / `PAS_LabelDate` preview widget in the **generic settings menu** framework (`viewUnified2.so`), not the date editor. It is refreshed by:

```
sub_2A4C28(widget, model):              ; viewUnified2 @ 0x2A4C28
   getModelInfo(model, 9, params)       ; sub_19A6E0 -> ViewBase::getModelInfo, info-id 9
   y = ParamList::get(params, 0x10F)    ; year
   m = ParamList::get(params, 0x110)    ; month
   d = ParamList::get(params, 0x111)    ; day
   if (y && m && d && value(y)!=-1 && value(m)!=-1 && value(d)!=-1):
        PAS_LabelDate::setDate(...)
        setFormatType(Bkup 0x1070040)   ; date-format
        show
   else:
        setDispState(false)             ; <-- blank
```

It is driven for four clock/date models in turn (`sub_2A4D40`/`sub_2A4E2C`, event ids 0/1/2/4): **`@M015`, `@M060`, `@M063`, `@M090`** (model ids `0x15`/`0x60`/`0x63`/`0x90`). The widget subscribes to all four (`sub_2A4D88` `attachEventId`). The row stays blank whenever `getModelInfo(@M0xx, 9)` returns failure or omits `0x10F/0x110/0x111`, so `setDate` is skipped and the label keeps its empty/dummy text.

This matches the `RE_RTC.md` §6 live finding: during the redraw, **`PAS_LabelDate::setDate` / `setDummyDate` are never called** and no `udate_set_date`/`udate_kif_setget` happens — i.e. `sub_2A4C28` is taking the "else" branch (`getModelInfo` for the date model is not delivering Y/M/D in this view). It is a UI model-availability problem in the settings-menu path, independent of the clock value proven valid in §3.1.

### 3.3 Spec / next step

No board or RTC change is warranted for this item (the clock model reports valid). The fix belongs to understanding why `getModelInfo("@M015"/"@M060"/"@M063"/"@M090", 9)` returns no date in the settings-menu view while the editor (which uses `@M018` `INFO_INITIAL_DATE_TIME`, a path that hard-codes "valid") works.

**Next probe (for a follow-up RE pass, before any code):** in the Date/Time Setup list, hook libObj `ViewBase::getModelInfo` (or `viewUnified2` `sub_2A4C28` at its im.elf load base) with `a5ktrace` and capture the return value and the `0x10F/0x110/0x111` params for model `@M015`. Two outcomes:
* `getModelInfo` returns 0 (model not prepared/attached in this view) → the model `@M0xx` (id 0x15) is never instantiated in the emulator's settings view; investigate why that model is absent (a missing model-start event, as in the AUTOREPAIR case `RE_ENGINE_TICK.md` §12.3), **not** the clock.
* `getModelInfo` returns the params but `value == -1` → the clock/date model's `onGetInfo(id 9)` is itself gating on something the emulator does not satisfy; trace that model's handler.

Mark the exact sub-cause `[?]` until that probe is run. Whatever the outcome, §3.1 shows it is a UI-model issue, so the item should be moved out of the RTC/sub-micro section of the README into the UI-bring-up list.

---

## 4. Evidence index

| Claim | Evidence |
|---|---|
| Wheel counter at `0xF2404014`, block `0xF2404000` | `av-cam.bin` `JogTpu` read `43919B10` (`dword_43919B1C`=`0xF2404010`, +4), arm/config `4391959C`/`4391996C`/`43919A74`/`43919BBC`/`43919B34` (block literals `0xF2404000`/`08`/`18`/`20`); live `-d unimp` shows all wheel-block I/O in `0xF2404000..2F`, counter polled at `0xF2404014` |
| Sampling + degrees | `av-cam.bin` `435A6468`/`439C1F36`/`439C1CC2`/`439C1C76`/`439C1E64`/`439C1EC6`; emit `435A6690`→`435A65EC`→`osal_snd_msg` queue `0x11B02AA` |
| Event0 525/526 | `libObj` `InfraDial` (`0x710370` recv on `0x11B02AA`, post to `0x7A0020`); `libInputServant` `dial_cb` (`0x2950`); `ScalarInput.ISV_DIAL_1_CLOCKWISE=525`/`COUNTERCW=526`; `re/keys_capture3.txt` |
| Wheel path live-works | gdb `set {u16}0xf2404014=N` → `a5ktrace pc=0x435A6690` emits with `r2`=±24 (idle `r2=0`) |
| Zoom value→event | `libInputServant` `zoom_cb` (`0x2A18`); `ScalarInput` `ZOOM_TELE=610`/`ZOOM_WIDE=611`/`ZOOM_OFF=645`; band table `RE_ENGINE_TICK.md` §12.2; `re/keys_capture3.txt` (Tele=610, Wide=611) |
| Emulator zoom reversed | `hw/arm/sony_a5000.c` `a5k_key_event` `T`=162/478, `W`=301/400 |
| Clock data valid | live udate base `0x3F088E48` = flag 1 UTC, TZ 540, DST 0, area 0; `udate.ko` `udate_get_localtime` @`0x508`; `libObj` `M_CLOCK onGetInfo` case 0 (`sub_404FD8`, `~0x405160`) |
| Date row = UI model query | `viewUnified2` `sub_2A4C28` (`getModelInfo(@M015/@M060/@M063/@M090, 9)`, params `0x10F/0x110/0x111`, `setDate` else-skip), `sub_2A4D40`/`sub_2A4D88`/`sub_2A4E2C`; `RE_RTC.md` §6 (setDate never called) |

---

## Implementation notes: wheel (branch `wheel`)

* New device `hw/misc/cxd90014_jog.c` (`cxd90014-jog`), 0x30 bytes at `0xF2404000`, overlay priority 0 over the io-f0 regfile. `+0x14` returns the 16-bit counter (writes set it); other registers echo writes. `a5k_key_event`: `]` = CW (+1), `[` = CCW (-1), Shift = x4, on key-down.
* Verified: with MENU > Setup page 4 open, two `]` presses moved the highlight down one row, two `[` presses moved it back up (selection moves with lag; one press alone sometimes does not cross the dial-event detent/curve threshold, so a press is a fraction of a detent). Cold boot, MENU, PLAY unaffected. The av-cam `435a6690` trace was not captured (UI reaction observed directly instead).
* Open: tune per-press step (Backup 0x1070100..02 interval/curve) so one press = one row.

# Engine frame tick → sub-microcontroller → UI startup (2026-11-14)

The four gates that blocked the UI before its first frame, and the evidence for each. Addresses are av-cam.bin 1.10 (VA = PA − 0x40000000),
kernel 3.0.27 symbols (`re/kernel_syms.txt`), and modules by load address (table below).

## 1. Camera timing event block 0xF3C30000 (IRQ 83 / 84) — `hw/misc/cxd90014_evsel.c`
Task 65 (engine main, `43764494`) runs to completion (gdb confirmed every call returns). At the end it calls
`436AC4CC(29, 1, {cb = 4376C1E4})`, which **registers the engine frame routine on event source 29**, and then exits.
`436AC4CC` puts the entry into an 8-slot table and programs the hardware:

| off | meaning | evidence |
|---|---|---|
| +0x04 / +0x08 | group A status / W1C | IRQ 83 handler `436AC410` (`436AC7C8`/`436AC7A0`) |
| +0x0C / +0x10 | clear bit k on registration [?] / set bit k = channel active | `436AC784` |
| +0x20+4k | counter captured at event time → callback argument | `436AC7D4` |
| +0x40+4k | source ID of channel k | `436AC600` |
| +0x60+4n | per-source mode (always 1) | `436AC644` |
| +0xE0 | free-running counter | `436AC5F4` |
| +0xE4 / +0xE8 / +0xEC | 0 / 0x17 / run | `436AC3A4` → 24 MHz/(0x17+1) = **1 MHz** [hypothesis] |
| +0xF4 / +0xF8 / +0xFC | group B (10 comparators) status / enable (0x3FF) / W1C | IRQ 84 handler `436AC440` |
| +0x110+8k / +0x114+8k | comparator k arm(1)/disarm(0) / compare value | `436AC81C`; the handler disarms before the callback → one-shot |

Runtime values (gdb): engine config struct `0x43D87254` +0x24 = **0x8257 = 33367 µs (29.97 fps)**, +0x2C = 0x104A (1/8 frame).
This matches the 1 MHz hypothesis exactly. Source 29 → `4376C1E4` (frame start), 28 → `4376F181`, 30 → `4376C028`, 23 → `4376A07C`.
Which hardware signal each source ID corresponds to is unknown [?] → since there is no sensor, **all active channels fire once per engine frame period**.

## 2. Sub-microcontroller slot A permit flag 0x43D887AE
The engine tick `4376ABFC` calls `43A3DDC0` (sub-microcontroller transmit) only when the flag byte `0x43D887AE` is 1.
- `4376C1E4` (frame start): `AC(0x43D887AC) = byte_43D872D8 (=4)`, `AD = 0`, `AE = 0`, and
  `436AC384(1, capture + cfg[0x2C], 1, cb = 4376CD90)` → **schedules "int_HS" 1/8 frame later via a comparator**.
- `4376CD90` (int_HS): `AD++`; if `AD+1 >= AC` then `AE = 1`; reschedules itself up to 7 times.

→ With the comparator + µs counter model, `4376CD90` fires 16k times, `43A3DDC0` 2k times, and **CPU slot A exchange (`4369BED0`) 1.5k times**.

## 3. GIC SPI targets = bootloader values (RCU stall → umount stall → i1 stall)
Symptom: i1 (`THREAD_INFRA1`) in D state in kernel `sys_umount → rcu_barrier → mutex`; the boot script's
`umount` (pid 130) waits forever in `rcu_barrier → wait_for_completion`.
- Diagnosis: ran `show_state`/`sched_show_task` via gdb `call` and dumped the kernel `log_buf` (0x4037C760: len, buf; log_end 0x40395290).
- trace (`gic_acknowledge_irq`): **IRQ 29 (TWD) is not used**; tick timer IRQs 155/156/157 were all handled on CPU0.
- Sony `irq_select_affinity` (0x4004F0D8) **derives affinity from the current byte of GICD_ITARGETSR** (the kernel does not write targets).
  The boot stub routed all SPIs to CPU0, so CPU1–3 had no tick → RCU callbacks never ran.
- Fix: the boot stub writes the **ITARGETSR from the real-device GICD dump (`re/device/hwdump/GICD.BIN` +0x820)** as is
  (ticks 153/155/156/157 → CPU0/1/2/3, SYSV 98 · VPH 101/102 → CPU1, 83/84 → CPU2). Only zero bytes (unused on the real device) go to CPU0.
- Result: i0/i1/i2 all exit, `83 E wait` (gate A released), `094_main()` exits.

## 4. LDEC hardware LZ77 decoder (memory mode) — `hw/misc/cxd90014_ldec.c`
Symptom: `Appframework` gets SIGABRT → the crash handler SIGSTOPs every im.elf thread (confirmed by hooking kernel `send_signal`).
Preceding stderr: `error_code = 5` = failure (EIO) of `ioctl(/dev/ldec, 1)` in libObj `sub_5334CC`. Used to decompress UI image resources.
ldec.ko (initrd) analysis:
- Registers at 0xF2903000 (IRQ 171): +0x04 1=hold/0=start, +0x0C output page count, +0x10 output PA, +0x24 abort,
  +0x28/+0x2C/+0x30 = 0x7B/4/0x7B (init), the handler writes the status it read back to +0x30 (ack [?]), **+0x34 status 1=OK, 4=EIO**.
- Input: BOSS DMAC (ch 3, cfg 0x11100003) pushes the stream page by page into the **0xF0020000 window** (64 KiB, wrapping).
- Format = kernel lib/lz77 (same as the NAND ldec path). The hardware completion condition is unknown [?] → completion is signalled when
  "the received bytes end on a complete chunk and the output has reached the last page" (libObj sets pages = ceil(bytes/4096)).

## 5. Current status
- The UI stack (Rigel WindowServer `w=640 h=480 buffer num=2`, appFw) **is rendering frames**: GPU jobs (P3D/PPF/PSC) and
  completion IRQ 76 repeat, and PPF transfers to the 640×480 RGB565 buffers 0x8F758000/0x8F7EE000. Android (zygote, SurfaceFlinger) also starts.
- But the buffers are all zero: the UI draws with P3D (3D drawing), and **the model does not execute P3D commands, it only signals completion**.
- VPH AIF (ch51–58) is not programmed yet → why av-cam VDF does not open the panel OSD layer has not been investigated.

## Module load addresses (stock load order, identical every boot)
osal_utm 0x3F045000, **osal_uipc 0x3F049000**, utimer 0x3F056000, backup 0x3F05F000, osal_ulogio 0x3F072000,
input 0x3F07E000, udate 0x3F087000, indctr 0x3F08D000, i2c 0x3F092000, sircs 0x3F097000, dmm 0x3F0C7000,
upm 0x3F31A000, **grm_gles 0x3F346000** (sugilite_ioctl +0x8E8, ISR +0x644), hdmi 0x3F34B000, cec 0x3F372000,
mmc_drv 0x3F3DC000, liro 0x3F400000. (gdb `module_alloc_update_bounds` return value 0x4008E034 + matching the start of .text)

## 6. Where the panel output (VPH OSD) fails to open (further investigation)
Breaking in libObj user code: the libObj base differs on every run → derive it from kernel structures in the same gdb session
(use `find_task_by_vpid` to find comm "im.e" → task+0xCC = mm → mm+0 = first VMA; VMA {+4 start, +8 end, +12 next, +72 pgoff};
the VMA with size 0xE0E000 and pgoff 0 = libObj text).
- The Rigel flush `sub_5DB588` keeps getting called (32 times in 100 s), but `sub_5DC9FA` → `sub_5D4EE4` → `sub_5D3BEC` → `sub_5D3F0C`
  (= `sub_5D86EC(&unk_EEE630)`) returns 0 → "VDF not in run state" path: it sends only `vdf_if_port_send_break` (type 1) + `send_exec` (type 3)
  to osal 0x80003B (confirmed 42 times in the kernel) and **never sends `vdf_if_port_send_update` (OSD record)**.
- That state flag is updated by `sub_5D3A94` of the CmdQueue (`sub_5D4DF4`, receives 0x80003B) when it receives **DataflowInfra message id 4096** (`sub_5D4830`: 0x101/0x102/
  0x8100/0x8101/0x8102 …) → depends on the camera app's dataflow pin connection (shooting mode / live view start) [?].
  Likely tied to the sensor/ISP live view not being modelled (unconfirmed).
- So for pixels to reach the screen, both are needed: (a) P3D execution (b) VDF dataflow connection → OSD record → VPH AIF programming.

## 7. Signs that the app does not reach the "normal operation" state (further investigation, unresolved)
- The only sender to 0x80003B (libObj dmpNative CmdQueue) is im.elf itself (init types 2/4/6/8/9 + break/exec on every flush).
  The OSD pin connect (DataflowInfra id 4096) does not come from anywhere. 0x61003C = av-cam **AVBBC** command queue (libObj `ObjAvbbcCmn`).
- SD: card detect (0xF2915010 = 0x0C010000) and the SDHCI model exist. But `mcmnd_core_checkinsertedmedia` (mcmn_drv 0x3F3958C4)
  is never called → user space does not start media detection. Sub-microcontroller task 48 sends a front-media
  notify (0x28017C) once early in boot: payload `02 00 00 07 00 00 00` (byte3 = 7). The mcmn callback drops it if no user queue (dword_AD6C)
  is registered [?, it may have arrived before registration].
- No messages observed going from UPM to the im.elf power-state notify queue (0x920169, which im.elf registers via 0x920021/0x92016E).
  Only the UPM services (0x9241C2/0x9243D8 battery level, 0x924126 power state, 0x924223 heat) are updated, from task 48.
- Hypothesis: the app's startup state transition depends on boot reason / power state (snapshot values in the hand-off page: boot factor 7, loader mode 12) and
  on sub-microcontroller state. Next step = identify which event the appFw (Appframework) state machine is waiting for.
- Experiment: changing the hand-off page boot factor (0x8002104C) from 7 → 1 (the power-key path of upm `k_upm_setup_boot_factor`)
  (`A5K_HANDOFF=<file>`, run.sh) made no difference to VPH/SD/GPU behaviour.
- Set the appFw verbose log switch `byte_F13D88` (libObj, `testmode_debugPrint` → osal 0x8F0139 text message +20) to 1 in im.elf memory by calling kernel
  `access_process_vm` (0x400C5AB4) via gdb `call` (succeeded at both 26 s and 50 s) → afterwards
  **zero app log lines**: the Appframework main loop is idle in poll(), waiting for the next event (presumably a power-ON type event).
  (Procedure for writing another process's memory: `__kmalloc(16,0x20)` for a kernel buffer → `access_process_vm(task, va, kbuf, n, write)`.)

## 8. Correction + panel OSD brought up (hwtimer)
- gdb breakpoint `commands … c` drops the session with "target is running" when several vCPUs hit at once, so **events are missed**.
  Some of the "never sends / not registered" claims in §6 and §7 were wrong because of this. Tracing from here on uses the TCG plugin `tools/a5ktrace.c`
  (`-plugin tools/liba5ktrace.dylib,words=N,pc=0x…,r0=0x… -d plugin -D log`, build: README).
- Re-measured with the plugin: both the app (0x920168) and im.elf (0x920169) register with UPM → UPM active (0x92015C to 0x94016A) / ObjRenderer connects the OSD pin
  (0x290119) / 191 VDF responses / **Appframework calls `vdf_if_port_send_update` twice** (panel, 640×480, 16 bpp, PA 0x8F7EE000).
- However, the VPH task (task 43, `43904274`) runs `43AC2FD0` only twice and then stalls forever in `dly_tsk(11/12)` (`sub_4355D498`):
  liro `dly_tsk` → kernel `hwtimer_schedule_timeout` → the expiry callback `hwtimer_process_timeout` is never called.
- Cause: hwtimer0..3 (timer ch5..8, CTRL **0x1132**) are the kernel's per-CPU clock-plus-compare timers (`hwtimer_intr` reads +0xC to compare for expiry).
  The model reset the counter to 0 on a RELOAD (bit8) match, so the clock never reached the expiry time. Fixed as **bit5 = keep counting after a match** [hypothesis].
- Result: the VPH task runs every frame, **panel OSD layer configured** (0x144 = 0x03000232, ch53 = 0x8F7EE000 / 0x500 B/line / 480 lines).
  The only remaining cause of the black screen is the buffer contents (P3D not executed).

## 9. SD card detection (sub-microcontroller slot switch)
- mcmn_drv instrumentation (a5ktrace): the front-media notify (0x28017C) arrives before the user queue is registered, but `mcmnd_rm_setosalidformedianotify`
  resends the cached state on registration, so nothing is lost. Content `02 00 00 07 00 00 00` = all 3 slots "no card".
- Source: task 48's slot-A field extraction `439161D4`/`439162A8`/`4391637C` (slots 0/1/2) → state struct `byte_43E5581E`+25..27
  → `43916660` builds the state table (+18 valid mask, +19..21 slots) → `43916150` sends it to 0x28017C.
  Slot 0 = slot A **byte 2 bit4 = card present**, bit0 = second flag [? lock] (config mode ≤1 path; mode 2 uses byte 5).
- The snapshot frame has 0 → when `-drive if=sd` is present, the board sets that bit and recomputes the CRC (`a5k_submicro_card_inserted`).
  Result: libInfraMediaDetect → `checkinsertedmedia` → SDHCI card init (CMD0/8/41/2/3/9/7/16/51/6/30/42) → FAT reads (CMD17/18).
  No writes (CMD24/25) yet: waiting for key input in the first-boot "area/date/time setting" dialog.

## 10. After the first screen (in progress)
- GPU P3D execution (2 subagents: `RE_GPU_P3D_CMD.md`, `RE_GPU_P3D_SHADER.md`) + panel OSD RGBA4444 → the stock first-boot screen
  "Set area/date/time. [OK][Cancel]" is shown on QEMU console 0, looking the same as on the real device.
- Button front end (`a5k_key_event`): Enter → GPIO P10.10 low. Pressing "OK" makes the UI react and the screen changes.
  But the next frames contain only a PSC clear + PPF transfer, with no P3D drawing (blank screen) → before the next screen (area setting) is drawn,
  the app appears to be waiting for something else (mode transition completion, etc.) [?]. From the same point, polling of 0xF3C10xxx (CEC block) spikes.

## 11. Black screen after "OK" = button wiring error (the MOVIE button was pressed) → first-boot setup completed
### 11.1 Cause
The button front end in §10 attached the wrong meanings to the Sony key codes. GPIO P10.10, which host Enter pressed, is not the center button but
the **MOVIE (record start/stop) button**. So the first-boot dialog closed via the "Cancel" path, and the app went straight into shooting mode.
- The key names come from libObj's `{code, "KEY_..."}` table (from file offset 0xEE1078, 8-byte entries).
  0x0002 POWER, 0x0003 UP, 0x0015 LEFT, 0x0030 DOWN, **0x01AC ENTER**, 0x01AF RIGHT, **0x01AB MENU**,
  **0x01F0 STARTSTOP(MOVIE)**, 0x01F7 PLAY, 0x01F8 DELETE, 0x1BF SLIKEY_ZOOM, 0x70A6 PANEL_ARM_DET,
  0x7094 FLASH_POPUP, 0x70D5 UM_CONNECTOR_POWER.
- event0 codes follow the libInputServant table (0x3364, `{sony, linux}`). 0x03→103, 0x30→108, 0x1AF→106, 0x15→105,
  0x1AC→232, 0x1AB→514. The second table (0x3394) has 0x1F0→229, 0x1F1→102, 0x1AB→158.
- So mapping the input.ko key table (`RE_INPUT_I2C_POWER.md` §7) onto the actual buttons gives the following.
  ADC ch 0x0D: 0..91 **ENTER** / 92..273 UP / 274..439 LEFT / 440..599 DOWN / 600..772 RIGHT.
  ADC ch 0x0C: 0..91 DELETE. GPIO P10.10: MOVIE. GPIO P5.2: panel 180° rotation detect (active low).
  front CAkey1: bit0 POWER (active low), bit1 PLAY, bit2 MENU, bit5 UM_CONNECTOR_POWER, bit9 FLASH_POPUP (active low).
  The old mapping was LEFT=center, M=UP, RIGHT=LEFT, UP=DOWN, DOWN=RIGHT, Enter=MOVIE.
- App-side evidence (a5ktrace function tracing, §11.4) is as follows. Right after the key press, the M_CLOCK transition function `sub_40696C` received internal event 3 in state 4 and
  ran action 8 = `ActionSetForceSetting` (`sub_407254`). This action sets a default date and
  writes `setClockForceSettledToBackup` (Backup 0x1070140 = 1). After that, `[M_ROOT] getDateInfo() Clock Force Settled` is printed and
  `viewStlrec.uxc` opens (shooting screen). The area setting screen (`viewAreaSet.uxc`) never opened.

### 11.2 Fix (`hw/arm/sony_a5000.c` `a5k_key_event`)
| host key | hardware | Sony code |
|---|---|---|
| Enter / KP_Enter | ADC unit 1 ch 1 (=0x0D) level 45 | 0x1AC ENTER |
| ↑ / ← / ↓ / → | same channel 182 / 356 / 520 / 686 | 0x03 / 0x15 / 0x30 / 0x1AF |
| Delete / Backspace | ADC unit 1 ch 0 (=0x0C) 45 | 0x1F8 DELETE |
| R | GPIO P10.10 low | 0x1F0 MOVIE |
| M / P | sub-microcontroller slot A **byte 28 bit2 / bit1** (CRC recomputed) | 0x1AB MENU / 0x1F7 PLAY |

- Each ladder channel remembers which key is held. Releasing a different key does not return the level to 1023.
- Path for MENU and PLAY: the av-cam field parser `43AC73AE` reads b28 | b29<<8 of slot A and reflects it into task-48 state 0x43ED43B0+24 (CAkey1).
  The 0x280132 sync handler `43915890` (`43AC7408`) copies this value to msg[8..23], and input.ko polls it.
  The snapshot CAkey1 value is 0x0201. Confirmed that while M is held, 0x83ED43C8 changes to 0x0205, and `{0x1AB, 0, 1}` and `{0x1AB, 0, 0}` are delivered to 0x7A0020.
  Pressing MENU in the first dialog is handled like "Cancel" and moves on to the PlayMemories notice. This matches the real device.

### 11.3 First-boot setup progress (unmodified firmware, sendkey)
1. Dialog "Set area/date/time." → Enter → **area setting** (world map, "Tokyo/Seoul", GMT +9.0, 2014-01-01 00:00 AM)
2. Enter → **date/time setting** list (daylight saving off / date/time not set / date format Y-M-D / [OK])
3. ↓, Enter → date/time editor. Change it to 2026-11-14 with ↑/→ and press Enter, select [OK] with ↓↓, then Enter
   (when back in the list, the value to the right of "date/time" is blank. The RTC (sub-microcontroller clock) is not in the model yet [?])
4. **PlayMemories Home notice** ("www.sony.net/pm", [OK]) → Enter → shooting mode.
   - Note: the dialog is drawn before InputService `toActive`. Keys pressed within about 20 s after it appears are discarded.
     A `sendkey` sent while gdb has the VM stopped is also lost.
5. The shooting mode result depends on whether an SD card is present.
   - **No SD**: the warning "Check that the lens is attached … set 'Release w/o Lens' to 'Enable'…" appears. Since no lens is modelled, this matches the real device.
     Pressing Enter shows the **shooting screen OSD** (NO CARD, 16:9, 17M, RAW+J, 1080, battery 98%, SCN, ±0.0, ISO AUTO, grid lines).
     Live view is black (sensor/ISP not implemented). In this state the MENU event (0x7A0020 0x1AB) is delivered but the menu does not open.
     Wheel keys do not change the OSD either → presumably camera startup (monitoring start) has not finished, so input is blocked [?].
   - **With SD** (FAT32 formatted on macOS, no camera DB): the StlRec OSD is drawn for one frame. The media icon shows "ERROR";
     confirmed by dumping the panel buffer 0x8F7EE000. Then the root view receives event 0x13067054 and opens **view 12** on top of StlRec (blog: `type=4 ev=0x1c`).
     After about 250 widgets of the StlRec layout are destroyed (`Widget::~Widget`, lr 0x7027BF), the screen goes blank (only PSC fill + PPF).
     view 12 is presumed to be **AUTOREPAIR** (viewUnified8 `ViewAutoRepairToInstance`, automatic image DB repair) [?].
     Evidence: the registration order of the unified IdSoTable in viewUnified2. With this order, STLREC=131 and INITIAL_SETTING=61 match the observed values.
     The view appears to wait for an M_REPAIR response (estimated time, etc.) via `setModelRequest(true)`. After that, no SD command is issued at all
     (only CMD17/18/13/30 from boot, no CMD24/25). The CPU is idle → what response the repair model is waiting for is unconfirmed.
     Only the root view (103) receives keys, and it only arms a 10 s timer.

### 11.4 Tools (investigation methods, for the next person)
- Added options to `tools/a5ktrace.c`.
  - `str=N` / per-probe `s=N`: prints the string at r1+N (osal text message +20, `do_sys_open` file name +0).
  - `x=V`: r0 exclusion filter (repeatable). `comm=NAME`: task name prefix filter.
  - **`fn=PATH`, `fncap=N`**: a list of user-space function probes that is re-read at runtime (one `hex-address [label]` per line).
    When the file mtime changes, the plugin re-instruments via `qemu_plugin_reset`. It works even with all 70k libObj functions hooked.
- libObj base: in gdb, find comm "im.elf" with `find_task_by_vpid` (pid 100..200). Among its VMAs, the one with size 0xE0E000 and pgoff 0 is libObj.
  Write 1 to the appFw verbose log switch `byte_F13D88` with `access_process_vm`. Both can be done in one script.
  Verbose logs are carried at osal 0x8F0139 +20. Level 1 (error) is always printed regardless of the switch.
- appFw **blog** (osal 0x8F013A, message +0 = 0x00830002): +4 category, +8 length, +24 data.
  Category 4 = view/model event sequence (`blogEventSequence`: `{arg2, vm(1=view, 2=model), type, …, eventId BE, dest, queue}`;
  type 1=create, 2=terminate, 3=activate/deactivate, 4=push), 7 = TopologyManager, 14 = AppSystem main loop (sleep timeout, updateWidget, draw).
  `blogEventSequence` lets you follow the screen transition order directly.
- Once (run10: `-trace sdcard_*` + gdb `call`), a NULL dereference oops occurred in av-cam task 66 (`4376FA2x`, `ldr r0,[r0,#24]`).
  The system then hung with "scheduling while atomic". Repeating the same procedure without gdb did not reproduce it → presumably debugger interference [?].

### 11.5 Remaining work
- AUTOREPAIR stall with an SD card: trace the response path of the M_REPAIR → ObjSalvage/MprIf asynchronous commands (`[M_REPAIR] ActionGetRepairTime*`).
- Menu and wheel not responding in shooting mode: check whether a camera startup (CAMUSER_* / av-cam sensor path) completion event is required.
- Inputs: RTC (blank date display), zoom lever (ADC 0x0F multi-level), shutter (S1/S2 path not identified).

### 11.6 "↑ moves the highlight LEFT" in the stock MENU = screendump lag, not a mapping bug (verified 2026-11-16)
A report that a single `sendkey up` moved the main-menu highlight **left** (Setup → Playback) and that "input lags behind
screendumps" was investigated and traced to the observation method, not the device model. The 4-way ADC mapping in
`a5k_key_event` is correct.

- **Input path for menu navigation = the 4-way ADC ladder on ch 0x0D** (not the jog dial, not a separate arrow device).
  Each arrow writes one band mid-point into `cxd90014_adc_set(adc[1], ch 1, level)`: ↑ 182, ← 356, ↓ 520, → 686, centre 45.
  The band table (§11, `RE_INPUT_I2C_POWER.md` §7) is **firmware-verified** against the real-camera WBI snapshot and the
  user's `Backup.bin` (0x1120000): 92..273 → 0x03 UP, 274..439 → 0x15 LEFT, 440..599 → 0x30 DOWN, 600..772 → 0x1AF RIGHT.
  libInputServant (0x3364) then maps 0x03→103, 0x30→108, 0x15→105, 0x1AF→106. The `cxd90014_adc` model stores and returns
  `result[ch]` verbatim with no latch or sweep, so the level the board writes is exactly the value input.ko decodes — no
  place for a swap. (The control-wheel **rotation** `] / [` → event0 525/526 is a *separate* input, `RE_WHEEL_ZOOM_DATE.md`;
  it also scrolls the menu but is not what the arrows use.)
- **Stock MENU is a 3×2 tile grid** (not a horizontal tab bar): row0 Camera / Custom / Wireless, row1 Application /
  Playback / Setup. Navigation is an ordinary 2-D grid: ← → move within a row, ↑ ↓ move within a column (wrap at the
  edges).
- **In-emulator verification** (one key per screendump, ~450 ms hold, ~4 s settle before the dump): every transition is
  correct — Setup --←--> Playback --←--> Application --↑--> Camera --→--> Custom --↓--> Playback --→--> Setup --↑--> Wireless.
  8/8 transitions match a correct grid; in particular **↑ from Setup goes to Wireless (straight up), never to Playback**.
- **Root cause of the false report**: the whole input→av-cam→input.ko→im.elf→view→GPU-redraw pipeline lags by roughly one
  key (av-cam samples the ADC only every ~43 ms, input.ko debounces 4×10 ms, then the view redraws over several frames).
  When keys are fired back-to-back, a screendump taken right after `up` still shows the *previous* key's result. Sending a
  prior `left` (Setup→Playback) and then `up`, with the dump landing on the stale frame, reads exactly as "up moved left
  (Setup→Playback)". With one key at a time and an adequate settle the symptom disappears.
- **Conclusion / fix**: no change to `hw/arm/sony_a5000.c` — the mapping is faithful. The fix is methodological and is now in
  `README.md` (Keys): for scripted menu nav send one directional key at a time and wait before the screendump. Changing the
  ADC levels would instead *break* fidelity (they are the firmware band mid-points, and the first-boot date editor in §11.3
  already navigates correctly with them).

## 12. Shutter held since boot (MENU/PLAY ignored), shutter and zoom wiring, the SD-card repair stall, CEC W1C
### 12.1 Root cause of "MENU does nothing in shooting mode": S1/S2 read as pressed
- At every boot the av-cam key path posted `{0x1F1, 0, 1}` and `{0xA7, 0, 1}` to 0x7A0020 (a5ktrace on `osal_snd_msg`,
  r0=0x7A0020). 0x1F1 = `KEY_RELEASE_LOCK` (S1, half press) and 0xA7 = `KEY_RELEASE_CAP` (S2, full press). Names come
  from libObj's key-name table at 0xEE1078. The key→event table in viewUnified2.so at file 0x46A2F4 maps them to
  0x15010020 and 0x15010030. **The emulated camera booted with the shutter held fully down.**
- The shutter is not in input.ko's key table (Backup 0x1120000, 18 entries, dumped from `re/backup` → no S1/S2).
  av-cam reads it directly:
  - `436FCA80` (init): if sysparam 0x00F90139 == 0 and 0x00F9010A == 13, it uses IRQs 134/142/133/140. Otherwise it
    uses **122/142/132/134** (p0.1, p1.5, p0.11, p0.13 via `gpiopin_to_irq`). This firmware takes the second set,
    confirmed by the GPIO scan below.
  - The IRQ handlers `436FCB84…` mask their IRQ pair, capture all states (`436FCA64`) and queue an event for a task.
  - `436FC740(n)` reads the level of the pin named by sysparams (n=0: 0x00F90139/0x00F9010A, n=1:
    0x00F90140/0x00F90111) through `43AC774C(port, pin)` and returns **its inverse (active low)**.
  - `436FC780` re-reads after a debounce `dly` (a table value or 500).
- GPIO scan (new debug machine property `gpio-poke`, see 12.5). With the camera at the first-boot dialog, raising each
  candidate line from the old reset level 0 gave:
  - p0.1 → `{0x1F1, 0}` (S1 released)
  - p0.11 → an S1 event (the second S1 interrupt line)
  - p1.5 → `{0xA7, 0}` (S2 released)
  - p0.2, p0.5, p0.13 → nothing (no state change)

  Each switch therefore has a level pin plus a second interrupt line [? two lines per switch is inferred from the IRQ
  set, not from a schematic]: **S1 = p0.1 (+ p0.11), S2 = p1.5 (+ p0.13), active low**.
- Fix (`hw/gpio/cxd90014_gpio.c` reset): p0.1, p0.11, p0.13 and p1.5 idle high. After the fix the boot events are
  `{0x1F1, 0}` / `{0xA7, 0}`.
- Result (no SD): in shooting mode, Enter opens the shooting-mode selector (scene selection with the mode dial),
  MENU closes it, and MENU again opens the **main menu** (Camera settings / Custom / Wireless / Application / Playback /
  Setup). This answers the question in §11.5: the menu was **not** waiting for camera-core readiness (P2S, AIF
  1/4/5, IRQ 35-38 from RE_SENSOR_ISP.md). It was inhibited by the held shutter.
- How keys were routed before the fix: only ROOT_SELECTOR (view 103, `EventFilter::attachCategoryId(0x15000000)`,
  viewUnified2 0x175615) received MENU (0x15010112/0x15010111). Its event maps (`ActionEventMapBase::DispatchEvent`,
  libObj 0x340FF4) had no action for it. STLREC (131) received no key events at all. PLAY was dropped the same way.

### 12.2 Shutter and zoom lever (`hw/arm/sony_a5000.c` `a5k_key_event`)
| Host key | Hardware | Event at 0x7A0020 `{code, 0, value}` (verified) |
|---|---|---|
| `S` | p0.1 + p0.11 low | `{0x1F1,1}` … `{0x1F1,0}` |
| `Shift+S`, or `Space` | additionally p1.5 + p0.13 low | `{0x1F1,1}{0xA7,1}` … `{0x1F1,0}{0xA7,0}` |
| `T` / `Shift+T` | ADC unit 1 ch 3 (ch 0x0F) = 478 / 162 | `{0x1BF,1}` / `{0x1BF,2}` … `{0x1BF,0}` |
| `W` / `Shift+W` | same channel = 400 / 301 | `{0x1BF,0x8001}` / `{0x1BF,0x8002}` … `{0x1BF,0}` |

- Zoom: input.ko entries 6-10 (ch 0x0F, multi key 0x1BF): 80..244 → 2, 245..358 → 0x8002, 359..442 → 0x8001,
  443..514 → 1, 515..1023 → 0 (centre). Reading 1/2 = tele slow/fast and 0x8001/0x8002 = wide slow/fast
  [? bit 15 = wide is inferred from the values, not from the userland consumer].
- Not wired: control-wheel rotation (av-cam `JogDev`/`JogTpu` classes, a TPU counter, not yet located). The real-camera
  event0 note "half press = 516+517" does not match the tables: 0x15010010 → 516 comes from key 0x0002 (POWER) and
  0x1D2. 0x1F1 → 0x15010020 → 517, and 0xA7 → 0x15010030 → 518 (libInputServant 0x3304) [?].

### 12.3 SD card: the blank screen is AUTOREPAIR, which waits for a model that is never created
- Confirmed: a probe on libObj's `dlsym` thunk (0xDF0FC, string at r1) shows `ViewAutoRepairToInstance` resolved
  right after `ViewStlrecToInstance`.
- View IDs: the order of the viewUnified2 "unified" IdSoTable registrations matches the observed IDs (STLREC 131,
  INITIAL_SETTING 61, **AUTOREPAIR 12**, AREA_SET 5, ROOT 101, ROOT_POWER_SW 102, ROOT_SELECTOR 103). Model IDs in the
  blog are the hex in `@Mxxx` (e.g. `@M00B` = 11 Camera, `@M05E` = 94 Repair).
- AUTOREPAIR's setup (viewUnified8 `sub_1CCA0`) attaches `@M05E` (ModelRepair) as prepare slot 1 and `@M031`
  (YuvMute) as slot 6. `ViewBase::IsModelPrepare` → `getModelInfo(model, 0x7FFFFFFF)` stays false. After the view is
  created, the only model traffic is `requestModelExecute("@M031", 2)` (mute YUV). **`ModelRepairToInstance` is never
  resolved**: no `dlsym`, and no model-start event 0x11004005 for it in ModelManager `sub_705D60`. So the view chooses
  no layout and draws nothing.
- What else was ruled out:
  - sync messages: entry/exit tracing of `__osal_snd_sync_msg` shows no thread blocked in one; only a timer sleep
    0x910041 is pending.
  - SD I/O: none after boot (CMD13/17/18/30 only, never CMD24/25).
  - CPUs: idle.
  - card image: an SD-spec SDHC image (`tools/mksd.py`: MBR, FAT32, 4 MiB boundary unit, 32 KiB clusters, data area
    BU-aligned) triggers the same AUTOREPAIR.
- With the shutter fix, MENU works on top of the blank view. **Setup → Format → OK also hangs on the confirmation
  dialog with no SD command.** Setting slot A byte 2 bit0 ("second flag [? lock]") at run time changes nothing.
  Media write/repair operations stall before any I/O. Next step: trace the M_FORMAT / ObjCntMgr / MPR (libmpr.so,
  analysed on windev as `v2_libmpr`) path for the asynchronous request that is never answered.

### 12.4 CEC interrupt status is write-1-to-clear
- `hw/arm/sony_a5000.c`: `a5k_regfile_ex("cec", 0xF3C10000, 0x1000, NULL, "0x180=0xf", -500)`. In the regfile
  helper, "autoclear" bits written as 1 are not stored, which is W1C for a status nothing sets.
- Before: cec.ko saw a phantom RX-ready and libObj's CEC Msg Thread power-cycled CEC endlessly (RE_HDMI.md §4a).
- After: 942 CEC accesses in 20 s of host time, almost all reads of +0x180 (~47/s), and only a handful of writes. No
  set_power cycling.

### 12.5 Tools added
- a5ktrace:
  - `libobj=1|2`: finds libObj's base without a debugger. At the first `Appframework` hit it walks task→mm→VMAs; `2`
    also lists every file mapping.
  - `poke=OFF:VAL`: writes a byte at libObj+OFF from an im.elf thread.
  - fn-file directives: `+OFF` (libObj-relative probe), `!OFF VAL` (queued poke), `?` (re-list VMAs), `#cap N`
    (per-probe hit cap).
  - `fnwords=`, `fnstr=`, `fnr1s=1`: dump words at r0, the string at *(r0+4K), or the string at r1.
  - The task pointer `t=` is now printed, which lets you match sync-message entries and exits.
  - Do not set appFw's debug byte `F13D88` from boot (`poke=`): with it on that early, the first-boot dialog ignores
    keys. Set it later (fn-file `!F13D88 1`).
- Board debug properties (off unless set, for RE only): `qom-set /machine gpio-poke PORT.PIN=LEVEL` and
  `qom-set /machine submicro-poke IDX=VAL` (patches the replayed slot-A frame, recomputing its CRC). The slot-A bit scan
  (bytes 1-63) found key events only in bytes 28/29 (CAkey1: PLAY, MENU, UM_CONNECTOR_POWER, FLASH_POPUP).
- Trace event `cxd90014_gpio_in(port, value)` for pin-data reads. Polled ports are P0, P3, P5 and P10 only.
- `tools/mksd.py OUT.img [GiB]`: blank SD-spec SDHC image (written under `data/`, which is gitignored).

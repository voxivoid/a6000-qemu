# RE: what gates the first UI (OSD) frame — im.elf stage "a" → Rigel/EGL → dmpNative → VDF

Static RE of firmware 1.10 for the `sony-a5000` QEMU machine. Follows `RE_USERLAND_SUBCPU.md` §1.3 (im.elf stages),
`RE_DISPLAY_GPU.md` (GPU/rm driver) and `RE_DISPLAY_SCANOUT.md` §5 (Rigel → VDF). The sub-micro link is **not**
covered here (separate analysis).
Sources: `re/ida/libObj.so.c` (all of appFw/grm/gs/uxengine/Rigel/dmpNative/DMP-EGL is statically inside libObj.so),
`re/ida/im.elf.c` plus objdump of im.elf `main`, new Hex-Rays dumps in `re/ida/ui/` (below), and the av-cam VDF IDB.
Claims marked **[?]** are inferred and not proven.

New artifacts:

| file | content |
|---|---|
| `re/ida/ui/ui_<lib>.so.c` | Hex-Rays of every function of 31 stage-i/u libraries that are *not* in libObj: libScalarDaemon, libHapiLib, libInputServant, libInfraHdmi, libInfraMediaCommon, libInfraFsys, libInfraMediaMount/Detect, libmprctrl, libInfraCacheMgr, libInfraScalarMpr, libSyncAndroid, libsdcom, **dataflowInfraFramework** (DataflowInfraPin), arbiter, libUsb*, libNetService, libWlan*, … (IDBs on windev `C:\a5000\emu\ui\ui_*.so.i64`) |
| `re/ida/ui/idx.py`, `dem.py`, `blk.py` | index a Hex-Rays dump into a pickle, add demangled aliases, then BFS a call graph from an init function and list every potentially blocking primitive (`osal_wai_sem_tmo(..,-1)`, `osal_snd_sync_*`, `pthread_cond_wait`, `ioctl`, …) with the call path |
| `re/ida/ui/bl.py`, `xr.py`, `ptr.py`, `lit.py` | Thumb-2 BL/BLX/B.W xref index for libObj (Hex-Rays dump has no xrefs), pointer search, and "literal pool + call" listing of a function (recovers OSAL queue IDs that Hex-Rays drops from `osal_*` calls) |
| `re/ida/ui/ui_q.py` | IDA query runner (copy of `vdf_q.py`) on `C:\a5000\emu\ui\ui_avcam.i64` (copy of `vdf_avcam.i64`) |

---------------------------------------------------------------------------------------------------
## 0. Summary (read this first)

The first OSD frame is the end of a long chain. **Nothing in it is time-driven: the Rigel window server only
composes when a client window is dirty, and the only client that can be dirty at boot is the camera UI (appFw).**
So "no `vdf_if_port_send_update`" means that appFw has not drawn yet. It does **not** mean that the GPU or VDF is wrong.

```
im.elf main            i0 thread (grm, gs=Rigel WS, uxengine, InputServant, ScalarDaemon, HapiLib, …)  ──join──┐
  (sub_9ab8)           i1 thread (udate, InfraAvbbc, Media*, mprctrl, StillPool, Database, …, MediaPwrMgr)       │
                       i2 thread (USB*, HDMI, CEC, NFC, updater, Wlan*, NetService, gps…, CameraBackground,      │
                                  SyncAndroid)                                                                   │
  osal_wai_sem_tmo(0x83037A,-1)  ◀── main takes the "app" semaphore after i0 joins ─────────────────────────────┘
  start a_thread → appFw.so Application_System_Init → thread sub_702978:
        uxengine root window 640x480 (sub_4F03C8 → sub_503A30)
        AppSystem ctor sub_7031A8:  "B wait"  osal_wai_sem_tmo(0x83037A,-1)  "E wait"   ◀── GATE A
  main: join(i1), join(i2), osal_sig_sem(0x83037A)                                     ──▶ releases GATE A
  AppSystem::Main sub_702E3C loop: events → state machines (M_ROOT power-on …) → draw only if a widget is dirty
        sub_70CD64 "updateFrame / notifyDraw / drawFramebuffer" → ux gfxeng GL draw → Rigel WindowClient post
  Rigel WindowServer (gs, sub_67BBB8 loop on IPC "UX_WINDOW_SYSTEM") → WindowServerEGL compose (GLES)
        → eglSwapBuffers → DMP-EGL rm request type 6 (sub_5EBFA0):
             begin_swap (WindowImpl, takes UpdateManager lock)  ◀── GATE C (lock held until VDF ack of previous frame)
             P3D quad (0x1804/0x1800, wait IRQ bit31) and/or PPF transfer (0xC00.., wait bit30)
             end_swap sub_645BE0 → sub_644C80 → sub_5DDEEA → FbContainer::gen_struct_for_update (sub_5DD190)
             → UpdateManager sub_5DCC1C → vdf_if_port_send_update (sub_5DC3D4, osal 0x80003B type 5)
  dmpNative CmdQueue thread (queue 0x80003B) → InternalCmdUpdate::set_data (sub_5D7BD4)
        → OSDPin::send (sub_5D80EC) → DataflowInfraPin::Send → av-cam VDF OSD pin 0x59000E   ◀── GATE B (pin connected?)
  av-cam VDF SystemCmdPinSend::OsdPinExec (0x436731D8) → VPH OSD layer ch53 …
        → MSGID_PIN_SEND_COMP back to 0x80003B → unlock → next frame may begin               ◀── GATE C
```

The three gates that can block forever:

* **A: the app semaphore 0x83037A.** appFw cannot leave its constructor until **every** stage-i init in the i0, i1 and
  i2 threads has returned. One blocking `*_Init` (for example a sync request to an av-cam service whose task is stuck) means no
  UI at all, even though "D keyboard" (i0, row 49) and GLES (i0, rows 38/39) are already up.
* **B: the OSD dataflow pin.** The libObj OSD pin has to be connected to VDF's OSD pin `0x59000E`. The app's
  `ObjRenderer` initialize sequence requests the connection. VDF must complete the PIN_REQUEST_CONNECT/CONNECT
  handshake. If the pin is not connected, an update is dropped ("osd_pin send failed"). It does not hang, but nothing reaches ch53.
* **C: the per-frame VDF acknowledgement.** After each OSD update libObj keeps two locks (InternalCmd sem and UpdateManager
  mutex) **with no timeout** until VDF answers `MSGID_PIN_SEND_COMP`. Without it the *second* `eglSwapBuffers` blocks forever
  in `begin_swap`, and with it the Rigel WS thread and the whole UI.

**GPU:** neither the window server nor the EGL fences wait on GPU-written memory. All completion goes through
**GPU status register +0x34 bits 31/30/26 → IRQ 76 → grm_gles `int_status` → ioctl 0xC0048203**. EGL sync objects are
signalled by the CPU (`status = EGL_SIGNALED_KHR 0x30F2`) after that ioctl returns (§4). A missing completion costs
3000 jiffies plus a GPU reset per job, but it never blocks forever. The exception is the cross-process kick hand-off (§4.3).

---------------------------------------------------------------------------------------------------
## 1. im.elf stage gating (objdump of `main` @0x9ab8; Hex-Rays loses the IDs)

Relevant part of `main` (Thumb, addresses in im.elf):

```
9c26  bl sub_9A14(r9=&i0)      ; start i0_thread  (sub_94C8, tgt 1)
9c2c  bl sub_9A34(r8=&i1)      ; start i1_thread  (tgt 2)
9c32  bl sub_9A54(r5=&i2)      ; start i2_thread  (tgt 4)
9c3e  osal_wait_thread_exit(i0)
9c48  osal_wai_sem_tmo(0x0083037A, -1)      ; literal @0x9de4 — main grabs the app semaphore
9c4e  bl sub_99F4(&a)          ; start a_thread (sub_9474): rows 101..103 → appFw.so Application_System_Init
9c56  osal_wait_thread_exit(i1)
9c5c  osal_wait_thread_exit(i2)
9c62  osal_sig_sem(0x0083037A)              ; release appFw
9c6a  osal_wait_thread_exit(a)
9c72  Backup_read(...) ; warm counter ; osal_snd_sync_direct(cmd 0x920117) ; power-state loop sub_A06C
```

* The only other reference to 0x83037A in the whole firmware (all /usr/lib, /usr/bin, kmods, av-cam scanned) is
  libObj `sub_7031A8` (AppSystem constructor): `sub_702AC8(0,"wait"); osal_wai_sem_tmo(0x83037A,-1); sub_702AC8(1,"wait")`.
  The initial count must be 1 (static OSAL config). Otherwise main itself would dead-lock at 0x9c48.
* Rows run per thread (`sub_B63C`): i0 = rows 37–52 (CoreSync†, grm, gs, DDS, uxengine, Indctr, Tp, RsrcHotshoe, Hotshoe,
  Sircs, Dial, Lanc, InputServant, ScalarDaemon, HapiLib, Selector). i1 = tgt 2 rows (udate, InfraAvbbc, MediaConfig, mcmn,
  Fsys, EyeFi, MediaMount, MediaDetect, mprdummy/clema/mpr (no init), mprctrl, StillPool, Database, SalvageStatus,
  CacheMgr, CameraStillDate, dig_level, ScalarMpr, FuFsys, **MediaPwrMgr** (row 98)). i2 = tgt 4 rows (UsbBusPower, usb,
  sensapi, UsbControl, BusPower†, Hotplug, MtpAppDlSvr, HDMI, CEC, Airplane, NFC, udtr_senser, AdjMedia, WlanResource,
  WlanAdjust, NetService, NetDeviceInfo, WirelessPower†, gps_rsrc, gps, compass, geo, HdmiInput†, **CameraBackground**,
  **SyncAndroid**). Row 97 (type 6) does not stop the walk, so rows 98–100 belong to stage i.
  † no such file or symbol: `sub_B25C` returns −1/−2 and the row is skipped. psid≠0 rows dlopen `/usr/lib/libObj.so`.
* Each row is bracketed by boot-time profile messages: `sub_B63C` → `sub_ADFC(0/1,"Ii",me,file)`. That is an
  `osal_snd_sync_msg(0x8F013A)` of type 9 `"%4X B|E Ii %03x_<file>"`, served by **osal_ulogio.ko**, which forwards to the
  kernel's **/proc/snsc_boot_time** (`boot_time_add(const char*)` @0x4018eed8). Stage markers are `"Ib 094_<letter>"`
  and `"Ib 094_i0_thread"`. appFw adds `"  83 B AC <addr>"`, `"  83 B wait"` / `"  83 E wait"` (the gate-A wait).
  **The last "B" without a matching "E" names the blocking module exactly** (§7, P0).

---------------------------------------------------------------------------------------------------
## 2. appFw (Application_System_Init, libObj 0x7029E8)

* `Application_System_Init(cfg)` stores "AppConfig.so" and creates the AppSystem thread (`osal_create_thread` with
  descriptor @0xEE8D68: entry `sub_702978`, field 0x83 = appFw `me`) and returns. The im.elf a_thread does **not** wait for the UI.
* `sub_702978`: `sub_4F03C8()` (uxengine: `sub_503A30("",640,480,1,0,0x80000000,0)` = Porting_Musashi table
  `sub_534978`, screen create, `/tmp/area_check_*`). Static closure found no blocking primitive. Then `sub_7031A8` (ctor:
  config = `getConfig` after `initializeConfig()`, **gate A**, then EventManager / managers / boot event push) → `sub_702E3C` (main loop) → `sub_70332C`.
* Main loop `sub_702E3C`: `poll()` on an internal pipe (`sub_702B54`, woken by `EventManager::push`), dispatch, then, if
  `IsViewModeDiadem()` (`dword_F13D84==0`, the default) and `(flags & 8)==0 || sub_709790()`, run "updateWidget" and
  "draw" (`sub_70CD64`). `sub_4EFADE` posts a frame **only if the dirty region is non-empty** (`sub_66D442`), so the first
  frame needs a view or widget created by the app state machines (M_ROOT "ActionHandleToPowerOnEvent", boot-info and boot-factor
  getters). Which av-cam/UPM events the boot state machine needs before it shows its first view was not traced: it goes through
  table-driven state machines and vtables [?]. Inputs it reads are at least the UPM boot factor (sync 0x924127, upm.ko, from
  the loader hand-off page) and the Backup boot props.
* Per-iteration state strings ("sleep", "wake", "updateFrame", "draw in", "draw out") are written with a 0x910042 time stamp into
  a debug ring `dword_F13B14` (user memory of im.elf). It is only useful from a guest debugger.

---------------------------------------------------------------------------------------------------
## 3. Rigel window server (stage i0, `gs_init` → GraphicsServer::Init `sub_67C184`)

* `hal_create()` (ws_hal) must be 0 and `rglWindowServerInit({w=640,h=480,fmt=7,0x1000,bufnum=2,…})` (`sub_679D6C` →
  `sub_67AB68`) must succeed. **Either failure is `__assert_fail` → abort of im.elf.** Then `ABT_reg_resource()` and
  `bootComplete` registers sync queue **0x014B0355** (`sub_67C304`). Init creates the IPC objects "UX_WINDOW_SYSTEM" and
  "UX_DEBUG_CLIENT" and the window "UX_MAIN_WINDOW", and prints `[WindowServer] init : Initialize Window System... w = 640 h = 480
  buffer num = 2` to stderr (useful checkpoint).
* `grm_init` (row 38) prints "!!! GLES Vecgen disabled." and then **"===== DMP_2D_Initialize : %d"**. DMP_2D_Initialize
  (`sub_63FC10`) starts worker threads and `pthread_cond_wait`s for their hand-shake. A missing second line means i0 is stuck
  in the 2D worker start-up [?].
* The server loop `sub_67BBB8` blocks reading window messages from "UX_WINDOW_SYSTEM" and composes only on client messages
  (`sub_67BB00`). **It has no periodic refresh.**
* Caveat: `WindowServerEGL` `sub_67A53C` (FB-mode path) does `open("/dev/fb0")`; `ioctl(FBIOGET_VSCREENINFO)`. When
  that fails it asserts and calls `raise(SIGTRAP)`. On the real camera `/dev/fb0` is ENXIO, so this path is not taken in the
  normal mode [?]. **Do not add an fbdev to the kernel model.**

---------------------------------------------------------------------------------------------------
## 4. GPU waits — what the compositor and EGL actually wait on

### 4.1 Request executor `sub_5EA170` (rm request types)
| type | action | wait (`sub_5EA030` → ioctl 0xC0048203 with mask) |
|---|---|---|
| 1/7/8 | P3D list: `+0x1804 = addr`, `+0x1800 = size` (`sub_5E9F90`) | bit31 0x80000000 |
| 2 | PPF transfer `+0xC00…+0xC18` (`sub_5E9E00`), or CPU tiler `sub_5E92E0` for w/h < 32 | bit30 0x40000000 |
| 3 | PSC fill `+0x10..+0x1C` (ctrl default 0x0F010201) | bit26 0x04000000 |
| **6** | **window post = eglSwapBuffers back end (`sub_5EBFA0`)**: begin_swap, optional P3D quad (wait bit31), PPF transfer of the colour buffer into the native window buffer (wait bit30), end_swap → dmpNative update | bit31 and/or bit30 |
| 9/10 | bypass on/off (ioctl 15) | — |

* The ioctl returns the matched bits. If **bit24** is set (kernel 3000-jiffy timeout), `sub_5EBEF0` records the failure and the code
  carries on. **If the ioctl itself fails, `sub_5EA030` returns 0 and `sub_5EA170` *re-kicks the same request in a loop*
  (`if (v3==0) continue`)**, so the ioctl must never return an error.
* After the wait, the executor walks the request's sync list (`a1[10]`, `a1[11]`): `sync->status = 0x30F2` (EGL_SIGNALED_KHR)
  + cond broadcast. It then calls the completion callbacks `a1[7]`/`a1[8]`. `eglClientWaitSyncKHR` (`sub_5F1A60`, from the
  proc-address table @0xE821D4) only waits on that pthread cond. **There is no GPU-written fence or timestamp anywhere.**
  The P3D kick is gated by `byte_E819F0` (initial value 1 in .data → enabled). PSC dummy-write is off (ioctl 0x63 = 0).

### 4.2 What the GPU model must provide (consolidated from RE_DISPLAY_GPU §2.2, confirmed by the code above)
* `+0x34` status: set bit31 after a write to `+0x1800`, bit30 after `+0xC18` (trigger), bit26 after `+0x1C` bit0. Raise
  **IRQ 76 level-high** while `+0x34 != 0`. **Never assert IRQ 76 with +0x34 == 0** (`BUG()` in `sugilite_interrupt`).
  The ISR acks by writing `+0x1000=0` / `+0xC18=0` / `+0x1C=0`, which must clear the matching status bit and drop the line.
* For a *visible* frame the PPF transfer must really copy (8×8-tiled → linear, formats, `>>2` addresses): the window
  post always ends with a PPF from the GL colour buffer into the dmpNative window buffer. P3D can stay a no-op for the first
  frame. The picture is then black or stale, but the update is still sent.

### 4.3 Cross-process kick queue (possible stall) [?]
The rm queue lives in the grm_gles shared page (mmap 0x20000) and is shared by im.elf and Android surfaceflinger
(libDMPSUGIRM, same driver). Kick thread `sub_5E9120`: if the head request belongs to another pid it sends that pid an event
(ioctl 0xC004820E) and sleeps on its own (ioctl 0xC004820D, 10 000 000 jiffies). If the owning process never runs its kick thread,
the queue stalls. ioctl 14 returning 0x1FFFFFFF (no such pid) drops the request (`sub_5EB280`). So the per-pid
event/semaphore ioctls (12–14, 17/18) must be implemented faithfully by the stock grm_gles.ko. They are, but this needs both CPUs
(Android runs pinned to CPU3) to be scheduled.

---------------------------------------------------------------------------------------------------
## 5. dmpNative → VDF (UpdateManager, vdf_if, OSD pin)

### 5.1 Objects
* **UpdateManager** (singleton `sub_5D695C`), mutex at `+8`. Two FbContainers at `+88` (panel) and `+200` (line) [?].
  The callbacks registered in `sub_5DC8C0` are update-done `sub_5DCBD8`, mute-done `sub_5DC97A` and `sub_5DCB3C`.
* **vdf_if** (`vdf_if.cpp`): four OSAL semaphores `EEF154` (break), `EEF160` (exec), `EEF15C` (update), `EEF14C` (mute),
  created in `vdf_if_port_init` [initial count 1 ?]. A send takes its semaphore (`-1` timeout) and posts a message (type 1/3/5/7)
  to the dmpNative CmdQueue queue **0x80003B**. The "done" message (type 2/4/6/8) signals the semaphore and calls the callback.
* **InternalCmd** objects at CmdQueue `+8/+68/+128/+196` (break/exec/update/mute), each with a lock semaphore (`+16`)
  and a pending slot (`sub_5D34B4`, `abort()` "slot %d didnt empty" if a second request comes before completion).
* **OSDPin** (`OSDPin.cpp`, object `unk_EEE630`, DataflowInfraPin at `+4`). Pin state `+40`: 0 = connected, 1 = disconnected,
  2 = connecting. A send while not connected returns 0 and logs "osd_pin send failed". The completion is then run locally,
  so there is no hang.

### 5.2 Frame N sequence (libObj addresses)
1. `begin_swap` (`sub_5DD3DC`, WindowImpl.cpp) takes and releases the UpdateManager mutex. **It blocks while frame N-1 is still
   unacknowledged.**
2. `end_swap` → `sub_5DD33C`: `gen_struct_for_update` per valid container (32-byte `update_osd_record`, see
   RE_DISPLAY_SCANOUT §5), then `sub_5DCC1C`. That function **takes the mutex and keeps it**. If anything changed
   (`sub_5DCAB4`: first frame, count change, address change, or Backup 0x01700000 bit0 = 1 "always"; the real value is 0) it calls
   `vdf_if_port_send_update(rec, n≤2)`. Otherwise it sends "exec" (`sub_5DCBE4`, still needs the VDF ack).
3. CmdQueue `sub_5D3DEC` type 5 → `InternalCmdUpdate::set_data` (`sub_5D7BD4`) builds `C_DataflowInfra_StdData_OSD` /
   `Entry_OsdImageSize` (address, sizes, target offset/size, sel = bpp 2/4, out-target panel/line, channel, trans-type
   "yuv trans" (bit4, only 32 bpp; `/tmp/dn_trans_type` override), read-orientation 1) in the shared stddata memory (assert
   `mem.is_mapped()`). Then `sub_5D7F20` takes the InternalCmd lock, does `OSDPin::send` (`DataflowInfraPin::Send` →
   `SendASync` to the **target pin's osal id 0x59000E**, 0x34-byte MSG_PIN_SEND with the stddata pointer at +40), and arms the pending slot.
4. VDF answers **MSGID_PIN_SEND_COMP** to 0x80003B. `sub_5D4364` matches on the word at msg+40: `0x10000000` → slot 2,
   `0x10000001` → slot 0, `0x10000002` → slot 1, `0x10000003` → slot 3 [? meaning of the word, mapping to update/exec/break/mute].
   The slot handler is `sub_5D7B9C`: it unlocks InternalCmd and calls the done callback (`sub_5DC100`), which signals `EEF15C` and calls
   `sub_5DCB48`. That function moves the "pending" records to "current", **releases the UpdateManager mutex**, and frame N+1 may begin.
   **No timeout exists on this path.**

### 5.3 Who connects the OSD pin (gate B)
* `CmdProcessorVdf::proc_request_connect` (`sub_5D4328` → `OSDPin::requestConnect` `sub_5D820C`) accepts only
  src = {type PIN_OSD(1), dir INOUT(3)} and dst = {PIN_OSD, INOUT, osalId & 0xFFFF0000 == **0x00590000**} (VDF).
  `DataflowInfraPin::RequestConnect` then sends MSG_PIN_CONNECT to the VDF pin and enters state 2. VDF must reply CONNECT_COMP
  (`OSDPin::connectComp` `sub_5D83F0`), after which the state is 0 (connected).
* The request originates in the app business layer: **`[ObjRenderer]SequenceCmnInitializeCmd` / `SequenceCmnResumeInitializeCmd`**
  (libObj ~0x1C0E7C / ~0x1C3xxx) build an MWF ObjMsg param 0xE001 = `{1 (OSD), 3 (INOUT), no, 0x59000E}` and execute
  connect command 3 through the Avbbc command executor [?]. So the pin is connected only after appFw has started and created
  ObjRenderer. **Before gate A opens, the OSD pin cannot be connected.**
* av-cam side (VDF, `ui_avcam.i64`): `VDF::SystemCmdPinRequestConnect::Execute` 0x43672AA0, `SystemCmdPinConnect::Execute`
  0x436729BC, `SystemCmdPinSend::OsdPinExec` 0x436731D8 → `CallOsdVdfCommand` 0x4367308C (runs the VDF OSD command lists
  `dword_43EDAACC[0xBC1/0xB23/0x1BA6]`), vtable of `VDF::SystemCmdPinSendComp` @0x43EA4050. Whether SEND_COMP is sent
  right after OsdPinExec or from the VPH OSD done-callback (descriptor +48, i.e. after a vsync) is not decoded [?]. Assume the
  latter, so **IRQ 101/102 must keep running** (they do now).

---------------------------------------------------------------------------------------------------
## 6. Blocking inventory of the stage-i inits (static closure, depth 5–10, Hex-Rays text)

Method: `blk.py` BFS from each init. The queue IDs come from `lit.py` (Hex-Rays drops them). Hex-Rays loses
`osal_create_thread` entry points, so work done in spawned threads is *not* included. That work does not hold the stage. Indirect calls (vtables)
are not followed. "kernel" = served synchronously by a kernel module, so the call only blocks if that module blocks.

| thread | init | blocking calls found | served by | verdict |
|---|---|---|---|---|
| i0 | grm_init | DMP_2D_Initialize cond-waits (own threads); `sync_direct 0x910042` | utimer | ok if 2D threads run |
| i0 | gs_init | WS init (asserts), EGL init GPU jobs (ioctl waits, 30 s timeout) | grm_gles | ok with GPU model |
| i0 | DDSUserInit | `sync_direct 0x910040` ×3 (alarm timer) | utimer | ok |
| i0 | uxengine, Lanc, Selector, Dial, Tp, Indctr | `0x910042` time stamps only | utimer | ok |
| i0 | infra_hotshoe / infra_sircs | `osal_wai_sem_tmo(0x7C80E6/0x7C80E7 / 0x7D80DE,-1)` own mutexes, sync to own threads | self | ok [?] |
| i0 | ScalarDaemon_Init | none (registers parameters, ArbiterObserver, InputServantObserver, DMM queue 0x015408C8 cb) | — | ok |
| i0 | HapiLib_Init | async logs, creates 3 threads, `Stm_Init` | — | ok |
| i1 | udate_init | `osal_wai_sem_tmo(0x908199,-1)` (own), **`sync_direct 0x90013D` cmd 2 (get clock status)**, `sync_direct 0x90013F` (notify reg) | udate.ko (writes go on to av-cam RTC 0x900140) | ok if udate.ko answers from its cache [?] |
| i1 | InfraMediaConfig_Init, EyeFi, StillPool, Database, CameraStillDate, AdjMedia | `0x910042` only | utimer | ok |
| i1 | InfraSalvageStatus_Init | `sync_msg 0x924127` (boot factor) | upm.ko | ok |
| i1 | **InfraMprCtrl_Init** (libmprctrl) | `sync_msg 0x924126` (power status), `0x9241C2` (battery level), `0x924223` (heat), **`0x4B024E` (updater, registered by i2 `udtr_senser_init`!)** via on-demand load `0x940021/0x940022` to im.elf `0x94016A`, `0xCF016D` (MediaConfig GetMediaInfo) | upm.ko / im.elf callback / libObj | **candidate**: a race with i2 (send to a not-yet-registered queue: fails fast per RE_INPUT §11 [?]). UPM values come from the sub-micro cache |
| i1 | mcmn_init, InfraFsys, MediaMount, MediaDetect, CacheMgr, ScalarMpr, FuFsys | none found (constructors and command registration) | — | ok [?] (mcmn_drv_init opens /dev/mcmn) |
| i1 | InfraMediaPwrMgr_init | own `SemLock` | self | ok |
| i2 | senser_init, CameraBackground_init | `sync_msg 0x924127` | upm.ko | ok |
| i2 | Hotplug, CEC, gps, geo, compass | 0x910042, own sems; CEC init joins a thread it created | self | ok [?] |
| i2 | gps_init | `sync_msg 0x1088262` (gps rsrc) | libgps_rsrc (i2, earlier row) | ok if gps_rsrc_init ran |
| i2 | netservice_init | `NsApiSendWlanResourceSyncMsg` → WlanResource queue | libWlanResource (i2, earlier row) | ok [?] |
| i2 | UsbControl_Init | `UsbControl::PreInit` sync msg [?id] | usb kmods | [?] |
| i2 | HDMI_init, Airplane, NFC, Wlan*, NetDeviceInfo, gps_rsrc, SyncAndroid, MtpAppDlSvr | none found | — | ok [?] |

Conclusion: **static analysis does not show a stage-i init that is guaranteed to block on av-cam.** Every direct sync target
is a kernel module (utimer, upm, udate, ulogio) or a userland queue. The remaining risks are (a) UPM/udate handlers that
might wait on the av-cam sub-micro services (0x2802xx, 0x900140) inside the kernel, (b) the i1/i2 ordering race on 0x4B024E, and (c) code
reached through vtables (MWF/Avbbc C++ objects, e.g. `InfraAvbbc_Init`). The boot-time log (§7 P0) resolves this in one run.

---------------------------------------------------------------------------------------------------
## 7. Prioritised emulator requirements for the first OSD frame

| P | requirement | concrete values | why |
|---|---|---|---|
| **0** | **Checkpoint log**: hook kernel `boot_time_add` @0x4018EED8 (gdb `b *0x4018eed8` + `x/s $r0`, or a TCG plugin), or `cat /proc/snsc_boot_time` from the console shell | look for `Ii <me>_<file>` B without E; `  83 B wait` / `  83 E wait`; stderr lines `===== DMP_2D_Initialize : 0` and `[WindowServer] init : Initialize Window System...` | tells which of gates A/B/C, or which init, is the blocker. No guessing needed |
| **1** | **All stage-i inits must return** (gate A) | whatever P0 shows is blocking. Most likely an OSAL sync reply from av-cam / sub-micro via a kernel module (UPM 0x9241xx/0x9242xx, udate 0x90013D→0x900140) [?] | appFw is held on sem 0x83037A until i0, i1 and i2 all join |
| **2** | GPU completion model (exists): `+0x34` bits 31/30/26 on 0x1800/0xC18/0x1C writes, IRQ 76 level, cleared by the ISR acks; **never IRQ with +0x34==0**; ioctl must not fail | §4.2 | otherwise each GL job waits 3000 jiffies and resets the GPU; an ioctl error leads to an infinite re-kick |
| **3** | **PPF display transfer must copy** (tiled→linear, RGBA8/565, `>>2` addresses) | RE_DISPLAY_GPU §2.3 | the window post ends with a PPF into the dmpNative buffer whose PA goes to VDF |
| **4** | av-cam VDF alive with OSD pin 0x59000E registered, answering **PIN_REQUEST_CONNECT / CONNECT → CONNECT_COMP** | §5.3 | gate B. Without it updates are dropped silently (log "osd_pin send failed", "requestConnect failed: %d") |
| **5** | VDF answers every OSD send with **MSGID_PIN_SEND_COMP** (msg+40 = 0x10000000 [?]) | §5.2 step 4; probably after VPH applies the layer at vsync (IRQ 101/102) [?] | gate C. Without it the second `eglSwapBuffers` (and the whole UI thread) blocks forever |
| 6 | VPH/AIF model latches ch53 on vsync (RE_DISPLAY_SCANOUT §6) | 0x144 bit1, ch53 +0x20/+0x24/+0x2C/+0x48, 0x404 bit0 | turns the update into pixels |
| 7 | Keep both GPU clients schedulable (Android on CPU3) | §4.3 | the cross-process rm queue hand-off can otherwise stall [?] |
| 8 | Backup props read during this path must exist: **0x01700000** (4 B, dmpNative setting, `__assert_fail` if the read size ≠ 4), 0x3E0003/0x3E0009 (grm_gles, BUG), UPM boot-factor inputs 0x51000F/0x510013/0x510014, 0xFA0029 → 0x8002104C | values from the user's Backup.bin (0x01700000 = 0) | asserts or a wrong boot type |
| 9 | Do **not** provide `/dev/fb0` | §3 caveat | the FB-mode path would assert and SIGTRAP im.elf [?] |

---------------------------------------------------------------------------------------------------
## 8. Open items [?]
* Which events or state the appFw boot state machine (M_ROOT and the AppConfig states) needs before the first view is dirty:
  power-on from UPM, av-cam "camera ready", lens or battery status. Needs either the state tables or a run with P0.
* Exact meaning of the 32-bit word at MSG_PIN_SEND_COMP+40 (0x10000000..3) and where VDF emits SEND_COMP (OsdPinExec
  return vs VPH done-callback).
* Whether `udate_kif_setget(cmd 2)` and the UPM 0x924126/0x9241C2/0x924223 handlers answer without contacting av-cam.
* Whether `InfraAvbbc_Init` and the MWF/Avbbc objects created in stage i make blocking sync calls through vtables. Static closure
  through Hex-Rays text found none.
* Initial counts of the vdf_if semaphores and of 0x83037A (static OSAL config tables in osal_uipc.ko).

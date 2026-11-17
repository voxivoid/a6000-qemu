# SD card: blank AUTOREPAIR screen and the Format hang (fw 1.10)

Scope: why, with an SD card inserted, the camera ends on a blank AUTOREPAIR view and Setup → Format → OK hangs.
The work is dynamic (TCG plugin probes, guest RAM dumps) plus static (local Hex-Rays dumps in `../re/ida`, IDA on
windev for `av-cam.bin`). Facts carry their evidence. Guesses are marked **[?]**.

Addresses: `libObj+X` = offset in `/usr/lib/libObj.so` (IDA address, base 0). av-cam addresses are absolute kernel VAs
(load address 0x43541000, no relocation).

---------------------------------------------------------------------------------------------------------------------

## 0. Summary

| Question | Answer |
|---|---|
| Root cause | **Not SD hardware and not a media/database service.** Both symptoms come from the app's **ModelManager**, which hangs half way through a model-tree switch. Leaving the shooting tree means tearing down **ModelCamera (`@M00B`)**, and that needs the camera engine to confirm "EE (live view) stopped". The engine never confirms, because a handshake between the camera-user layer and **av-cam** never completes. Which handshake fails depends on the run (§3). |
| Repair model | `ModelRepairToInstance` is never called, because the ModelManager never gets to create anything new. The request for `@M05E` reaches the ModelManager (`MM_create(0x5E)`), but the manager is stuck in its switch state machine (§2). |
| Format | Opening Format requests model `@M017` (ModelFormat). That request is queued behind the same stuck switch, so **ModelFormat is never created**. The confirmation dialog therefore has no model to send `ObjCntMgr` commands to. No SD command is ever issued, because none was ever requested (§4). |
| Same cause? | **Yes.** It is one stuck ModelManager. AUTOREPAIR is only the first model switch the SD case asks for. |
| Same stall without SD? | **Yes.** With no card, PLAY also sends `EE_STOP_CMD`, av-cam receives `0x4081`, and `EE_STOP_CMP` never arrives (§5). The SD card only makes the problem visible right after boot. |
| Hardware or software? | It is the **camera-engine side of the emulator** (av-cam / SA DSP / camera mode sequencer). It is not the SD host, card detect, DMA or sub-micro media bits. Two concrete missing completions were observed (§3): (A) the **SA DSP "function result"** for ZIT (face detection) never arrives, so av-cam's event task blocks; (B) av-cam never answers the camera-mode request **`0x3007`**. (A) is clearly missing hardware (the SA cores are not modelled). (B) is still open **[?]**. |
| "ERROR" in the first frame | Not resolved. **[?]** It is most likely the media-state icon shown while the media model reports "needs repair" or "not ready", not an SD probe failure: SD init and the FAT reads succeed (RE_ENGINE_TICK §9), and the same card formatted to SD spec gives the same result. See §6. |

The SD controller, the card-detect word, the sub-micro slot bits and the card image are **not** the cause. No change to
them is needed for this problem.

---------------------------------------------------------------------------------------------------------------------

## 1. Method (so the next person can repeat it)

* Runs use a clone of the pristine NAND plus `tools/mksd.py` 4 GiB image. The build at 21:00 (`7f4fd60`, RTC, boots
  straight to shooting) gives r6–r14. The repository build with the camera front end (`2144956`, P2S/Pig/AIF models)
  gives r15–r18. Both behave the same way for this problem.
* The plugin is a private copy of `tools/a5ktrace.c`, built in the scratch directory; the repository was not changed.
  It adds:
  * `lib.so+OFF label` probe lines (library bases taken from the im.elf VMA list when libObj is discovered);
  * `w1=` / `w2=` dumps of the words at r1 / r2;
  * a comm field;
  * per-probe caps;
  * `#m NAME` marks.

  Worth upstreaming into `tools/a5ktrace.c` **[suggestion]**. One gotcha: user-mode code cannot read kernel VAs from the
  translate callback, so VMA names must be resolved inside a kernel-mode hit.
* **Offline state from RAM dumps.** HMP `pmemsave 0x80000000 0x20000000` dumps guest RAM. Kernel VA = PA − 0x40000000.
  Useful layout of this kernel:

  | Structure | Field | Offset |
  |---|---|---|
  | `init_task` | address | 0x4036D7C0 (from `__ksymtab_init_task`) |
  | `task_struct` | `stack` | +0x08 |
  | | `tasks` | +0xB0 |
  | | `pid` | +0x104 |
  | | `thread_group` | +0x15C |
  | | `mm` | +0xCC |
  | | `comm` | +0x1E8 |
  | `mm` | `pgd` | +0x24 |
  | `vma` | `vm_file` | +76 |
  | `file` | dentry | +12 |
  | dentry | name pointer | +56 |
  | `thread_info` | saved kernel sp / pc | +60 / +64 |
  | user `pt_regs` | location | `stack` + 0x2000 − 80 |

  Walking each thread's kernel and user stacks shows where it blocks. This works for blocking points that a
  sync-message trace cannot see (pthread condvars, `osal_rcv_msg_tmo`, liro waits).
* av-cam on windev: an own copy, `C:\a5000\emu\sdrepair\gavcam.i64` (copied from `gfx_avcam.bin.i64`; the
  `v2_avcam.i64` copy did not open).

---------------------------------------------------------------------------------------------------------------------

## 2. Why the repair model is never created

### 2.1 ModelManager, briefly (libObj)
* `View::requestModel` `libObj+70CB38` → `sub_70A780` posts event **0x11004002**, but only if the requesting view holds
  model control. Model control (`vm+92`) belongs to the topmost view whose `setModelRequest` byte (+28) is set
  (`sub_70A2xx`, `vm.c` lines around "Lc Layer").
* The ModelManager dispatcher `sub_70859C` handles 0x11004002 in `sub_708546(mgr, id)`:
  1. it queues the id in `mgr+14*4`;
  2. it sets `mgr[41] = mgr[42] = 0x40000`;
  3. it runs the switch state machine `sub_707A74`.
* The model tree is built from nodes `{+4 id, +8 model, +16 parent, +20 prev, +24 next, +28 child}`. The root is
  `*(*(mgr+12)+4)`. A switch walks from the current node `mgr[8]` to the target `mgr[9]`. For each model it leaves, it:
  1. sends **0x11004007** (stop) to that model instance (`sub_706CE4`);
  2. waits in sub-state **0x40201** (`sub_706EF0`) until the instance state drops to ≤ 0x10000.

  Instance states (`sub_705D60`): 0x10000 created, 0x30000 starting, 0x40000 active (sub-state 1..4), 0x20000 stopping.
  An instance in 0x20000 leaves that state only on **0x11004008** ("stop done"), which the model itself must post.
* Model ids (from the IdSoTable `sub_2F26C4`):

  | Id | Model |
  |---|---|
  | 0x0B | Camera |
  | 0x0F | StillRec |
  | 0x10 | Browser |
  | 0x5A | StillRec |
  | 0x5E | Repair |
  | 0x17 | Format |
  | 0xDC | RecPbNeutral |
  | 0xF00 | RecPbMix |

### 2.2 What happens with a card (run r4, RAM dump `ram4`)
1. AUTOREPAIR (`viewUnified8 sub_1CCA0`) calls `setModelRequest(1)` and `ViewBase::requestModel("@M05E")`.
   `VM_requestModel(0x5E)` and `MM_create(0x5E)` both fire. The request **is** delivered. (The previous note "no model
   traffic" only probed `dlsym`.)
2. The manager walks the tree. It stops and restarts the sibling models (0x11004007 / 0x11004008 pairs), then stops
   **ModelCamera**.
3. Manager dump: `mgr[41] = 0x40000`, `mgr[42] = 0x40201`, `mgr[13] = 0x5E` (pending), target node = 0x5E (it exists
   in the tree), node being left = 0xDE → model **0x0B (Camera)**. The Camera instance is in state **0x20000**
   (stopping). The manager waits forever for its 0x11004008.

In another timing (r10) the model being waited on is 0x0F (StillRec), stuck in 0x40000 sub-state 2..4 (busy). That
model is also waiting on a camera-engine reply (§3 A).

### 2.3 Why ModelCamera never finishes stopping
On 0x11004007, ModelCamera (`ModelCameraToInstance libObj+3E03C0`, object 0x27B8 bytes):
1. `ActionExeEeStop` `libObj+3DD708` sends **ObjCamera `MSGID_EE_STOP_CMD` = cat 0x3100 / msg 0x7504** (async, via
   `sub_3D4618`).
2. `pvt_UpdateEEStopCounter` `libObj+3CDD70` increments `m_eeStopCounter` (`obj+9960`).
3. The guard `sub_3C4C80` (`counter > 0`) moves the model to state **"EtryStpW"** (entry-stop wait, `libObj+3C82AC`
   log).

The counter is decremented only by `ActionObjectCameraEeStopCmp` `libObj+3CDDD4`, which runs on
**`MSGID_EE_STOP_CMP` (0x3100 / 0x7505)**. That handler never runs (probed in r5, r12, r13, r15, r17, r18).

---------------------------------------------------------------------------------------------------------------------

## 3. The chain below ObjCamera, and the two observed failure points

### 3.1 How `EE_STOP_CMD` flows

**Userland.** CamMgr (thread `CAMUSER_CAMMANA`, dispatcher `sub_8607B8` for msg 0x7504) creates
**`CamUser::SequenceEEStop`** (ctor `libObj+986464`). Its step 1 `sub_986250` does the following, in this order:
1. sends av-cam command **0x4081** (`sub_880BB4` + `sub_89423C` → `osal_snd_msg(0x00030001, {0x40810005, seq, …})`);
2. optionally sends 0x40C6;
3. calls **`BackgroundCmd::SndStart` `libObj+849318`**. This allocates a dynamic queue, sends `{16, 32, …}` to queue
   **0x0102021F** (the CameraBackground task), then **blocks in `osal_rcv_msg_tmo` until `{17, 32}` comes back**;
4. only then completes the sequence (`sub_981C18` → `EE_STOP_CMP`) and sends pin message 0x2001 / 0x1001.

**Messages to av-cam.** Every command goes to av-cam queue **0x00030001** as `{(cmd << 16) | 0x0005, seq, param…}`.
The reply is `{(cmd << 16) | xx01, seq, 0xF000 = OK, …}` and arrives in `sub_85F500`. Examples observed:
`0x40490001`, `0x401b5101`, `0x40f60a01`, `0x40670001`.

**av-cam dispatch.** The av-cam command table is at **0x43EAFDBC** (218 × 12-byte `{cmd, handler, flag}`), plus a
second one at 0x43EB2480. Relevant entries:
* `0x4067` → `0x43763018`;
* `0x4081` → `0x43A522FE` (RelMgr event 5);
* `0x4049` → `0x436FC388`;
* `0x4045` → `0x43A521A8`;
* `0x404A` → `0x43A522AC`.

**CameraBackground.** The CameraBackground task (`sub_17A9A0` loop on 0x0102021F, handler `sub_17A760`, object
`libObj+EEBB80`, state at +4) answers `{16, 32}` in states 1–5 with `{17, 32}` (`sub_17A25C`, sent to the sender
queue). In state 1 it first calls **`sub_17A0E8(obj, 0x3007)`**, which:
* sends `{0, 0x3007}` to **av-cam queue 0x00030001** (literal at `libObj+17A1C8`);
* **waits with infinite timeout** (`osal_rcv_msg_tmo(q, &m, -1)` at `libObj+17A152`) for the av-cam reply
  `{…, 0x3007, …, 0xF000}`.

### 3.2 Failure point B (r12, r15, r17): av-cam never answers `0x3007`
* Trace order (r17):
  1. `EE_STOP_issue`;
  2. `CamSend 0x40810005 seq 0x8000005B`;
  3. av-cam `0x43A522FE` runs (RelMgr event 5);
  4. `BGCmdStart`;
  5. `BG_handle {16, 32}` arrives in the CameraBackground task;
  6. **no `BG_reply`**.
* RAM r15 / r17:
  * CameraBackground's state is **3**. The state-1 path sets it to 3 *before* calling `sub_17A0E8`.
  * Its thread is blocked in `osal_rcv_msg_tmo` called from `libObj+17A152`.
  * `CAMUSER_CAMMANA` is blocked in `osal_rcv_msg_tmo` inside `BackgroundCmd::SndStart` (`libObj+8493A1`).
* So the chain is blocked on **av-cam's reply to `0x3007`**. In av-cam, `0x30xx` events go through a state × event
  table at **0x43EC6420…** (16-byte `{state, event, fn, event}`; states seen 0, 1, 2, 0x20, 0x59, …; xref `437D9C94`).
  The `0x3007` handlers (`437D9225`, `437D9231`, `437D9051`, `437D90B5`, `437D97A1`, …) only return sequence
  descriptors (`&dword_43B71A44`, …), so the reply is sent later by the sequence engine that runs those descriptors.
  What that engine waits for (frame / VD event, YC output, SA, …) is **not decoded [?]**.
* av-cam's RelMgr gives no reply for `0x4081` either. This is probably normal: the CamUser sequence completes from
  the Background handshake, not from a `0x4081` reply.

### 3.3 Failure point A (r7, r8, r10, r16, r18): av-cam never answers `0x4067` (SetSettingLoad)
* Earlier in boot, CamMgr runs **`SequenceSetSettingLoad`** (ctor `libObj+9A92FC`, step `sub_9A9270` sends **0x4067**).
  It waits for `0x40670001`. While it waits, the CamMgr sequence queue is occupied, so the later `EE_STOP_CMD` is
  never even turned into a SequenceEEStop. In these runs `SeqEEStop1` never ran.
* The av-cam handler `0x43763018` builds the reply `{1, 0, 0x4067, seq, 0xF000}` at `0x43D7FF5C`. It then posts the
  callback `0x43767DC0` to the event-driven task ("EvtDrvnTsk", object `0x43D849CC`, liro **tid 90**,
  `liro-kliro_90`) through `sub_438948B4`. The callback sends the reply (`sub_43767204`).
* In the failing runs the callback never runs. The RAM stack of `liro-kliro_90` (r18) is:

  ```
  4355DA43 (liro API wait)  ← 4398EA5B sub_4398EA3C  "SA Function Result Receive …"
                            ← 4398ECD7 sub_4398EC9C   ZIT_SaOpen / ZIT_SaStart / ZIT_SaClose
                            ← 43990C81, 4398F38F, 4398DDC5 (sub_4398DBFC)  ← 43A4A9F5 task loop
  ```

  The event task is **blocked in the ZIT (face detection) code, waiting for an SA DSP "function result"**:
  * result ids 33 = open complete, 34 = load, 35, 65 = "SA Function Start Done";
  * error strings `!!!ZIT ERROR!!!:: SA Function …`.

  Every later event for that task (including the `0x4067` reply) is starved.
* In the runs that reach failure point B (r12, r15, r17), `liro-kliro_90` is idle (`4355D68B ← 43A96E73 ← 43894983`).
  So A and B are independent. Which one happens first depends on timing: whether ZIT starts an SA function before
  SetSettingLoad.

### 3.4 Classification
* **A is missing hardware**:
  * The SA DSP cores (0xF34A0000 / 0xF34B0000 / 0xF34C0000 / 0xF35C0000, IRQ 51–56 / 61–66; RE_AVCAM_IRQ_BOOT §1.1,
    RE_USERLAND_SUBCPU §3) are not modelled.
  * The SA manager (av-cam task 44, `rcv_msg_tmo(0x101)`) never receives the mailbox interrupt that turns into result
    message 0x930101 `{8, 0xC1, src, regs…}`.
  * The same SA gap is already known to block LPS and movie recording (RE_AUDIO §5).
* **B is undecided [?]**. It is either a further camera-pipeline completion (the EE sequencer probably runs on VD and
  needs the YC output of RE_SENSOR_ISP §7.3, which never happens), or another SA dependency. Front-end modelling alone
  (P2S, Pig, AIF in `2144956`) does **not** fix it: r15 used that build, and its engine log is clean except
  `Error : GetImagerTemperature`.

---------------------------------------------------------------------------------------------------------------------

## 4. Format path (run r10, probes after entering Setup → Format)

1. Entering the Format dialog causes `VM_requestModel(0x17)`, then `MM_create(0x17)`, then `MM_reconcile`. The
   reconcile returns at once because `mgr[42]` is still in a wait state (`0x40101` in r10, `0x40201` in r4).
2. `ModelFormatToInstance` (`libObj+464534`) and `FMT_init` (`sub_4647FC`) never run.
3. Pressing OK therefore reaches no M_FORMAT action. **No SD command can be issued**.

Had the model existed, the chain would be:
1. M_FORMAT `sub_464A4C` issues `ObjMsg(0x3700, 0x100A)` (GET_FORMAT_TIME) and `sub_464B14` issues
   `ObjMsg(0x3700, 0x1022)` (FORMAT), both `IssueCommandAsync` to the ObjCntMgr instance (`sub_31EA74`).
2. Completion arrives as `MSGID_GET_FORMAT_TIME_CMP` / `MSGID_FORMAT_CMP` (`sub_464F0C`, `sub_464E24`), with a 1 s
   progress timer (`sub_389936(…, 7, 1000)`).
3. ObjCntMgr runs libmpr scenario `MPR_SCN_FORMAT.so` (MprIf / MMgr threads, raw access to `/dev/mmca*`).

That chain was not exercised.

---------------------------------------------------------------------------------------------------------------------

## 5. Control experiment without a card (r13)
1. On the shooting screen, PLAY produces `VM_requestModel(0xE5)`, then `EE_STOP_issue` (ModelCamera), then
   `SeqEEStop1`, then av-cam `0x4081` …, and **no `EE_STOP_CMP`**.
2. The "No memory card / cannot play back" view appears. PLAY again returns to shooting.

So the missing EE-stop completion exists without SD as well. Any feature that has to stop live view is affected
(playback with media, menu items that change the model tree, USB, …) **[?] beyond the cases tested**.

---------------------------------------------------------------------------------------------------------------------

## 6. The "ERROR" media icon and why AUTOREPAIR opens (open)
* AUTOREPAIR is opened by `DefRootView::ActionOpenRepair` (`viewUnified2 sub_160108`, vtable 0x413CC8), which only
  calls `openView("view/AUTOREPAIR")`. Its trigger is the root event 0x13067054 (RE_ENGINE_TICK §11.3).
* M_MEDIA exposes `PARAMID_IS_NEED_REPAIR` ("[M_MEDIA] testGetIsNeedRepair"). The mount-side fields
  `needRepairContent`, `needRepairAvi` and `inhNeedRepairAVCHD` (libObj strings) are what set it **[?] not traced**.
* A blank, PC- or `mksd.py`-formatted card has no Sony image database (`AVF_INFO`, `PRIVATE/…`). "Need repair → build
  the database" is plausibly the real camera's behaviour for such a card **[?]**. The one-frame "ERROR" icon is then
  the media state shown before the repair finishes **[?]**.
* Next step if this matters: probe `[M_MEDIA]` / ObjMedia param 0x… sets. Alternatively, put a card image taken from a
  real a5000 into the emulator: if it does not trigger AUTOREPAIR, the database check is confirmed.

---------------------------------------------------------------------------------------------------------------------

## 7. Spec for the code agent

### 7.1 What not to do
Do not change the SD host (F_SDH30), `0xF2915010` CD/WP, the sub-micro slot-A media bits or `mksd.py` for this problem.
Do not add any software shortcut that answers `EE_STOP` or `0x4067`. That would be firmware-level HLE of av-cam, which
AI_WORKFLOW §1 forbids.

### 7.2 Gate A: SA DSP "function result" (required)
Model the SA cores well enough that an SA **open / load / start** issued by av-cam gets its completion.

| Item | Value | Evidence |
|---|---|---|
| Register blocks | 0xF34A0000, 0xF34B0000, 0xF34C0000, 0xF35C0000; per core +0x000 control, +0x100 mailbox / IRQ, +0x200 | av-cam constants, RE_USERLAND_SUBCPU §3 |
| Clock / reset | 0xF3400000 set / clear pairs | same |
| IRQs | 51–53 = SA0 A/B/C, 54–56 = SA1, 61–63 = SA2, 64–66 = SA3. Source A = +0x00/+04/+08, B = +0x40/+44/+48, C = +0x100/+104/+108. The ISR (`4391A4C4…4391A880`) reads the bytes, writes them back to ack, and posts `{8, 0xC1, src#, regs…}` (msg id 0x930101) to SA manager task 44 | RE_AVCAM_IRQ_BOOT §1.1 |
| Result consumer | ZIT `sub_4398EA3C` waits (liro API at `4355DA30`) for a result record whose word 1 = expected result id (33 open complete, 34 load complete with `+8 != 0`, 35, 65 "start done" with `+0xA == 1`) and whose words 5–7 are function code / status / error (0 = OK) | Hex-Rays `sub_4398EA3C` |
| Kick → result | **[?] not decoded.** Next RE step: decompile `sub_4398EC10` (ZIT_SaOpen), `sub_43AE1E94` (ZIT_SaStart), `sub_4398EB98` (ZIT_SaClose) and the SA driver they call. That gives the mailbox write sequence and the exact result layout the IRQ path must deliver. The `cmpr` driver gives one known SA protocol to compare against: descriptor in work RAM; `regs+0x30 = codePA \| 0x80000000`; `+0x8 = 0x80000000`; `+0x0 = 1`; mailbox `+0x10/+0x14`; `+0x4/+0x8 = 0xFF`; go = `regs+4 = 1` | RE_USERLAND_SUBCPU §3 |
| Minimal model **[?]** | On a "go" write, after a short delay, write a result record into the core's mailbox registers that echoes the request with status OK, and raise source C (+0x100) of that core. The program itself (sabin, unknown ISA) is not executed, so the result data (face list, …) is empty. ZIT only needs the open / start / close handshake | inference |

Acceptance (probes from §8):
* `liro-kliro_90` never parks in `4398EA5B`;
* `AV_4067cb` runs for every `AV_4067`;
* `SeqSetSettingLoad2` runs.

### 7.3 Gate B: av-cam's reply to camera-mode request `0x3007` (required, needs more RE)
* Requester: CameraBackground `sub_17A0E8(obj, 0x3007)` → queue 0x00030001, infinite wait.
* av-cam side: the 0x30xx state × event table at 0x43EC6420; handlers return sequence descriptors.
* **Next RE step:**
  1. find the engine that executes those descriptors (xref of `0x43EC6420` at `437D9C94`) and the place where it
     sends the `0x3007` reply;
  2. probe it at run time to see which step it stops at;
  3. then name the hardware event it needs, probably a VD-synchronous EE state change that needs the YC path of
     RE_SENSOR_ISP §7.3, or an SA ack as in gate A **[?]**.

Acceptance:
* `BG_reply(32)` follows `BG_handle {16, 32}`;
* `BGCmdStart` returns;
* `EE_STOP_cmp` (`libObj+3CDDD4`) fires;
* ModelCamera reaches 0x10000;
* `ModelRepairToInstance` (`libObj+54EB68`) runs and AUTOREPAIR shows its progress layout.

### 7.4 Final acceptance for this issue
1. With an SD card: AUTOREPAIR runs, then either returns to shooting or shows the DB result.
2. Setup → Format → OK issues CMD24/25 to the card.
3. Without a card: PLAY → `EE_STOP_CMP` arrives.

---------------------------------------------------------------------------------------------------------------------

## 8. Probe set (private plugin syntax; addresses for fw 1.10)
```
libObj.so+705d60 MM_trans       (r1 = instance {id, ?, state, sub}, r2 = Event {.., id})
libObj.so+70a780 VM_requestModel (r1 = model id)
libObj.so+708546 MM_create      libObj.so+707a74 MM_reconcile
libObj.so+3dd708 EE_STOP_issue  libObj.so+3cddd4 EE_STOP_cmp  libObj.so+3cdd70 EE_counter
libObj.so+3c82ac CAM_EtryStpW   libObj.so+54eb68 ModelRepairToInstance
libObj.so+986250 SeqEEStop1     libObj.so+9a92fc SeqSetSettingLoad_ctor  libObj.so+9a90a8 SeqSetSettingLoad2
libObj.so+849318 BGCmdStart     libObj.so+17a760 BG_handle  libObj.so+17a25c BG_reply
libObj.so+89423c CamSend (w1 = command to av-cam)   libObj.so+85f500 CamRecv (w1 = av-cam reply)
43a522fe AV_4081  43763018 AV_4067  43767dc0 AV_4067cb  43767204 AV_4067reply  438948b4 AV_EvtPost
43894848 AV_err (r0 = format string in av-cam.bin, file offset r0 − 0x43541000)
```
ModelManager object: r0 of `MM_trans`. State: words 8 / 9 / 11 / 13 / 41 / 42.

---------------------------------------------------------------------------------------------------------------------

## 9. Open questions
1. Gate B: which step of the av-cam 0x30xx sequencer withholds the `0x3007` reply, and what hardware event it waits for.
2. Gate A: the exact SA mailbox request / result layout. Is a generic "ack every SA function with OK" enough for ZIT,
   or does ZIT check returned data?
3. Whether AUTOREPAIR on a blank card is real-camera behaviour, and where the "ERROR" media icon comes from (§6).
4. Run-to-run variation: why the ZIT SA open sometimes runs before SetSettingLoad (timing of the face-detect task
   start) **[?]**.
5. Once both gates pass: the libmpr format / repair scenarios (MprIf, MMgr, `/dev/mmca*`, DFS) are the next layer. Not
   yet exercised.
6. Stability: one run with the 21:00 build oopsed in av-cam (`liro-kliro_100`, PC `0x4396BFA6`, NULL + 0x78) with the
   plugin loaded. It is the same family as RE_SENSOR_ISP §7.4 and RE_ENGINE_TICK §11.4 **[?]**.

## 10. Implementation status (null SA)
Gate A is closed by the null SA model (RE_SA_DSP §5/§9): `liro-kliro_90` no longer parks in `4398EA5B`, and
`AV_4067cb` runs for every `AV_4067`. **Gate B also passes now**: after `BGCmdStart`, `BG_reply(32)` arrives and
`EE_STOP_cmp` (`libObj+3CDDD4`) fires, so it depended on the imager / mode sequencer, not on separate hardware.
Acceptance (§7.4) as of this change:
1. With a card: `VM_requestModel(0x5E)` → `EE_STOP_issue` → `BG_reply` → `EE_STOP_cmp` → **`ModelRepairToInstance`
   runs**. The screen then stays black and no write command reaches the card (only CMD17/18 reads). AUTOREPAIR does
   not complete and MENU does not respond, so Format could not be reached. Next layer: the repair model / libmpr
   scenario (§9 q5), not investigated.
2. Format → CMD24/25: not reached (see 1).
3. Without a card: PLAY → `EE_STOP_CMP` arrives (seen in 2 runs within about 15 s of PLAY; in 2 other runs it had not
   arrived about 40 s after PLAY when they were stopped). The "no memory card" playback view appeared in one run. In
   another the screen stayed black for more than 2 min after `EE_STOP_CMP` and PLAY was ignored (sequencer on step
   array `0x43B71526`, all modules matched). Before the SA model, the no-card view appeared without `EE_STOP_CMP`.

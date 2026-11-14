# RE: why the panel YUV live view is never requested (fw 1.10)

Follows `RE_SENSOR_ISP.md` §7.3. Method: static RE of `av-cam.bin` with IDA 9.4 + Hex-Rays (a copy of
`C:\a5000\emu\sensor\camfe.i64` at `C:\a5000\emu\ycreq\yc.i64`). Dynamic runs used the unmodified firmware on the
frozen QEMU build with pristine NAND clones, `a5ktrace fn=` probes, `-d unimp`, HMP `xp` of guest RAM, and one
diagnostic gdb write (§3). Addresses are av-cam VAs (PA = VA + 0x40000000) unless they are MMIO (0xF…).
`[?]` marks a claim that is not proven.

---

## 0. TL;DR

* **Nobody "writes" offset 604.** Byte `0x43D874F4` (block `0x43D87298` + 604) is **byte 4 of a constant ROM table
  entry**. Each VD_Pre copies the 8-byte entry `CS_Command` table `0x43B24F00[state]` into `0x43D874F0`. `state` is the
  low byte of the **target state of sequencer module 1** (camera module 1, `0x43DC7120`). The YC-output bits 1/2 are set
  only in "monitoring" states such as 0x02/0x03/0x04. The emulator stays in state 0x00/0x01 (flags 0x40) or 0x1A
  (flags 0x00).
* There is no app-side command and no queue id behind it. The app sent only one sync request at boot, `0x3003`, and it
  was accepted. The firmware's own **mode sequencer** (`s_mode_sequencer`, 13 modules) runs the resulting 43-step
  sequence. It **stops at step 33**, `WAIT mods {4,5,6,10}`, because **module 10 stays at 0x82 while its target is
  0x83**. Step 34 would set module 1 to 0x02 (flags 0x03, YC output 0 requested).
* Module 10 is the imager-control sequencer (`0x43ED6D60…`). It refuses request 0x83 while its async job is busy
  (`0x43ED6D67 = 1`). That job is the imager start job. Its step 1 converts the sensor defect table with
  **ZIT ConvZ1C**, which runs on **SA DSP core 0 (0xF34A0000)**. av-cam programs the core and writes "go"
  (`+0x004 = 1`), then waits forever for the "calc done" mailbox reply. That reply only comes from the **SA0 source-A
  interrupt, IRQ 51**. The emulator has no SA model (RAZ/WI), so the reply never comes.
* **Proven by experiment.** Clearing the busy byte by hand (gdb) made the sequencer finish at once. Module 1 went to
  0x02, `0x43D874F4` became 0x03, and YC output 0 (`dword_43D8AF48[0]`) became 1 (requested).
* **Root cause:** a missing SA-core completion (IRQ 51 plus a mailbox reply), §4. After that the next gate is
  YC state 2, which needs the EE sequence `43546E40` to run (§5, partly open).

---

## 1. The byte at offset 604 is a table lookup, not a command field

| fact | evidence |
|---|---|
| VD_Pre `4376CFDC` does `word_43D86D18[0..12] = target states` (`sub_437D86D4(i)` = `*(u16*)(0x43DC7120 + 2i)`) and then `sub_437DBC7C(word_43D86D18 & 0xFF, &dword_43D874F0)` | disasm `4376d388..4376d394` (`LDRH R0,[word_43D86D18]`; Hex-Rays shows the argument as 0, which is wrong) |
| `sub_437DBC7C/80` (`CS_Command.cpp:112`): `state < 0x50`; copy 8 bytes from `0x43B24F00 + 8*state` | decompile; "FATAL! %s:%d" when `state >= 0x50` |
| VD_Post `4376FC48` reads `0x43D874F4` (`a3[2]+604`): bit1 → output 0, bit2 → outputs 1/2, gated by bit7 and mode state `*(0x43D87254+0x10)` ∈ {4,5} | decompile (as in RE_SENSOR_ISP §7.3) |
| also read by HS_Sequence path `4376F8D8` when mode state ∈ {4,5}: `(b & 0x82)==0x82` → out 0, `(b & 0x84)==0x84` → out 1/2 | decompile |

Flag byte (entry byte 4) per state, from `0x43B24F00` in `av-cam.bin` ("YC" = bit1 or bit2 set):

```
00,01:40   02:03 YC   03,04:02 YC   05:00   06:80   07:82 YC   09:8e YC   0b:02 YC  0c:22 YC  0d:02 YC
0e,0f,10:00   11:22 YC   12,13:02 YC   16:22 YC   1a:00   1b:80   1c,1d:02 YC   20..2e: 02/14/16 YC
32:02 YC   38..3a:0e YC   3b..3d:02 YC   3e:00   3f:02 YC   41..47: 02/0e YC   48,49:06 YC   4a:00
```

Run time (HMP `xp`, stalled boot): target `0x43DC7120` = `0x011A`, so state 0x1A. `0x43D874F0` = `4000b24a 00000000`,
the table entry for 0x1A (flag 0x00). Earlier runs that read 0x40 were in state 0x00/0x01, the target set by
"Liro Start" `43764494` (`sub_437D87C4(0, 0x100)`).

## 2. The mode sequencer and where it stops

### 2.1 Two state machines (CS_Command / CamMain)
* **Path selector** `dword_43DC70E8` (`s_path_selector`). Table `0x43EC6420`, 249 × `{cur, req, fn, next}`.
  Requests come from `CAM_SyncRequest_Wrapper 4375C920` (`wrapper/CamMain.cpp`) → `437D8560` (logs "Can't Transfer
  Request cur_state[%d] req[%d]" when refused). The request codes are 0x30xx. The wrappers `43A5BB60…43A5BD44` each
  send one constant (0x3000, 0x3001, 0x3003, 0x3006, 0x300A, …). Applied every frame in VD_Pre by `437D85D8`.
* **Mode sequencer** `dword_43DC7104` (`s_mode_sequencer`): `{+0 table 0x43DC71A0 (250 entries), +8 nmod=13,
  +12 active, +16 step array, +20 nsteps, +24 step idx, +28 target[13] (u16 per module = id<<8 | state),
  +92 actual[13]}`. A step word without bit 15 sets one module's target. A word with bit 15 is a barrier: it waits
  until `target == actual` for every module whose bit is set in the low 13 bits (`CheckReturnAllModlueResponse
  437D8810`). The sequencer advances in HS_Main `4376ABFC` → `437D8708` → `43A70EE0`.
* HS_Main also fills `actual[]` every frame (`437D86F4`). For module 10 that value is `*(u16*)*(0x43D84A88)` =
  `0x43DA212C`. This is `0x0A00 | imager_seq_state`, mapped through table `0x43B6F09C` in `437A3F40`.

### 2.2 Run time
* `syncreq` probe: the boot sends exactly **one** request, `0x3003` (LR `4375ACF7`), and it is accepted
  (`437D8560`). After that only `can_req(0x3024)` checks appear. No further request is needed to reach monitoring:
  the 0x3003 path already contains the monitoring steps.
* Path selector after 0x3003: `0x43DC70E4` = 2 (path value; `437D85D8` logs event 0x2001 for path 2/0x3F),
  path array `0x43B71EB0`. Mode sequencer: step array `0x43B709C6`, 43 steps, **index 33**:

```
 0 0105 0285 0320 0400 0500 0600 0780 0910 0a10 0b00 0c01 0d00   12 WAIT{1,2,3,7,9,10}
13 0281 0323 0480 0702 0920 0a11                               19 WAIT{2,3,10}
20 0a82                                                        21 WAIT{10,12}
22 011a 0282 0325 0a83 0c02                                    27 WAIT{1,3}
28 0326                                                        29 WAIT{2,3,4}
30 0481 0580 0680                                              33 WAIT{4,5,6,10}   <== stuck: module 10 actual 0x82, target 0x83
34 0102 0202 0302 0405 0502 0602 0a00 0b10                     42 WAIT{1..7,9,12}
```

  `target[]` = `011a 0282 0326 0481 0580 0680 0702 0000 0920 0a83 0b00 0c02 0d00`; `actual[]` matches except for
  module 10 (`0a82`). This is the same in 3/3 runs where the imager powered on.

## 3. Module 10 = imager-control sequencer; its job blocks on the SA DSP

### 3.1 Imager sequencer (`0x4398xxxx`, globals `0x43ED6D60…`)
* `sub_4398E1B8(obj, task, req)` (called from `437A3F40`) returns the current state `word_43ED6D62`. It takes
  request `word_43ED6D64`. `0x43ED6D67` = **busy** (async job running) and `0x43ED6D68` = "accept request".
  - req 0x10 → `4398E168` posts job `4398DB88` → `4398DB8E` (imager power-on / AsyncInitial, see RE_SENSOR_ISP §1.1).
  - req 0x82 → `sub_43AE1892`, `sub_43990650`, then `4398E118(0, task 0x43D849CC)`: sets busy=1 and posts
    `4398DBFC` (step-table job, code 1).
  - req 0x83 (`4398E30E`): **if busy → `0x43ED6D68 = 0`, so the request is not taken**. Otherwise current = 0x83.
* `4398DBFC` is a step-table interpreter. The code→list table is at `0x43ED6D7C`. Code 1 → list `0x43ED6E60` =
  `{4398F274, 0xFF}, {4398F28C, 1}, {0}`. A step returning 0 goes to the next step, 1 jumps to `entry.arg`, and any
  other value waits. The job reposts itself (`4398E0E4`) while the status is ≤ 0xF. On completion it clears busy
  through `*off_43ED6D58`.
* Step `4398F28C`: when the defect "shadow" buffer (`0x43E1F964`) holds points and its format is 4 →
  `Copy_IntegratedBuffer 4398D428` → **`ZIT ConvZ1C 43990B2C`** → `ZitSaFunctionDriver` `4398EC9C`:
  `ZIT_SaOpen 4398EC10` (cmd 0x21, waits for results 0x21 and 0x22) → `ZIT_SaStart 43AE1E94` (`4398E870`, cmd 0x41
  "CalcStart", waits for result 0x41) → `ZIT_SaClose 4398EB98` (cmd 0x23).
* Waiting is `4398EA3C` → `rcv_mbx(tmp mbx, &msg, TMO_FEVR)` (`sub_4355DA30(.., -1)`). There is **no timeout**.

### 3.2 Run time (r3, r4)
```
step1_1 4398F28C → convz1c 43990B2C → zit_func → sa_open → sa_cmd(0x21) → mq_send(0x101, 20 B)
sa_wait(0x21) ok → sa_wait(0x22) ok → sa_start → sa_calc → mq_send(0x101, 28 B) → sa_wait(0x41)   … never returns
```
`0x43ED6D60` = `00820000 01010083` (current 0x82, request 0x83, step 1, **busy 1**). Job struct `0x43ED6D44` =
`{0, code 1, idx 1, list 0x43ED6E60}`. Task `0x43D849CC` is blocked inside the job.

### 3.3 MMIO trace of the calc start (`-d unimp`, right after `sa_wait(0x41)`)
```
wr 0xF34002B0 = 0x11            SA0 reset release      (clk/rst block 0xF3400000, as in cmpr: rst +0x2B0)
wr 0xF34000B0 = 0x5             SA0 clock on           (clk +0xB0)
wr 0xF34A0110 = 0x11CB0000      mbox A host->SA word0 = PA of the ZIT parameter block (VA 0x91CB0000)
wr 0xF34A0114 = 0x10A1B000      mbox A word1           [? second buffer]
wr 0xF34A0118 = 0 ; 0xF34A011C = 0
wr 0xF34A0008 = 0x80000000
wr 0xF34A0030 = 0x11E10600      code/entry PA          [? SA program image]
rd 0xF34A0104 ; wr 0xF34A0104 = 0xFF      ack all source-A factors (W1C)   (43925FB4, state := 3)
rd 0xF34A0108 ; wr 0xF34A0108 = 0xFF      enable all source-A factors
rd 0xF34A0110..011C (read back)
rd 0xF34A0000 ; wr 0xF34A0000 = 0x1       enable
rd 0xF34A0004 ; wr 0xF34A0004 = 0x1       GO
```
After this there is no further SA access and no IRQ 51 (the block is RAZ/WI in `io-f0`).

### 3.4 Diagnostic proof (r5)
In the stalled state, `set *(u8*)0x43ED6D67 = 0` (gdb, VM paused for a moment). Within a few frames:
* imager request 0x83 accepted; mode sequencer at **step 42** with all `actual == target`
  (`0102 0202 0302 0405 0502 0602 0702 … 0a00 0b10 0c02`);
* `0x43D874F0` = `8040b24a 00000003` (CS state 0x02, **flag 0x03**);
* `43773E80(0, 1)` called from `4376FDC3` → **`dword_43D8AF48[0] = 1`** (output 0 requested). Outputs 1/2 stay 0
  (bit2 clear in state 0x02).
* `VphUpdateYuvCall 43661BC8` / `438F6CF4` still 0 hits: state 2 is not reached yet (§5).

This poke is only a diagnostic. It is **not** a valid fix: the imager job stays blocked in `rcv_mbx`, and the ZIT
result (defect correction data) is never produced.

## 4. Spec — SA DSP cores (what the emulator must provide)

The SA program ISA is unknown (RE_USERLAND_SUBCPU §3), so this boundary needs HLE (AI_WORKFLOW §1.5). Do the HLE at the
**SA core register boundary**, not in av-cam.

### 4.1 Register map (per core; av-cam accessors `43923E74…43924660`)
Core bases: SA0 `0xF34A0000`, SA1 `0xF34B0000`, SA2 `0xF34C0000`, SA3 `0xF35C0000` (accessor index 0/1/2/3). Clock
and reset are in `0xF3400000` (SA0: rst `+0x2B0`, clk `+0xB0`).

| offset | access | meaning (evidence) |
|---|---|---|
| `+0x000` | RMW bit0 | enable / run (`43923E74`) |
| `+0x004` | RMW bit0 | go / start (`43923EE0`); read `43923EAC` |
| `+0x008` | W | `0x80000000` before start (`43923F18`) [? mode] |
| `+0x00C`, `+0x01C` | W / RMW | misc (`43923F66/FC6/4008`, `43924038`: `+0x1C` low 2 bits from cmd byte 15) [?] |
| `+0x030` | W | program/entry PA (`43924070`) |
| `+0x100 / +0x104 / +0x108` | R / RW / RW | source **A**: status byte, **interrupt factor** (W1C by writing the read value back), factor **enable mask** |
| `+0x110..+0x11C` | RW | source A host→SA mailbox, 4 words (`43924284`; cleared to 0 on A events) |
| `+0x120..+0x12C` | R | source A SA→host mailbox, 4 words (`4392424C`, idx+4; `439242C0`, idx+8 → `+0x120/+0x124`) [? exact split] |
| `+0x140/+0x144/+0x148`, `+0x150..+0x16C` | | source **B**, same layout (ISR B writes `+0x144 = 0xFF`, `+0x148 = 0`) |
| `+0x200/+0x204/+0x208`, `+0x210..+0x22C` | | source **C**, same layout |

### 4.2 Interrupts
IRQ 51/52/53 = SA0 sources A/B/C, 54–56 = SA1, 61–63 = SA2, 64–66 = SA3. All are enabled by task 44 `439232BC`
(RE_AVCAM_IRQ_BOOT §1.1). ISR A for SA0 is `4391A4C4`:
`ev = {8, 0xC1, src=0, b3=+0x100&0xFF, b4=+0x104&0xFF, b5=+0x108&0xFF, 0, 0}`. It then writes `+0x104 = b4` (ack)
and `+0x108 = 0` (mask off), and posts `ev` to queue 0x101 (`4391A42C`, tag 0x930101).
Model the line as `(+0x104 & +0x108 & 0xFF) != 0` per source. That is level-like: the ISR's ack plus mask clear
drops it [? exact polarity of `+0x108`].

### 4.3 Firmware-side completion path (what makes the 0x41 reply)
* Task 44 (`439232BC`, queue 0x101) receives the event. Message byte1 = 0xC1 → `43925888`, switch on `src`.
  For `src 0` and core-0 state `dword_43E10FF8 == 3` (set by `43925FB4` at calc start), it calls
  **`4391D3BC`**, which builds the client reply:
  `{len 28, id 0x41, 0x0101, b4, b5, 0, 0, 0, 0, b10 = ev.b4 (int_fact), 0, words +12..+24 = 4 mailbox words
  (sub_43AC864E(core)), …}`. It posts the reply to the ZIT client mbx (`0xBC60xx`).
* The client `4398EA3C` accepts the result only if **byte1 == 0x41, byte10 (int_fact) == 1, byte6 == 0 and
  byte7 == 0**. Otherwise it logs "SA Function Start Done Received But SA int_fact Error %d mbox %x %x %x %x" and
  returns an error (`720 + int_fact`).
* `4391D3BC` then ends core 0's job via `4391CE7C` (if `int_fact & 0x3F`). The client sends `ZIT_SaClose` (0x23),
  which the manager answers without hardware [? not traced].

### 4.4 Minimal HLE behaviour (proposal)
1. A write of `+0x004` bit0 = 1 while `+0x000` bit0 = 1 starts "execution". After a short delay (e.g. 100 µs [?]),
   set `+0x104 |= 0x01` (source A, factor bit0 = "function end"). Raise the IRQ of that core's source A (51 for
   SA0) while `+0x104 & +0x108 & 0xFF` is non-zero.
2. `+0x100` reads 0 [?]. `+0x120..+0x12C` read 0 (they are copied into the reply but only printed on error).
3. Leave the parameter block in DRAM untouched. ConvZ1C zeroes the output counters (`v4+24..+38`) before the start,
   so 0 output points passes its overflow check (`43990B2C`: `sum(+24..+30) <= +20`). That is "no defects to
   correct" [?: `sub_43AE1A46 → sub_43AE19A6` must accept 0; verify at run time].
4. The same core is used by other SA clients (JPEG, face detection, cmpr). Keep the HLE generic: every "go"
   completes on source A with factor 0x01, and nothing else is emulated. Log every start with `+0x030` /
   `+0x110..11C` (`LOG_UNIMP`/trace) so that clients that need real output can be identified later.

### 4.5 Expected result once §4.4 is in
`ZIT_SaStart` returns 0, then `ZIT_SaClose`, then ConvZ1C returns 0, then step `4398F28C` returns 1. The imager
job finishes and clears busy. Module 10 accepts 0x83 and the sequencer runs steps 34–42, as in §3.4. Then
`0x43D874F4 = 0x03` and `dword_43D8AF48[0] = 1`.
[? open: step 1's table entry jumps to itself (`arg = 1`). Termination needs a status > 0xF or an empty defect
buffer. Confirm at run time that busy (`0x43ED6D67`) really returns to 0. If not, trace `4398DDD6..4398DE88`.]

## 5. After the request: the YC frame (state 1 → 2), partly open

* State 2 is set only in `437737D4`, by the **EE sequence functions** `43546E40 … 43551478` (25 call sites). In CS
  state 0x02 (`(entry word0 >> 15) & 0x1F = 1`), VD_Pre's sequence selector `437DD4BC` (case 1 → `437DD3D8`) picks
  **`43546E40`**, or `4354D618` when `sub_437B9668()==0` etc. At run time the current sequence `0x43D84B5C` =
  `{1, 0x43A6C1F3 (functor invoker), 0, 0x43546E41}`, so `43546E40` was picked.
* The functor is not called directly. VD_Sequence `4376BDBC` passes it as `a2` to `4376FC48` only when
  (`0x43D84B5C == 0x43D84D04`) **and** `dword_43D84B74 == 0` **and** `byte_43D8747D != 0`. `4376FC48` then creates
  an **Image** record (`437E3910`, "Image::Create Image NULL" if the pool is empty) that carries the EE functor. The
  functor runs later from the image pipeline [? exact trigger not traced: expected to be the front-end/develop
  completion chain].
* In the poked run (§3.4, plugin-slowed) `4376FC48` always got `a2 = 0`: `byte_43D8747D` read 0 when sampled,
  `437E3910` got 0 hits, and `43546E40` / functor `43A6C1F2` got 0 hits. The many "VD_Sequence: start delayed"
  frames, where VD_Pre bails out before setting `0x43D8747C/7D = 1`, are a likely cause [?].
* So once §4 is modelled, the next things to check are, in order:
  (1) whether `byte_43D8747D` is 1 when VD_Sequence runs (timing);
  (2) whether `437E3910` creates Images and what consumes them;
  (3) whether the develop/resize IRQs 67–75 or VFX 77/78 are then kicked and must complete (RE_SENSOR_ISP G4).
  These IRQs were not seen kicked in any run so far.

## 6. Other observations
* **The flaky "imager does not power on" boots** (about 2 of 5 here) stall the *same* sequencer earlier, at step 12
  (`WAIT{1,2,3,7,9,10}`). Every `actual[]` stays `xx00` (`0x43DC7160 = 02000100 04000300 …`, even modules 1–3) and
  module 10 never gets request 0x10. That points at the HS_Main reporting branch (`byte *(0x43D84A5C)+2`), not at
  the imager [?]. Passed on to the owner of that issue.
* The known engine oops `PC=0, LR=4376FBD7` (task `liro-kliro_100`) is in `4376FBA4`. It walks list
  `dword_43D88F18`, executes items, and calls `item[6]->vtbl[3]`. It happened once in r4 at guest time ~14.4 s,
  before any manual intervention.
* `0x3024` is checked with `can_req` every few frames from `43A5B7B7/7CB`. It is never sent in these runs [?
  meaning].

## 7. Open questions
1. Exact semantics of `+0x008`, `+0x00C`, `+0x01C`, `+0x100` and the source-A factor bits other than bit0. Is a
   real calc-done always source A, factor 0x01? This rests on `4398EA3C` requiring int_fact == 1 and on
   `43925FB4` arming source A.
2. Whether the ZIT step-1 loop terminates after a successful ConvZ1C (§4.5).
3. The EE-sequence trigger chain (§5) and whether develop/resize/VFX IRQs gate YC state 2.
4. Whether other SA clients run during boot or live view (JPEG/face detection) and need real output data.
5. The cause of the step-12 stall in the flaky boots (§6).

## 8. Reproduce
* IDA: `C:\a5000\emu\ycreq\q.py` (copy of `sensor\cf_q.py`). Recopy `camfe.i64` before each run: the copy got
  corrupted twice after a save (`Database initialization failed with error 4`).
* Probes (`a5ktrace fn=`): `4375C920 syncreq`, `4398E1B8 img_req`, `4398DBFC img_job`, `4398F28C step1_1`,
  `43990B2C convz1c`, `4398EC10 sa_open`, `4398E93A sa_cmd`, `4398EA3C sa_wait`, `4398E870 sa_calc`,
  `43AC7D84 mq_send`, `43773E80 yc_req`, `437737D4 yc_state2`, `4376FC48 vd_post_fc48`, `437E3910 img_create`.
* RAM to check (HMP `xp`, PA = VA + 0x40000000): `0x83DC7110` (sequencer idx), `0x83DC7120`/`0x83DC7160`
  (target/actual), `0x83D874F0`, `0x83ED6D60` (imager seq), `0x83D8AF48` (YC outputs).
* Run `-d unimp` and grep `f34a|f340` to see the SA0 start sequence of §3.3.

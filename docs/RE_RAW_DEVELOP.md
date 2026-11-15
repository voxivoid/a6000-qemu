# Still capture: RAW development passes, flag 22 bit 0x400 and the IDT engine (fw 1.10)

Scope: the gate after `EndRawSeq` in the lever-off capture (`RE_CAPTURE_COMPLETION.md` §6, §9). Task
`liro-kliro_115` waits forever in `F22_wait(0x440)` from `4385B82F`; bit 0x40 arrives, bit 0x400 never does. This
document finds the hardware behind bit 0x400, proves it by forcing the events by hand, follows the capture to the next
stop and gives a spec for the code agent. RE only: no emulator code was changed.

Evidence:
* Static: local disassembly of the live av-cam dump (VA 0x43500000, 10 MiB; the IDA host was unreachable). Addresses
  are av-cam VAs (Thumb), GOT base 0x43EDAACC. Backup props from the unit's `Backup.bin` and the updater's backup
  images.
* Dynamic: binary `qemu-system-arm-sgc1m` (= the merged Sgc1/Iif model), pristine NAND clone per run, guest shell,
  `a5ktrace` `fn=` probes (`ts=1`, `fnr1s=1` for av-cam log strings), HMP `xp`/`pmemsave`/`trace-event cxd_aif_*`,
  `log unimp` (switched on at the lever only), gdb used only to divert a thread into an ISR (§5).
  Loader: the stock-equivalent cold-boot loader (`0x51000F = 1`), and for r3–r6 the same loader plus one extra line
  (§3.3).

| run | loader | what |
|---|---|---|
| r1 | cold boot | reproduce the stop; probe the CamCore and IDT paths |
| r2 | cold boot | boot-time IDT init: "Init NG" |
| r3, r4 | + `idt_cam.bin` | IDT init OK; same stop; MMIO log of the kick; events of IRQ 72 forced → bit 0x400 |
| r5 | + `idt_cam.bin` | same, step/flag probes after the force |
| r6 | + `idt_cam.bin` | same; next stop decoded (IRQ 69), forced, then the third stop (task 74 trap) |

`[?]` = not proven.

---------------------------------------------------------------------------------------------------------------------

## 0. Summary

| Question | Answer |
|---|---|
| What the two bits wait for | `4385B708` (step of task 115) starts **two jobs of one RAW pass**: `StartRdraw_AfterCap` `437D45E0` kicks the **IDT "FOPD" job** with completion callback **`43A59E6A` (bit 0x400)**, then a **CamCore unit 1 job** `43A3F3D8` with callback **`43A59E4E` (bit 0x40)**, and waits for both (`4385B82A`). |
| Who sets 0x40 | The CamCore object `0x43D6A190` completion: `43A408AA` calls the plain function pointer at `obj+0x84` from the unit-1 frame path (CPU 2, IRQ context). The campig model's per-frame unit-1 completion already produces it. |
| Who sets 0x400 | **IRQ 72**, ISR `436ACA24`: status **`0xF302042C`**. Bits 25:24 = `10` ("event 2") → `IdtEv 43983798(status)` → `43983020` → `43983050..4398309A` calls the stored callback (`0x43E1D590` = `43A59E6B`) → `set_flg(22, 0x400)`. |
| What that hardware is | The **IDT develop/FOPD control block** at **0xF3020400..0xF3020FFF** (same 4 KiB page as AIF group 0xF3020000, ch17–20). FOPD = a full-frame pass of the captured RAW: CamCore unit 1 reads the RAW (AIF ch6, bus 0x12000000) and streams it over the IIB (ch10 → ch17, `0x60000000`) into the develop block, whose output (ch18) writes 0x285F0 bytes to bus **0x1EC00000** (CPU 0x9EC00000); the OPD sub-block 0xF3031000 gets the same address (`+0x7C0`). |
| Why the model never produces it (two causes) | (1) **`idt_cam.bin` is not in RAM.** The bootloader loads `\sabin\idt_cam.bin` to **PA 0x83501000** (Backup 0xFA0080 load table, §3). IDT init `4397BB1C` checks `(u16)*0x83501000 == 0x3D` (`4397BAF8`); it reads 0 → **"IDT : Init NG"** → the IDT interrupt handlers are never installed (slots `0x43D6FD64..74` stay 0) and IRQs 67–70/72 are never enabled (`43983668`). (2) **0xF3020400..0xFFF is plain register file** (`io-f0`): no device sets `+0x42C`, nothing raises IRQ 72. |
| Proof | r3/r4/r5/r6: with `idt_cam.bin` loaded IDT init succeeds (`IdtChkV` r0 = 0x3D, `IdtReg 43983668`). Writing `+0x42C = 0x02003F00` and diverting a thread into ISR `436ACA24` → `IdtEv(0x02003F00)` → `43983020` → `F22_400 43A59E6A` → task 115 leaves `F22_wait(0x440)`, its step returns 0. Event 1 (`0x0100003F`) before event 2 also works (r4–r6). Without the loader line the same force jumps to a NULL handler (r1). |
| What the capture waits on next | **IRQ 69** (OPD unit **0xF3030200**). Task 74's step `4385B90C` runs the second IDT pass `43973F50` (synchronous): enables `0xF3030208 = 0xFF`, starts the OPD block (`0xF303000C |= 2`, `436ADA9C`), and waits for a reply message on queue **0xB02C9** (`439740C2`). Forced IRQ 69 (`+0x204 = 1`) → `43983750` → deferred `439773BC` (task 77) → `+0x208 = 0` → callback `439738E0` → reply → task 74 continues (`IdtRel`, `sig_sem(21)`). |
| After that | Task 74 hits a deliberate trap (`439B193C`, "strb r3,[r3]" with r3 = 0) shortly after, in the result handling `4385B9CC → 43A7B9A6 → 4379067A` → kernel oops; `sub-micro power off` not reached. Whether this is a consequence of the hand-forced events / unwritten result buffers or a further missing device is **not decoded** [?] (§6). |

---------------------------------------------------------------------------------------------------------------------

## 1. The pass in task 115 (`4385B708`)

```
4385B7EE  bl 437D45E0  StartRdraw_AfterCap(rec, …)      ; callback 0x400, KickFOPD
4385B812  bl 43A3F3D8  CamCore job(handle 4, rec[0x428], …, callback = GOT[0x2840] = 43A59E4E (0x40) or GOT[0x25C])
4385B82A  bl 43A59C7A  F22_wait(0x400 | 0x40, 1)
4385B836  bl 43A5ABFA  …  → step returns 0 (next)
```
* `437D45E0` fills a request at `rec+0x4E7C` (`43A7062A`), puts **GOT[0x4CF4] = `43A59E6B`** at `req+0x28`
  (`437D4638..437D4640`) and calls `439739F0(rec+0x3658, desc, req)` (**KickFOPD**, IDT library; error string
  `"StartRdraw_AfterCap: KickFOPD error"`). Siblings with the same pattern: `437D4380` (`StartRdraw_Prelight`, no
  callback) and `437D4460` (`StartRdraw_Read`, callback 0x400 at `437D452A`).
* KickFOPD (`req[7] != 2`, the case seen): `43982F58(2)` (IDT state = 2), `4397738C(req+0x28)` stores the callback
  in **`0x43E1D590`**, `43978394(7)`.
* Run time (r1, r3–r6): `T115a` (r0 = 0x1580 = 5504, r1 = 0xE48 = 3656, the RAW size) → `Rdraw_After` →
  `KickFOPD` → `CCjob` → `F22_440wait`; `F22_40 43A59E4E` arrives from `43A408C3` (swapper/2) one frame later;
  0x400 never.

### 1.1 Bit 0x40: CamCore completion
`43A408AA(obj)`: `43A40620(obj, 0)`, then `f = obj[0x84]; obj[0x84] = 0; f()` (plain function pointer). `obj` =
CamCore `0x43D6A190` (handle 0x10E1). Called (lr `43A40023`) every unit-1 frame; with an empty `+0x84` nothing
happens. The job of `4385B812` programs unit 1 (`0xF3100118/0x178 = 0x200`, `0xF3110xxx`, AIF ch6 read of bus
0x92000000 / ch10 write to IIB) — the existing campig/AIF models complete it. No change needed.

---------------------------------------------------------------------------------------------------------------------

## 2. Bit 0x400: IRQ 72 and the IDT event path

### 2.1 IRQ table (av-cam `{irq, 0, isr, 2}` records at `0x43E9510C..`)
| IRQ | ISR | status | ack | calls slot | slot handler (installed by `43983668`) |
|---|---|---|---|---|---|
| 67 | `436AC9B4` | `0xF3030104` | write back | `0x43D6FD64` | GOT[0x365C] = `43ADF086` = `set_flg(29, status)` |
| 68 | `436AC9D0` | `0xF3030144` | write back | `0x43D6FD68` | GOT[0x4FA8] = `439836D4` |
| 69 | `436AC9EC` | `0xF3030204` | write back | `0x43D6FD6C` | GOT[0x51C4] = `43983750` (§4) |
| 70 | `436ACA08` | `0xF3030304` | write back | `0x43D6FD70` | GOT[0x0E80] = `43983710` |
| **72** | **`436ACA24`** | **`0xF302042C`** | see below | `0x43D6FD74` | GOT[0x1E14] = **`43983798` IdtEv** |

`43983668` (only caller: IDT init `4397BB1C`, `4397BBA2`) installs the five handlers and enables IRQs 67, 68, 69,
70, 72 (`ena_int` `4355D8FC`). Before that the slots are 0 (r1: HMP/gdb read of `0x43D6FD60..7C` = 0).

### 2.2 ISR `436ACA24` (IRQ 72)
```
st = R(0xF302042C)
switch (st & 0x03000000):
  0x01000000: W(0xF302042C, 0x0100003F); W(0xF3020410, 1); W(0xF3020414, 1); arg = 1        ("event 1")
  0x02000000: W(0xF302042C, 0x02003F00); W(0xF3020008, 0x1111); W(0xF3020000, 0xFFFF);
              W(0xF3020410, 2); W(0xF3020414, 2); arg = st                                  ("event 2")
  else:       error flag := 1; W(0xF302042C, 0x03000301); W(0xF3020008, 0x111); W(0xF3020000, 0xFFF);
              W(0xF3020410, 3); W(0xF3020414, 3); arg = st                                  ("error")
slot[0x43D6FD74](arg)
```
So `+0x42C` is a W1C status: bit 24 summarises bits 0..5, bit 25 summarises bits 8..13 [? meaning of the 6 + 6 low
bits; the ISR only tests bits 25:24]. Event 2 also clears all four nibbles of AIF group 0xF3020000 (`+0x000` W1C) and
re-enables them (`+0x008 = 0x1111`): ch17–20 belong to this job.

### 2.3 `IdtEv 43983798(arg)`
* `arg == 1` (event 1): IDT phase bookkeeping only (`43978394(9)` for states 2–4, `43978814`, `43974BA4`). No
  callback.
* otherwise (event 2 / error): `439774B0`, `43974C30`, `43982E90`; state 1 → `43983000`; **states 2..4 →
  `43983020`**: state 2 → callback = `439773A4()` = `*0x43E1D590`, states 3/4 → a second stored callback (2-pass variants, not decoded) [?];
  `blx callback` at `4398309A`.

### 2.4 The kick, as seen on the bus (r3, `log unimp` at the lever, task 115)
```
KickFOPD 439739F0:
  F30317F0 = 0x00800080, F30317C0 = 0x9EC00000            ; OPD sub-block: output address
  F2804044/148/14C/150, F280512C/130/134 (RMW 0x47x)       ; bus priority [?]
  F3020C10 = 0, C18 = 1, C04 = 0, C04 = 0, C08 = 2, C0C = 0x15800E48 (5504 x 3656), C74 = 0, C78 = 0
  F30317C8 = 0x018600D4, F30317CC = 0x00070011, F30317D0 = 0x002B1111
  F3020C1C = 0, C34 = 0x15570E1F, C30 = 0x0008000C, C3C = 0x2B, C40 = 0x1111, C38 = 0x00060010,
  C24/C20/C54/C50/C5C/C60 = 0, C58 = 0xFFFEFFFF
  F3020444 = 3, F3020444 = 3                               ; event enables (bit0 = event 1, bit1 = event 2) [?]
  F3020420: read 0, write 2                                ; RMW bit 1
  F3020418 = 2, F3020518 = 1
  F3020C00 = 0, F302051C = 0, F3020C00 = 1, F302051C = 1   ; run
AIF (trace): group 0xF3020000 ch1 (ch18) start addr 0x1EC00000 size 0x285F0
             group 0xF3020000 ch0 (ch17) start ctrl 0x803 addr 0x60000000 (IIB) size 0x2B00
CamCore job 43A3F3D8: group 0xF3102000 ch0 (ch6) addr 0x92000000 size 0x2B00, ch4 (ch10) ctrl 0x803 addr 0x60000000
~2 ms later: cxd_aif_done group 0xF3020000 channels 0x3; group 0xF3102000 channels 0x11
```
Live view uses the same block every frame with `+0xC0C = 0x07200208` (1824 × 520), `+0x418 = 3`, **`+0x444 = 0`**,
`+0x420 = 0`, `+0xC00 = 1`, `+0x51C = 1`, and the group's ch18 statistics output (bus 0x40100000/0x4012F000). It never
gets IRQ 72 (no `Isr72` hit in live view in r3–r6). The writer of `+0x444` (`436ACB80`, callers `436AD37A/38A/3BA`)
defers the write while a job is running and applies it afterwards — the behaviour of an interrupt-enable register.
Hence the rule used in the spec: **IRQ 72 events are generated only for the enabled bits of `+0x444`** [? bit
assignment inferred from 3 = both events and the ISR's two paths].

Reset/abort helpers `436AD14A`, `436AD3CC`: `+0x42C = 0x03003F3F`, AIF `+0x008 = 0x1111`, `+0x000 = 0xFFFF`,
`+0x410 = +0x414 = 3`, `+0x418 = 0`, `+0x41C = 0`. Software reads only `+0x420` of this block (RMW).

Register accessors (av-cam, base literal → offset): `436ACAF4` +0x400, `436ACB00` +0x404, `436ACB0C` +0x408,
`436ACB18` +0x40C, `436ACB24` +0x418, `436ACB30` +0x41C, `436ACB3C` +0x420 bit 16, `436ACB58` +0x420 bits 2:0,
`436AC984` +0x410, `436AC990` +0x414, `436AC99C`/`436AC9A8` +0x42C W/R, `436ACB74` +0x430, `436ACB80` +0x444,
`436ACB8C..BBC` 0xF3020500 +0x00/+0x08/+0x14/+0x18/+0x1C, `436ACAC8` +0x520 = 0x1F, `436ACBC8` 0xF3020600,
`436ACBE0/BEC/BF8` 0xF3020C00 +0x00/+0x04/+0x08, `439AAB38..` 0xF3020C04..C40.

---------------------------------------------------------------------------------------------------------------------

## 3. Cause 1: `idt_cam.bin` is not loaded

### 3.1 IDT init `4397BB1C` (called once at boot from the camera init list, `437645E4`, task 65)
```
r7 = 4355DCD8(0x00FA0074)          ; Backup/resource lookup → 0x83501000 (r2)
r0 = 4397BAF8(r7): *(u32 *)0x43E1… = *r7; return (u16)*r7 == 0x3D
if (r0 != 1) { log "[IDT]: IDT : Init NG : address = %x" (0x43C21D6F); 43A96E24(6, …, 7); return }
43A97114/43A97134(13..16, …)       ; AIF hooks
43969A68(); 43983668()             ; handlers + ena_int(67,68,69,70,72)
```
Run time: r2 (stock cold loader) `IdtChk r0 = 0x83501000`, `IdtChkV r0 = 0` → `IdtNG`; r3 (with the file)
`IdtChkV r0 = 0x3D` (r2 = `0xB039003D`, the first word of `idt_cam.bin`) → `IdtReg` → `IdtRegDone`.
VA 0x83501000 maps to PA 0x83501000 (HMP `gva2gpa`), i.e. 0x40000 below av-cam's load address 0x83541000.

### 3.2 Who puts it there: the bootloader's load table (Backup prop 0xFA0080)
`Backup.bin` of the unit (and every `0110_backup` image of the updater) holds, in prop **0x00FA0080** (9216 B), a
table of `{u32 type, u32 start, u32 end, u32 flags, char path[0x80]}`:

| type | start | end | flags | path |
|---|---|---|---|---|
| 3 | **0x83501000** | 0x83540FFF | 0x05000000 | **`\sabin\idt_cam.bin`** |
| 3 | 0x9EC00000 | 0x9EC2FFFF | 0x01000000 | `\sabin\sa_dfdet.bin` |
| 3 | 0x83541000 | 0x84136FFF | 0x05000000 | `\av-cam.bin` |
| 2 | 0x80033000 | 0x80033FFF | 0x01000000 | `\kemco.txt` |
| 3 | 0x80428000 | 0x80C27FFF | 0x01000000 | `\initrd.img` |
| 3 | 0x80038000 | 0x80427FFF | 0x81000000 | `\vmlinux.bin` |

The kernel, initrd, `kemco` block and av-cam rows are exactly the boot contract the board/loader already reproduce
(`HARDWARE.md`, `provision/loader.sh`). Prop **0x00FA0074 = 0x83501000** (the address IDT init asks for) and 0x00FA0076
= 0x83541000 (av-cam) are in the same sub-0xFA memory map. `idt_cam.bin` (214 496 B, header `3D 00 39 B0 42 34 …`,
the common `sabin` program header of `RE_USERLAND_SUBCPU.md` §3) fits the 0x40000 window. Nothing in av-cam, the
kernel or userland references the file name, so the bootloader is the loader.

Cross-reference for `RE_COLD_BOOT_MODE.md` (not edited here): the same table lists **`sa_dfdet.bin` →
0x9EC00000**, i.e. option (B) of that document. Note the FOPD job in §2.4 uses bus 0x1EC00000 (= 0x9EC00000) as its
output buffer, so on the real camera that region is reused once the SA boot unit is freed [?].

### 3.3 Loader change used in r3–r6 (test only)
After the av-cam line of `provision/loader.sh`:
```
a5kcopy -f2p /mnt/system/sabin/idt_cam.bin 0x83501000 || fail idt_cam
```

---------------------------------------------------------------------------------------------------------------------

## 4. The next pass: IDT synchronous job on the OPD block (IRQ 69)

After bit 0x400 (forced), task 115's step returns 0; task 73's step `43A8788B` runs `S4` and returns 3; task 74 runs
step `43A8AC67` → stage 0 `4385B90C`:
```
43973924  IDT acquire: wai_sem(21, -1)                    (passes)
43A706D4  request
43973F50  synchronous IDT op:  …, 43977398(GOT[0x30AC] = 439738E0)   ; reply callback "B" → *0x43E1D8E8
          43976194(2)  (IDT sync state 0x43E1D2E8 = 2)
          43A49190(0xFF) → 0xF3030208 = 0xFF                ; IRQ 69 source enable
          43AE3ADE(2), 43AE3AEA(0x1100), 43AE3AE6 → 436ADA9C: 0xF303000C |= 2   ; OPD start
          439740C2: osal receive on queue 0xB02C9, timeout -1          ← stop 2
43973984  IDT release: sig_sem(21)
```
Register state at stop 2 (r6): `0xF3030000.. = 1, 1, 0x80000000, 3`; `0xF3030200.. = 0, 0, 0xFF, 0`;
`0xF3020400..: +0x410/414/418 = 2, +0x41C = 0xC2, +0x420 = 2, +0x42C = 0x02003F00 (forced value left by the regfile),
+0x430 = 0x03000000, +0x444 = 3`; `0xF3020514/518/51C = 1, +0x520 = 0x1F`.
Kernel-stack evidence (task 74, `pmemsave` of DDR, thread_info 0x4A8DC000): `4355DA43 ← 439740C7 ← 4385B989 ←
43A6C1FB ← 43A72C5B`.

Completion path (r6, forced): IRQ 69 ISR `436AC9EC` reads `0xF3030204`, writes it back, calls `43983750(status)`:
sync state (`*0x43E1D2E8`) 1 → `439761D0` (disable); **0 or 2 → defer `439773BC`** (`438948B4` on queue object
`0x43E1F368`) → runs in task 77: state == 2 → `0xF3030208 = 0` (`43A49190(0)`) → `blx *0x43E1D8E8` =
`439738E0` → `snd_msg(0xB02C9, 12-byte reply)` → task 74 returns from `43973F50` (`T74ret 4385B988`) → `IdtRel`
→ `sig_sem(21)`.

The four OPD units at 0xF3030100/140/200/300 share one layout: `+0x04` status (W1C, byte; the ISR writes back what it
read), `+0x08` source enable (byte; `0xFF` armed, 0 idle; `43A49198` sets all four `+0x04 = 0xFF` and `+0x08 = 0` at
init). Which status bit the hardware sets is not decoded; the handlers do not test it [?].

---------------------------------------------------------------------------------------------------------------------

## 5. Proof by hand

Same diversion as `RE_CAPTURE_COMPLETION.md` §4 (task 43's `dly_tsk` veneer `4355D498`, lr `43904371`), with the
status register written first:
```
hbreak *0x4355d498 if $lr == 0x43904371
continue
set $sr0=$r0
set $slr=$lr
set $ssp=$sp
set *(int*)0xf302042c = 0x0100003f      # event 1   (IRQ 69 test: set *(int*)0xf3030204 = 1, pc 0x436ac9ec)
set $lr=0x4355d499
set $pc=0x436aca24
delete
hbreak *0x4355d498 if $sp == $ssp
continue
set *(int*)0xf302042c = 0x02003f00      # event 2
set $lr=0x4355d499
set $pc=0x436aca24
continue
set $r0=$sr0
set $lr=$slr
delete
detach
```

| step | observed |
|---|---|
| r1 (no `idt_cam.bin`), event 2 | `Isr72` runs, `IdtEv` never: slot `0x43D6FD74` = 0 |
| r3 (file loaded), event 2 only | `Isr72` → `IdtEv(0x02003F00)` → `IdtDone2 43983020` (r0 = 2) → `IdtCb 4398309A` (r0 = `43A59E6B`) → `F22_400 43A59E6A`; task 115 leaves `twai_flg` |
| r4/r5/r6, event 1 then 2 | event 1: `IdtEv(1)`; event 2: as above; task 115 `StepRet` 0; task 73 `S4`, task 74 `StepCall 43A8AC67` |
| r6 stop 2 | task 74 in `osal_rcv_msg_tmo` (stack §4) |
| r6 IRQ 69 forced (`+0x204 = 1`) | `Isr69` → `43983750` → `E750sched 43983768` → `IdtCbB 439773BC` (task 77) → `IdtReply 439738E0` → `T74ret` → `IdtRel` → `IdtRelSig` |
| r6 stop 3 | kernel oops in task 74, `PC 439B193E` (§6); no `ExitCap`, no `sub-micro power off` |

---------------------------------------------------------------------------------------------------------------------

## 6. Stop 3 (not decoded)

`439B193C` is a trap stub (`movs r3,#0; strb r3,[r3]`, a family of them at `439B1940..`); the oops shows
`lr 439B194D`, stack `4379067F`, `43A7B9EB`, `4385B9D1`: task 74's step `4385B90C` → `43A7B9A6` (after the IDT pass)
→ `4379063x..4379067A` (copies out of an IDT result table) hit an internal consistency check. Candidates [?]:
(a) result data the IDT/OPD hardware should have written (FOPD output at 0x9EC00000, OPD tables at 0xF3031000) are
zero because no model moves data; (b) side effects of forcing events from task 43 on CPU 1 instead of the hardware ISR
on CPU 2, or IRQ 69 forced with an arbitrary status value. Re-check after the spec below is implemented; then decode
`4379063x` (what it reads and which field is zero).

---------------------------------------------------------------------------------------------------------------------

## 7. Spec for the code agent

### 7.1 Loader (bootloader stand-in)
In `provision/loader.sh`, normal (non-updater) path, next to the av-cam line:
`a5kcopy -f2p /mnt/system/sabin/idt_cam.bin 0x83501000 || fail idt_cam`. Comment: bootloader load table Backup
prop 0xFA0080 (`\sabin\idt_cam.bin` 0x83501000..0x83540FFF), IDT init `4397BB1C`/`4397BAF8` checks u16 0x3D, prop
0xFA0074. A HOT resume keeps RAM, so nothing else is needed there. Rebuild `loader.cpio`. (The `sa_dfdet.bin` row is
the other agent's topic, `RE_COLD_BOOT_MODE.md`.)

### 7.2 IDT develop control block, 0xF3020400..0xF3020FFF, IRQ 72 (new device, e.g. `cxd90014-idt`)
Map 0xC00 bytes at 0xF3020400 (priority above `io-f0`; the AIF group 0xF3020000 keeps 0x000..0x3FF). All offsets
read back as written except below; unknown offsets `LOG_UNIMP`.
* **`+0x42C` status**, write-1-to-clear (all bits). Event 1 sets `0x01000001`, event 2 sets `0x02000100`
  [? low bits; firmware tests only 25:24].
* **`+0x444` enables**: bit 0 → event 1, bit 1 → event 2 (hypothesis, §2.4).
* **IRQ 72** level = `((st & 0x01000000) && (en & 1)) || ((st & 0x02000000) && (en & 2))`.
* **Run**: a write of `+0xC00` with bit 0 going 0 → 1 starts a frame **only if `+0x444 & 3`** (live view writes 0 and
  must stay silent). Event 1 a short time after the start (e.g. 100 µs) [?]; event 2 at the frame end: when the AIF
  channels of group 0xF3020000 that are running at the start have completed (a GPIO/callback from the AIF instance), or
  at most after a fallback delay (e.g. 5 ms) if none is running [?]. Timing hypothesis: the real duration is one pass
  over 5504 × 3656 pixels; firmware only needs event 1 before event 2, both after the run write.
* `+0xC00` bit 0 is not polled by software; leave it set (hypothesis: no self-clear needed).
* `+0x420`, `+0x410/+0x414/+0x418/+0x41C/+0x430`, 0xF3020500.., 0xF3020600.., 0xF3020800.. tables: read back.

### 7.3 OPD units, 0xF3030000..0xF3031FFF, IRQs 67/68/69/70 (new device, e.g. `cxd90014-opd`)
* Units k = 0..3 at `+0x100, +0x140, +0x200, +0x300` → IRQ 67, 68, 69, 70. `+0x04` status byte (W1C), `+0x08` source
  enable byte. IRQ_k level = `(status_k & enable_k) != 0`.
* **Start**: a write of `0xF303000C` with bit 1 going 0 → 1 (`436ADA9C`) completes after a short delay (e.g. 1 ms [?])
  by setting status bit 0 in **every unit whose enable is non-zero** [? which unit belongs to which job; r6 only
  proves unit 0x200 / IRQ 69 for the synchronous IDT pass]. Clear `+0x0C` bit 1 at completion [?].
* `0xF3031000..0xF30317FF` (OPD tables; `436ADB7A` clears 512 words at 0xF3031000) and `+0x7C0..+0x7F0`: read back.

### 7.4 Comments / provenance
Cite `436ACA24` (ISR and its acks), `43983798`/`43983020` (event → callback), `437D45E0`/`439739F0` (kick and
callback store `0x43E1D590`), `436ACB80`/`436AD360` (deferred `+0x444` write), live-view vs FOPD values of
`+0x444`, `43973F50`/`436ADA9C`/`43A49190` (OPD start/enable), `43983750`/`439773BC`/`439738E0` (IRQ 69 → reply
0xB02C9). Mark the event timing, the low status bits, the `+0x444` bit meaning and the OPD unit selection as
hypotheses.

---------------------------------------------------------------------------------------------------------------------

## 8. Acceptance tests

Fresh NAND clone per run, `A5K_SHELL`, the cold-boot loader with §7.1. Probes as in §9.
1. **IDT init OK.** Boot probes `4397BB08` (r0 = 0x3D), `43983668` hit, `4397BBB4` (Init NG) not hit.
2. **Live view unchanged.** Shooting screen, MENU (`sendkey m 300`), PLAY as before; probe `436ACA24` (IRQ 72) and
   `436AC9EC` (IRQ 69): **0 hits** while monitoring (`+0x444 = 0`); `436ACA24` probe count stays 0 for 60 s.
3. **Bit 0x400.** `sendkey o 300` at the shooting screen: `Rdraw_After 437D45E0` → `KickFOPD 439739F0` →
   `F22_wait(0x440)` (`4385B82A`) → `Isr72` (r0 = IRQ 72 path) twice → `IdtEv 43983798` with r0 = 1, then r0 & 0x03000000
   = 0x02000000 → `43A59E6A` → task 115 `StepRet` (lr `4385B86B`) r0 = 0.
4. **IRQ 69.** `IdtAcq 43973924` (task 74) → `Isr69 436AC9EC` → `439773BC` (task 77) → `439738E0` → `4385B988` →
   `IdtRel 43973984`.
5. **Lever off powers off.** `sub-micro power off (factor 7, warm, wake 81 81 81)` and HMP `info status` = `paused
   (suspended)` (`RE_REBOOT.md` C.6). **Expected to still fail at §6** (task 74 oops `PC 439B193E`, read with
   `tools/klog.py`); if so record the oops and hand §6 to RE.

## 9. Reproduce
* Probe set (lever): `437d45e0 Rdraw_After`, `439739f0 KickFOPD`, `43a3f3d8 CCjob`, `4385b82a F22_440wait`,
  `43a59c7a F22_wait`, `43a59e4e F22_40`, `43a59e6a F22_400`, `436aca24 Isr72`, `43983798 IdtEv`,
  `43983020 IdtDone2`, `4398309a IdtCb`, `43973924 IdtAcq`, `43973984 IdtRel`, `439739d4 IdtRelSig`,
  `436ac9ec Isr69`, `43983750 IdtE750`, `439773bc IdtCbB`, `439738e0 IdtReply`, `4385b988 T74ret`,
  `43a72c56 StepCall` (w[2] = step), `43a72c5a StepRet`, `437ccfe8 S4`, `436f94d4 ExitCap`, `43894784 LOG` (`fnr1s=1`).
  Boot: `4397bb1c`, `4397bb08`, `4397bbb4`, `43983668`.
* MMIO: HMP `log plugin,unimp` just before `sendkey o` and `trace-event cxd_aif_* on`; `xp /16wx 0xf3020400`,
  `xp /4wx 0xf3020c00`, `xp /8wx 0xf3030100`, `xp /4wx 0xf3030200`, `xp /4wx 0xf3030000`.
* Task stacks: `pmemsave` DDR, find `liro-kliro_<n>\0` (comm at task+0x1E8), thread_info = the 8 KiB-aligned
  0x4xxxxxxx word in the first three words of the task, `cpu_context.sp` at thread_info+0x3C, then scan for odd
  words in 0x43541000..0x44137000.

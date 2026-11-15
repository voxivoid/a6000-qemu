# Still capture: the RAW-buffer DMM gate and the loader boot mode (fw 1.10)

Scope: why every still capture blocks in the emulator (first seen on the power-lever off, `RE_REBOOT.md` section
"Power-lever off: module 1 (0x0104) gate"), what the real camera does differently, and a spec for the code agent.
RE only; no emulator code was changed for this document.

Evidence:
* Static: `dmm.ko` (objdump, symbols present), `DmmConfig.bin` (the unit's `/setting` copy, decoded below), `upm.ko`
  (Hex-Rays output in `re/ida/kmod_i2cgrp/upm.c`), av-cam (local disassembly of a live 10 MB dump, VA 0x43500000;
  the IDA host was unreachable), `Backup.bin` (PMCA `BackupFile` parser), the factory WBI snapshot (`tools/wbi.py`,
  page walk for module VAs).
* Dynamic: binary `910eae2`, pristine NAND clone per run, no SD, guest root shell (`A5K_SHELL`), dmm's own procfs
  (`/proc/udm/dmm_units`, `dmm_units2`, `dmm_extents`, `dmm_local_que`, `dmm_timer`, `dmm_log`), `a5ktrace fn=`
  probes, gdb stub, HMP `x`/`xp`/`memsave`/`pmemsave`, a kernel-stack scan of the waiting task.
  * r1: reproduce the stuck lever off, dump `/proc/udm/*` before and after the lever.
  * r2, r3: probes on the av-cam DMM client and on the OSAL sync send (inconclusive, superseded by `dmm_log`).
  * r4, r5: **diagnostic** gdb write that turns the SA manager's `Free(0x40B3)` into `Free(0x0400)` (message
    buffer only, no code): the RAW allocation then succeeds.
  * r7, r8: QEMU started with `-S`; gdb breakpoint on `k_upm_setup_boot_factor`; **diagnostic** write of the RAM
    Backup byte 0x51000F = 1 (COLD) before upm reads it. The stock firmware then frees the boot unit by itself, the
    RAW allocation succeeds, and the capture goes on to the next gate (§5).

`[?]` = not proven.

---------------------------------------------------------------------------------------------------------------------

## 0. Summary

| Question | Answer |
|---|---|
| What blocks `RawDevInit` | Unit type **0x173762** can only live in extent type **0x4E**, and the only DmmConfig table that defines 0x4E extents is **table 0x2B**, the capture layout of **area 0x11** (PA 0x9EC00000, 20 MiB). Area 0x11 stays laid out with table 0x2C because one of its children is still allocated: **boot unit handle 0x0400, type 0x116, extent 0x154 at 0x9EC00000, 0x30000 bytes** (the `wbimem=0x30000@0x9EC00000` region). dmm keeps the request in its local queue and retries on a timer; nothing ever frees the area, so `AllocNewWait` waits forever. |
| Who frees the boot unit on the real camera | **av-cam's SA manager (task 44, reply tag 0x930101)**: `Open(0x0400)` + `Free(0x0400)`. The factory WBI snapshot shows exactly this history on unit 0x0400 (alloc by dmm, open and free by 0x930101). |
| Why it does not happen in the emulator | The SA manager picks its start-up path from upm's **boot factor** (`g_bf+8`), which upm derives from the **loader boot mode, Backup 0x51000F**. The emulator presents **2 = HOT** (the value the user's NAND kept from the camera's last real power-on) while it actually does a **cold boot** (vmlinux + initrd from NAND, DDR cleared). HOT → `g_bf+8 = 4` → the SA manager takes the warm path: it tries to free/reuse the previous session's SA unit (handle 0x40B3 from Backup 0x930003, which no longer exists) and never touches the boot unit. |
| Proof | r7/r8: RAM Backup 0x51000F set to 1 before upm init → upm `g_bf+8 = 1`, unit 0x0400 is gone by 50 s uptime, the shooting screen comes up as before, and on the lever off `RawGet(0x173762)` returns at once (unit 0x6036 at 0x9EEB2000, extent type 0x4E, area 0x11 now on table 0x2B). |
| Missing hardware behaviour | Not a register: the **bootloader duty "write the loader boot mode for the boot actually performed"** into the RAM copy of Backup.bin. The board's stage-1 loader (our bootloader stand-in) copies Backup.bin unchanged. Spec in §6. |
| Is the lever off fixed by this alone | **No.** The capture then reaches its steps 2–4 and blocks at the next gate (§5): task 107 waits on av-cam event flag 22 bit 0x100, whose only setter is the completion callback of a **CamCore::Sgc1** capture job. That is a separate RE topic. |
| Normal shutter capture | Not run [?] (needs *Release w/o Lens* in the menu, RE_LENS_EMOUNT §6). The blocked unit type 0x173762 is the CamCore image-descriptor unit used by the capture init (`437C10B0`/`437C1198`/`437C11C8`), so a user capture is expected to hit the same gate [?]. |

---------------------------------------------------------------------------------------------------------------------

## 1. DmmConfig.bin and the dmm allocator

### 1.1 File layout (from `dmm_ext_tbl_Init` 0xDDD8 and `dmm_unit_tbl_Init` 0xB6E8)
dmm.ko reads `DmmConfig` (module parameter; the unit's copy is `/setting/DmmConfig.bin`, 75123 bytes, config version
"for ildcsc4g20m-mss 2013/11/08 1") sequentially through `dmm_config_GetConfigData`:

1. **Root map** (≤ 2): an 80-byte header, then 20-byte extent records `{addr, ?, type, size, attr}` until a record
   with `type == 0xFFFFFFFF`. These are the top-level areas (`/proc/udm/dmm_extents` ids 0x01..0x14), e.g.
   `{0x92000000, 0x17, 0x11, 0x0CC00000, 0x01000001}` and `{0x9EC00000, 0x15, 0x31, 0x01400000, 0x01000001}`.
2. **Tables** (≤ 1024, 367 present): 80-byte header with `hdr[2]` = the extent type the table subdivides and
   `hdr[3]` = table index; then 20-byte records `{0, ?, type, size, attr}`. An extent's "table" column in
   `dmm_extents` is the table currently applied to it.
3. **Unit table** at file offset 0xFCF8: `{unit_type, 0}` followed by a −1-terminated list of extent types the
   unit may use; ends with `unit_type == −1`. 608 units.

Attribute bits used by `dmm_ext_tbl_Init`: 0x40000000 = boot extent (a boot unit is created for it at init when
`dmm_ctrl_Init(mode) != 2`, `0x6DA4..0x6DC8`), 0x08000000 = suspend area, bit 31 = "boot-free" (freed by
`dmm_ctrl_RemoveBootUnit`, only from `dmm_pre_suspend`; **no extent has it in this config**).

### 1.2 The entries that matter

| item | value | source |
|---|---|---|
| unit 0x173762 | extent types **[0x4E]** | unit table @0xFD38 |
| extent type 0x4E | 17 × 0xC000, only in **table 0x2B** (parent type **0x31**) | table seq 42 @0x3714 |
| area of type 0x31 | extent 0x0011, PA **0x9EC00000**, 0x01400000 | root map |
| table 0x2C (parent 0x31, the boot layout) | 0x3A5 (1 MiB, sub-table 0x2D) + 0x3A6 (5 MiB, sub-table 0x2E) + free | seq 43 @0x3E1C |
| table 0x2D (parent 0x3A5) | **0x154**, 0x30000, attr **0x4000010F** (boot extent) | seq 44 |
| table 0x2E (parent 0x3A6) | 0x195, 0x500000, attr 0x4800010F (boot + suspend area) | seq 45 |
| unit 0x116 | [0x154] → boot unit handle **0x0400** | unit table |
| unit 0x157 | [0x195] → boot unit 0x0401 (freed by dmm itself at init, client 0x8F013A) | `dmm_log` |
| unit 0x277 | [0x334] → boot unit 0x0402 (opened/closed by client 0x050000, stays) | `dmm_log` |
| table 0x2B (capture layout of area 0x11) | 6×0x6A (0x2F000), 17×0x20D (0x18000), **17×0x4E (0xC000)**, 17×0x57 (0x400), 7×0xAA (0x40000), 14×0x9F (0x10000), 0x12A, 2×0xF4, 0x393 (0x7E9000), 3×0x65 | seq 42 |

Kernel command line: `wbimem=0x30000@0x9EC00000` is the same 0x30000 region as boot extent 0x154.

### 1.3 Allocation behaviour
`dmm_ctrl_AllocUnitNewHdlr` (0x67A4) → `dmm_unit_ctrl_Alloc` (0x8CE8). For the "no extent now" result codes it
returns 3 (request queued, no reply) instead of 2 (reply). The queued request is retried from a dmm timer. A table
switch of an area needs all of the area's current children free [? exact rule in `dmm_ext_ctrl_AllocExtent` 0xD420
not decoded; observed: as soon as unit 0x0400 was free, area 0x11 was re-laid with table 0x2B on the first
0x173762 request, r4].

---------------------------------------------------------------------------------------------------------------------

## 2. Stuck state (r1, lever OFF at ≈ 215 s)

```
/proc/udm/dmm_local_que:  Que(000099af): 000560f9 -> 0001fff8 00173762 00000000 ffffffff 00550001
/proc/udm/dmm_timer:      99af
/proc/udm/dmm_units:      0400  00000116  0001  01  00  00  01  00000001  9ec00000  00030000  0018 < 0015 < 0011
/proc/udm/dmm_extents:    0011  0000  01000001  00000031  0000002c  ...  9ec00000  01400000      (table 0x2C)
                          0018  0015  01000001  00000154  ...  00000001(pict) ...  9ec00000  00030000
```
No extent of type 0x4E exists. Live-view units of area 0x13 (0x80170417 …) and area 0x10 are released by the lever
path as expected; area 0x11 is not touched by live view at all. The av-cam side is as in RE_REBOOT C.1/C.3
(task 107 `osal_rcv_msg_tmo` in `438956BC`, `43A5A612(0x173762, 0, 1, −1)`).

---------------------------------------------------------------------------------------------------------------------

## 3. The boot unit on the real camera (factory WBI snapshot)

dmm's unit array (stride 0x188, unit 0x0400 at VA 0x3F1197F0 with dmm loaded at 0x3F0C7000 in both the snapshot and
the emulator) and its extent array (stride 0x48, extent 0x18 at VA 0x3F1469C0) were compared:

| | emulator (r1) | factory snapshot |
|---|---|---|
| unit 0x0400 status | `116 00000001 01000001` (allocated) | `116 ffffffff 01000000` (**free**) |
| unit 0x0400 command history (+0x78) | 1 entry: alloc by 0x00610000 (dmm) | 3 entries: alloc by 0x00610000, **open (5) by 0x00930101, free (4) by 0x00930101** |
| extent 0x18 (type 0x154) owner unit (+0x28) | 0x3F1197F0 | 0 |
| area 0x11 table | 0x2C | 0x2C (free children, so table 0x2B can be applied on demand) |
| upm loader boot mode copy (VA 0x3F32F060) | 2 (HOT) | **12 (BIS)** |

So the real camera released the boot unit during the boot that produced the snapshot (a cold-class boot, BIS), and
every later HOT/SS power-on inherits that DMM state (DDR or WBI). The emulator is the only case with a fresh cold
DMM init **and** a HOT boot mode.

---------------------------------------------------------------------------------------------------------------------

## 4. The decision chain (loader boot mode → upm → SA manager)

### 4.1 upm boot factor (`upm.ko`)
* `k_upm_setup_boot_factor` (0xBF88) reads **Backup 0x51000F** (`dword_151D4`) and fills the boot-factor block
  `g_bf` (registered as the sync reply of queue 0x924127, `k_upm_apicb_boot_factor`). The word read by av-cam at
  `g_bf+8` (`dword_14D34`) is:

  | 0x51000F | meaning (RE_USERLAND_SUBCPU §1) | `g_bf+8` | upm text |
  |---|---|---|---|
  | 1 | COLD | 1 | "ON:fast EE(Cold)" |
  | 2 | HOT | **4** | "ON:fast EE(Hot)" |
  | 10 | WARM | 4 | "ON:Other(Warm)" |
  | 11 | SS (WBI boot) | 8 | |
  | 12 | BIS | 1 (8 while profiling/rewriting) | "BIS" |
  | other | | 1 | |

* Live values: emulator r1 `g_bf` (VA 0x3F32EBB8) = `00920111 00000003 00000004 00000002 041f0001 00000001`;
  r7 (0x51000F forced to 1) → `g_bf+8 = 1`.

### 4.2 SA manager start-up (av-cam task 44, queue 0x101)
* `4391AB8C` (SA init): reads Backup 0x930000/01/02/**03**/**0A**/05/08/09 into globals, then the upm boot factor
  (`osal_snd_sync_msg(0x924127, {0x920111})` → 24 bytes at **0x43E0CCB4**).
  * Backup 0x930003 → table 0x43E0CCCC (8 × 8 bytes `{kind, state, u16 handle, u32 unit type}`); user's file:
    `02 11 40B3 001C4000` (the SA program unit of the camera's last real session).
  * Backup 0x93000A → table 0x43E0CC6C; user's file: **`02 01 0400 00000116`** = "program kind 2 is in boot unit
    0x0400, type 0x116".
* `43AC8FE6` (program restore at start-up):
  * `4391AB54()` = `g_bf+8 ∈ {4, 8}` (HOT/WARM/SS) → **warm branch**: walks the 0x930003/0x930006 table and frees /
    reuses the old units (`Free(0x40B3)` → error 0xFDFB, `Reuse(0x40B3, type 0x1C4000)` → 6, then ODL loads the
    program into a new unit 0x0429). This is the emulator today (`dmm_log` S_MSG 0x21/0x34 from 0x00930101).
  * otherwise (COLD/BIS) → **cold branch** `43AC9084`: walks the 0x93000A table; for an entry with state 1 it
    `Open`s the handle (`4391B77C`), registers the program, and unless `g_bf+8 == 2` (`4391ACE8`) **frees the unit**
    (`4391B870(handle, kind)`). With 0x93000A = {2, 1, 0x0400, 0x116} this is exactly the factory history of §3.
* The content of the boot unit (PA 0x9EC00000) is zero in the factory snapshot and in the emulator; only the DMM
  bookkeeping matters here [? what the bootloader may preload there on other boot kinds].

### 4.3 Other readers of 0x51000F (risk check for the fix)
`udate.ko`, `libObj.so`, `vdf_vph_driver`, `libUsbBusPower.so` and dmm (`SuspendModule`) only test for 12 (BIS) or
6 (USB_CHARGE); upm switches its boot-factor text and flags (§4.1). Mode 1 is a supported stock path ("ON:fast
EE(Cold)"). r7 with mode 1 reached the normal shooting screen (NO CARD, OSD complete) at the usual time.

---------------------------------------------------------------------------------------------------------------------

## 5. After the DMM gate: the next block (r7, r8; for the next RE task)

With 0x51000F = 1, the lever-off capture runs (probes, task numbers as in RE_REBOOT C.1):

```
EnterCap 436F94A0 (task 66) → CapStart 437C43A8 (task 96) → S0 43853B80 (72)
→ RawDevInit 437D0C18 (107): RawGet(0x173762) → DmmWait → RawGetDone (returns at once)
→ S2 43A8BE64 (72) → S3 437D0DB0 (107): RawGet(0x5056E) → done → S4 437CCFE8 (72, "NextSection")
→ step 4385947C (107): 43A59C7A(0x100, 1) = twai_flg(flag 22, 0x100, −1) ... never returns
```
`/proc/<pid>/wchan` of `liro-kliro_107` = `twai_flg`; kernel stack: `4355D5C7` ← `43A59CA3` ← `438594A9` ←
`43A6C1FB` (step dispatcher). Module 1 stays 0x0000, `0x43E64FC7` stays 1, no `ExitCapturing`.

* Flag 22 bit 0x100 is set only by **`43A59CEA`** (trace code 0xF008). It is never called directly; its only
  reference is GOT offset 0x589C (slot VA 0x43EE0368), loaded at `437D114A` in the capture step `437D0FC8`, where it
  is passed as the **completion callback** of a **`CamCore::Sgc1`** job: `43A46C10(Sgc1 object 0x43D10388, 3,
  rec+0x408, 1, &cb, &cb2)` → `436A7284` (programs the routing of units 0xF3000000 / 0xF3100000 / 0xF3120000,
  `43A468BC`/`43A468EA`) and then waits for 0x102 itself (`437D11AC`).
* In r8 neither `43A59CEA` nor the generic setter `43A59CAC` ran; whether `437D0FC8`/`436A7284` ran for this
  capture was not probed [?].
* Candidate missing hardware: the capture-job completion of the CamCore input unit 0xF3120000 (IRQ 35, ISR
  `436A6904`); `cxd90014_campig.c` never sets the second status word `+0x4C` ("source unknown") [?]. This needs its
  own RE document (probe `437D0FC8`, `436A7284`, `436A6904`, the Sgc1 vtable 0x43E8B718 slots, and the `+0x48/+0x4C`
  status during a capture).

---------------------------------------------------------------------------------------------------------------------

## 6. Spec for the code agent: loader boot mode in the RAM Backup

### 6.1 What the bootloader does (and the stand-in must do)
The Sony bootloader rewrites the loader-owned Backup properties (attr 0x02) **0x51000F (boot mode)** and 0x510013
(boot reason) in the RAM copy of Backup.bin on every power-on (RE_REBOOT B.5). The emulator's boot is always the
bootloader's **COLD** path (kernel + initrd from nflasha3, DDR cleared; RE_WARM_RESUME §0), so the RAM copy must
say **0x51000F = 1** [? bootloader code itself not available; the value follows from the mode list and from the
cold-class factory boot (BIS) behaving the same way, §3]. This supersedes RE_REBOOT B.7 item 6 ("keep 0x51000F = 2"):
2 (HOT) is only right once a real warm resume (RE_WARM_RESUME) is modelled, and then only for that path.
0x510013 stays as it is (1 = POWER_SLIDE, correct for lever and normal power-on).

### 6.2 Where (no QEMU device change)
In the stage-1 loader (`provision/loader.sh`, the bootloader stand-in), right after
`a5kcopy -backup /mnt/setting/Backup.bin`, set the property in the RAM image:

```
a5kcopy -bkset 0x0051000f 01 || fail "boot mode"
```

New `a5kcopy -bkset <prop-id> <hex bytes>` (in `provision/a5kcopy.c`), working on the RAM copy that `-backup` just
placed at PA = file header word 0 (0x82EC0000 for the user's file; read it, do not hard-code it):

| field | offset in the image | notes |
|---|---|---|
| header | 0x000, 0x100 bytes | +0x0C "BKn\0" (revision n = 4 here; 2 uses 5-byte property entries), +0x10 numSubsystems (u32), +0x14 numProperties, +0x18 dataOffset, +0x1C dataSize, **+0x20 checksum** |
| subsystem table | 0x100, 6 bytes each, packed | `{u16 numProperties, u32 firstIndex}`; index = id >> 16 |
| property table | 0x100 + 6·numSubsystems | rev 4: `{u16 attr, u32 ptr}` packed, entry = firstIndex + (id & 0xFFFF); rev 2: `{u8 attr, u32 ptr}` |
| property data | `ptr & 0xFFFFFF`, size `ptr >> 24` | size 0 = variable (`{u16 size, u16 max}` header), 0xFF = oversize (`{u16 size}` header); 0x51000F is a plain 1-byte property |
| checksum | +0x20 | sum of all bytes in `[0, dataOffset + dataSize)` except `+0x20..+0x23` (PMCA `BackupFile._calcChecksum`); recompute after the write |

Values for the user's file (cross-check only): numSubsystems 403, numProperties 17093, dataOffset 0x19B20, dataSize
0xA4F8B; entry of 0x51000F at image offset 0x4CD2, ptr 0x01045B7F → data at **PA 0x82F05B7F** (reads 0x02 today).
Refuse (exit non-zero) on an unknown revision, an id outside the tables, or a size mismatch. Map the page through the
existing `pmap()` helper as `-w32` does.

Optional, not needed for the fix: a machine/run option to choose the mode (e.g. 12 to mimic the factory BIS boot)
— default must be 1.

### 6.3 Comments / provenance
Cite: upm `k_upm_setup_boot_factor` (0xBF88, boot-mode → `g_bf+8` mapping), av-cam `43AC8FE6`/`43AC9084` (SA
manager cold/warm branch), Backup 0x93000A = {2, 1, 0x0400, 0x116}, dmm unit 0x0400/type 0x116, factory snapshot
history of unit 0x0400 (§3). Mark the bootloader behaviour as hypothesis (`[?]`, no bootloader code).

### 6.4 Acceptance tests
Fresh NAND clone per run, no SD unless stated, `A5K_SHELL` on.

1. **Boot mode and boot unit.** After boot (shell up): `bk.elf r 0x0051000F` → `01`. By 60 s uptime
   `/proc/udm/dmm_units` has **no handle 0400 / type 00000116** (0402 / 0x277 is still there, as on the real
   camera). Optional: HMP `x /6wx 0x3f32ebb8` → third word `00000001` (address valid for this module layout only).
2. **Shooting screen unchanged.** Same OSD as before at ≈ 200 s (`screendump`), keys/MENU work; RE_REBOOT B.8 tests
   1, 2, 4, the USB tests (README) and PLAY behave as before.
3. **RAW allocation.** Lever off (`sendkey o`) at 200–300 s, then within 30 s: `/proc/udm/dmm_local_que` has no
   entry for 0x00173762; `/proc/udm/dmm_units` shows one 0x00173762 unit at **0x9EEB2000, size 0xC000**;
   `/proc/udm/dmm_extents` shows extent 0x0011 with table **0x2B**. With probes (RE_REBOOT C.7 list): `RawGetDone
   4375421A` hits from `liro-kliro_107`, followed by `S2 43A8BE64`, `S3 437D0DB0`, `S4 437CCFE8`.
4. **Not expected yet:** `sub-micro power off (factor 7 …)` (RE_REBOOT C.6) still needs the §5 gate; the capture is
   now expected to stop in step `4385947C` (task 107 `twai_flg`). Once that gate is modelled, C.6 is the acceptance
   test for the lever off, and a shutter capture with SD (*Release w/o Lens* enabled) must complete without hanging.

---------------------------------------------------------------------------------------------------------------------

## 7. Reproduce

* Run: `A5K_SHELL=<p+2> A5K_NAND=<clone> A5K_QEMU=<copy> ./run.sh -display none -monitor
  tcp:127.0.0.1:<p>,server,nowait -gdb tcp::<p+1> [-S] [-d plugin -D <log> -plugin
  tools/liba5ktrace.dylib,fn=<file>,fncap=300,fnwords=4,s=0]`.
* DMM state: `tools/a5kshell.py --port <p+2> run 'cat /proc/udm/dmm_units'` (also `dmm_units2` = per-unit command
  history with client ids, `dmm_extents`, `dmm_local_que`, `dmm_timer`, `dmm_log` = message ring, wraps after
  ≈ 160 messages, so read it early for boot-time traffic). `bk.elf r 0x<id>` reads Backup props.
* Diagnostic A (proves §1/§2, not a fix): gdb `hbreak *0x3f0ccc20 if *(unsigned short*)($r0+14) == 0x40b3` (dmm
  `dmm_ctrl_FreeUnitHdlr`, message: +12 cmd, +14 handle, +34 mode), then `set *(unsigned short*)($r0+14) = 0x400`,
  `detach`.
* Diagnostic B (proves §4, what §6 implements): start with `-S`, gdb `hbreak *0x3f325f88`
  (`k_upm_setup_boot_factor`), `continue`, `set *(unsigned char*)0x42f05b7f = 1` (kernel VA of the RAM Backup byte
  of 0x51000F, PA − 0x40000000), `detach`.
* Probes used for §5: `43a59cea Flag22_100`, `43a59cac Flag22_set`, `43a59c7a Flag22_wait`, `4385947c WaitStep`,
  `437ccfe8 S4`, `437c3b54 EndRawSeq`, `436f94d4 ExitCap`, `436f94a0 EnterCap`.
* Kernel stack of a waiting task: find `comm` in a `pmemsave` of DDR (task_struct + 0x1E8), `stack` pointer at
  task_struct + 8, `cpu_context.sp` at thread_info + 0x3C, scan up to thread_info + 0x2000 for odd av-cam addresses.

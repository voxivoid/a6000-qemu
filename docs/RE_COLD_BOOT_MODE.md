# Cold boot mode (Backup 0x51000F = 1): MENU, the SA boot unit and the hand-off `wbi_memclr` word (fw 1.10)

Scope: `RE_STILL_CAPTURE.md` §6–§7 ask the stage-1 loader to present the loader boot mode **1 (COLD)** instead of the
stale **2 (HOT)**. The first implementation attempt (`a5kcopy -bkset 0x0051000f 01` in `loader.sh`) freed boot unit
0x0400 and let the RAW unit be placed, but "MENU no longer opens" (two runs). This document finds why, and lists what
else a real cold boot presents that the emulator does or does not reproduce. RE only; no emulator code was changed.

Evidence:
* Static: `av-cam.bin` (objdump, Thumb, VA = file offset + 0x43541000), `dmm.ko`, `re/vmlinux.elf`, the unit's
  `Backup.bin` (ODL tables 0xBD0000–0xBD0007, SA tables 0x930000–0x93000A), `/system/sabin` and `/usr/sabin`, the
  factory WBI snapshot (`re/wbi`: section map + RAM), the board's hand-off blob `data/handoff_80021000.bin`.
* Dynamic: binary `18850ab` (same model as `910eae2` for this path; the only difference is the HOT-resume code),
  pristine NAND clone per run, no SD, `A5K_SHELL`, `a5ktrace` `fn=` probes on the SA manager, HMP
  `xp`/`pmemsave`/`screendump`/`sendkey`. Two guests ran in parallel per pair (identical host load per pair).

| run | boot mode | extra | result |
|---|---|---|---|
| m1a / sta | 1 / 2 | SA probes | both: MENU opens (100 ms press); m1a's next 100 ms press did not close it, a 400 ms press did |
| m1b, m1c | 1 | — | 100 ms press at ≈ 283 s: no menu in either |
| m1d / std | 1 / 2 | key-sink probe | m1d: **kernel oops at 27 s** (task 113 `Sync_Kick_IDT`, `RE_ENGINE_OOPS.md`), UI dead; std: MENU opens |
| m1e, m1p | 1 | — | **300 ms presses at 200 / 250 / 300 s: MENU opens and closes 6/6** |
| m1f / stf | 1 / 2 | key-poll probe | 100 ms presses: m1f 1/4, **stf (stock) 0/4**; 300 ms presses: both open |
| m1q | 1 | diagnostic: `sa_dfdet.bin` at PA 0x9EC00000 + hand-off +0xE8 = 0 | ZIT start succeeds and repeats as in stock; unit 0400 free at 124 s; MENU, PLAY, lever off (area 0x11 on table 0x2B, empty local queue) OK |

`[?]` = not proven.

---------------------------------------------------------------------------------------------------------------------

## 0. Summary

| Question | Answer |
|---|---|
| Why did MENU "fail" with mode 1 | **It does not depend on the boot mode.** A 100 ms `sendkey m` (the default hold, used by the earlier runs) is often too short for the firmware's key path: it was missed in **both** modes (stock 0/4, mode 1 1/4 in the same run pair). With a **300 ms** hold MENU opens and closes every time in mode 1 (8/8 opens over m1e, m1p, m1f, m1q) and in stock (stf 1/1; sta/std/base also opened with 100 ms presses). The earlier "stock works" observations were lucky short presses; one further mode-1 "failure" (m1d) was the known host-load engine oops (`RE_ENGINE_OOPS.md`), not the boot mode. |
| Key sampling rate | av-cam task 48 parses the sub-micro slot-A key word (`43AC73AE`, CAkey1 = b28 \| b29 << 8) about every **43 ms** of guest time, identical in both modes (m1f 23.2/s, stf 23.1/s). A 100 ms hold covers only 2–3 frames; the downstream debounce needs more [? exact count not decoded]. |
| Is mode 1 safe to enable | Yes for the UI: shooting screen, MENU, PLAY, lever off, RAW placement all work with mode 1 (m1e, m1p, m1f, m1q). |
| What else differs on a COLD start | The SA manager's cold branch makes SA **program 2 = `sa_dfdet.bin`** (the "ZITTOOLS" imager defect detector) "resident" in boot unit 0x0400 at **PA 0x9EC00000** and later **reuses** that unit instead of loading the program through ODL (§2). In the emulator that memory is zero, so the first ZIT ConvZ1C start fails once with SA error **12** (function index ≥ program's function count) and ZIT is not retried. The UI is not affected; still-capture effects are unknown [?]. |
| Why the boot unit is zero | The kernel clears every `wbimem=` region above 0x80028000 at early boot when the hand-off word **`wbi_memclr` (PA 0x800210E8) == 1** (`wbi_cmdline_parse_mem`, `wbi_memclr`, §3). The board's hand-off blob (factory snapshot, a BIS boot) has **1**. |
| What a real COLD boot presents there | Unknown — the bootloader is not available. Either (A) `wbi_memclr = 1` and the region is zero (then the real camera also fails ZIT once, i.e. the emulator is already faithful), or (B) the bootloader loads `/system/sabin/sa_dfdet.bin` to 0x9EC00000 and leaves `wbi_memclr = 0` (then ZIT works as on a HOT boot). §4 weighs both; the spec keeps (A) and records (B) as a tested option. |

---------------------------------------------------------------------------------------------------------------------

## 1. The MENU observation, re-measured

### 1.1 Short key presses are missed in both boot modes
* m1f (mode 1) and stf (stock 2) ran side by side; at 216 s both got `sendkey m` (100 ms): neither opened the menu.
  A following `sendkey m 300` opened it in both. Then three more 100 ms rounds: m1f opened once, stf never.
* m1e and m1p (mode 1) with `sendkey m 300` at 200, 250 and 300 s uptime: the menu opened 20 s later and closed on
  the next 300 ms press, 6/6.
* The earlier failing runs (`RE_STILL_CAPTURE.md` §7, the code agent's r1/r2) used the default 100 ms hold. Their
  "after MENU" dumps show the shooting screen switching between its two OSD variants (with / without the `NO CARD` /
  exposure row) — this alternation happens without any key and is not a reaction to MENU.
* The ~300 s idle power-off seen in m1b/m1c right after the failed presses is the normal APO: a missed key does not
  reset the idle timer.

### 1.2 Key sampling (for the record)
`43AC73AE` (task 48, `liro-kliro_48`) copies slot-A bytes 28/29 into the key state every sub-micro frame. Hit counts
over 41.5 s of guest time (133→175 s uptime): mode 1 963 (23.2/s), stock 959 (23.1/s). So the
sub-micro exchange rate does not depend on the boot mode. The MENU event (0x1AB) does not go through av-cam's
`osal_snd_msg` to 0x7A0020 (`4355D9DC`) in either mode — no hit in m1d/std although std opened the menu — so the
debounce/forwarding is in another path [? not traced; not needed].

### 1.3 Host load
m1d died at 27 s with `Oops: 17` in `liro-kliro_113`, `PC 0x4394aa4c`, `LR 0x437dee4d` — exactly the
`Sync_Kick_IDT` race of `RE_ENGINE_OOPS.md`, opened by vCPU starvation (several QEMU instances were running on the
host). The shell then answers nothing and the screen freezes; such a run must not be counted as a MENU failure.
Check `tools/klog.py <monitor port>` for `Internal error: Oops` before judging a UI test.

---------------------------------------------------------------------------------------------------------------------

## 2. The real cold-boot difference: SA program 2 and boot unit 0x0400

### 2.1 Trace (m1b, mode 1; addresses av-cam)
```
SA cold branch 43AC9084 (g_bf+8 = 1)
  Open(0x0400)        4391B77C → {err 0, addr 0x9EC00000}
  state[2] := 2, handleA[2] := 0x0400, addr[2] := 0x9EC00000, type[2] := 0x116     (43920xxx setters)
  4391ACE8() == 0 (g_bf+8 != 2) → Free(0x0400) 4391B870 → OK (r0 = 0)
     Free also stores handleB[2] := 0x0400 (43920724, table 0x43E559FE)
  state[2] := 0
ZIT ConvZ1C (task 90): open prog 2 (0x21)
  43AC924E: per-core state 0, kind state 0 → "needs load" (0)
  4391E2E4 (ODL enabled byte 0x43E74D0B) = 1 → 4391FB24(core 0, prog 2):
     handleB[2] = 0x0400 ≠ 0xFFFF, type 0x116 → Reuse(0x0400, 0x116) (4391BB24)
     dmm_unit_ctrl_Reuse: generation, type and "free" bit match, extent 0x154 still free → success
  → load reply 0x22 at once: 01012214 00000201 04000001  (loaded, handle 0x0400), no ODL request
  per-core program address [core 0][2] (0x43E0F5D8 + 8) = 0x9EC00000
start (0x41) → 439208F8 → 439256E4: function type 3 ≥ header byte +0x0E of the image at 0x9EC00000 (= 0)
  → error 12 → reply 0101411C 0C010200 (b6 = 1, b7 = 0x0C)
close (0x23) → Free(0x0400) again (43AC807F); ZIT is not retried in this session
```
Stock (HOT, std): the warm branch sets handleB[2] = 0x40B3 (Backup 0x930003, a unit of the previous session);
`Reuse(0x40B3)` fails (0xFDFB), ODL loads program 2 into unit 0x0429 at **PA 0x91E00000** (unit type 0x1C4000), the
start succeeds (int_fact 1) and ZIT runs again on every imager start (8 open/start/close cycles by 124 s).

### 2.2 Program 2 is `sa_dfdet.bin`
* `pmemsave` of the ODL unit in std (PA 0x91E00000, 0x26084 bytes) is **byte-identical** to `sa_dfdet.bin`
  (155 780 bytes, header `aa0a1391 …`, `"ZITTOOLS0x02"` at +0x14, function count 8 at +0x0E). The copies in
  `/system/sabin` (nflasha3) and `/usr/sabin` are identical (same MD5).
* Backup ODL tables: 0xBD0000 file name list, entry 24 = `sa_dfdet.bin`; 0xBD0002 record 24 = `{0x18, owner
  0x0093 (SA manager), program id 2, 1}`; 0xBD0004 size classes (≤ 0x40000 → unit type 0x1C4000).
  This corrects `RE_SA_DSP.md` §6 (program id 2 was guessed as `sa_defect.bin` / `sa_zphdefdet.bin`).
* Backup **0x93000A = `02 01 0400 00000116`** ("program 2, state 1, boot unit 0x0400, type 0x116") has no writer in
  av-cam (the only reference is the init read list at `4391AC98`); it is static configuration. Boot unit type 0x116
  lives only in DmmConfig boot extent **0x154 at PA 0x9EC00000, 0x30000 bytes** (`RE_STILL_CAPTURE.md` §1.2), which
  is created by `dmm_ctrl_Init` on every non-resume dmm init (`dmm_unit_ctrl_InitBootUnit`) — big enough for
  `sa_dfdet.bin` (0x26084).

### 2.3 Effect of the failed ZIT start
ZIT ConvZ1C converts the imager defect table at imager start (`RE_SA_DSP.md` §0). With error 12 the client logs a
driver error and the imager start continues: live view, OSD, MENU, PLAY and the lever off are unchanged in all mode-1
runs. Whether a still capture later depends on the converted defect table is unknown [?] (the capture still stops at
the Sgc1 gate, `RE_CAPTURE_COMPLETION.md`).

---------------------------------------------------------------------------------------------------------------------

## 3. Why 0x9EC00000 is zero: the hand-off `wbi_memclr` word

* Kernel cmdline: `wbi_memclr=0x800210e8` (`parse_memclr` stores the PA). `wbi_memclr()` (`0x4033B534`) returns
  `*(PA 0x800210E8) == 1`.
* `wbi_cmdline_parse_mem` (`0x4033B56C`), for every `wbimem=size@addr` token: registers the region (the WBI save
  list); if `wbi_memclr()` and `addr > 0x80028000`, prints and **`__memzero`s the region** (`0x4033B634..0x4033B65C`).
  `bootmem_init` (`0x403396E4`) tests the same word.
* `wbimem=0x30000@0x9EC00000` is in the list; so is every other non-kernel region the WBI saves (the snapshot map has
  exactly these extents). The hand-off page itself (0x80021000) and anything below 0x80028000 are skipped.
* `data/handoff_80021000.bin` (from the factory snapshot, a BIS boot) has **+0xE8 = 1**. So today the kernel clears
  0x9EC00000 on every emulator boot — m1p showed it: a QEMU `-device loader` blob at 0x9EC00000 (present in
  `info roms`) was zero again when av-cam ran. With a hand-off copy whose +0xE8 = 0, the same blob survived (m1q).
* In the factory snapshot the region 0x9EC00000–0x9EC2FFFF is present and **all zero**; unit 0x0400 there shows
  alloc / open / free and no reuse (§3 of `RE_STILL_CAPTURE.md`).

Meaning [?]: +0xE8 = 1 tells the kernel "the WBI regions hold no valid image (DDR was not preserved), clear them";
the bootloader writes it per boot. Its value on a real COLD (mode 1) boot is not known.

---------------------------------------------------------------------------------------------------------------------

## 4. What a real COLD boot presents — the two candidates

| | (A) `wbi_memclr = 1`, region zero | (B) bootloader preloads `sa_dfdet.bin`, `wbi_memclr = 0` |
|---|---|---|
| ZIT at the first imager start | error 12 once (as the emulator does now with mode 1) | succeeds, repeats as on HOT/SS |
| For | the only hand-off value we have (factory BIS boot) is 1; the BIS snapshot has the region zero; no bootloader code that loads SA programs is known | static config 0x93000A points program 2 at that boot unit, and av-cam deliberately **reuses** it without an ODL load (fast cold start); `sa_dfdet.bin` sits in `/system/sabin`, the partition the bootloader reads (`vmlinux.bin`, `initrd.img`, `av-cam.bin`); the boot extent size fits it |
| Against | then the boot-unit scheme would only ever deliver zeros on a cold start, which makes 0x93000A pointless | the BIS snapshot (also a cold-class start) has the region zero and no reuse; with +0xE8 = 0 the kernel would not clear the other WBI regions either, so the bootloader would have to clear them itself |
| Emulator change | none (current behaviour) | stage-1 loader copies the file, board presents +0xE8 = 0 and clears the other `wbimem` regions itself |

Neither is proven. (A) needs no invented data and is what the unmodified firmware does with the only real hand-off
value; it is the default below. (B) is recorded as a tested option (m1q) for when a capture-path gate turns out to
need a successful ZIT run, or bootloader evidence appears (e.g. a real-camera RAM dump right after a cold boot:
read PA 0x800210E8 and 0x9EC00000).

---------------------------------------------------------------------------------------------------------------------

## 5. Spec for the code agent

### 5.1 Enable boot mode 1 (unchanged from `RE_STILL_CAPTURE.md` §6)
`provision/loader.sh`, right after `a5kcopy -backup /mnt/setting/Backup.bin`:
```
a5kcopy -bkset 0x0051000f 01 || fail "boot mode"
```
No other change is needed for MENU. Update `RE_STILL_CAPTURE.md` §7 / README status accordingly ("MENU breaks" was a
short-key-press artefact; this document).

### 5.2 Test procedure fix (scripts and docs)
* Use **`sendkey <key> 300`** (300 ms hold) for M, P, O and the arrows/Enter in every automated test; 100 ms (the
  HMP default) is missed most of the time in either boot mode.
* Before judging a UI failure, check `tools/klog.py <port>` for `Internal error: Oops` (host-load engine race).
* Wait ≥ 20 s after a key before the `screendump`.

### 5.3 Keep the hand-off as it is (candidate A)
No change to `data/handoff_80021000.bin` (+0xE8 = 1). Document in `docs/HARDWARE.md` / `BOOT.md` the hand-off word
**+0xE8 = `wbi_memclr`** ("1 = kernel clears the `wbimem` regions above 0x80028000 at boot"; value S from the
snapshot; meaning per kernel code; cold-boot value H).

### 5.4 Optional, off by default (candidate B, hypothesis)
Only if requested later: a machine/loader option (e.g. `A5K_COLD_PRELOAD=1`) that
1. in `loader.sh` copies `/system/sabin/sa_dfdet.bin` (raw, whole file) to **PA 0x9EC00000** (`a5kcopy` like
   `av-cam.bin`; refuse if larger than 0x30000);
2. presents hand-off **+0xE8 = 0**, and therefore clears the other `wbimem` regions (list in the cmdline, all with
   addr > 0x80028000 except 0x9EC00000) itself before stage 2, so their content does not depend on what the stage-1
   Linux left in DDR.
Mark both as `hypothesis:` with this document as evidence.

### 5.5 Acceptance tests (fresh NAND clone, no SD, `A5K_SHELL`, keys with a 300 ms hold)
1. **Mode presented:** `bk.elf r 0x0051000F` → `01`; HMP `x /6wx 0x3f32ebb8` third word `00000001` (this module
   layout only).
2. **Boot unit freed:** by 60–130 s `/proc/udm/dmm_units` has no line `0400 00000116` (0402 / 0x277 stays).
3. **MENU:** at ≥ 200 s, `sendkey m 300` → after 20 s the 6-tile main menu; `sendkey m 300` → shooting screen again.
   Repeat three times (200 / 250 / 300 s): 3/3.
4. **PLAY:** `sendkey p 300` → "no memory card" playback view; `sendkey p 300` → shooting screen.
5. **Lever off / RAW unit:** `sendkey o 300` at 200–300 s; within 30 s `/proc/udm/dmm_local_que` is empty,
   `/proc/udm/dmm_extents` shows extent 0x0011 on table **0x2B**; with `RE_REBOOT.md` C.7 probes `RawGetDone`, `S2`,
   `S3`, `S4` hit (`RE_STILL_CAPTURE.md` §6.4 test 3). The capture is then expected to stop at the Sgc1 gate.
6. **Expected ZIT behaviour (A):** with probes `43ac7d84` / `4391b254` (`fnr1w=8`): one ZIT cycle whose 0x41 result
   is `0101411c 0c010200` (error 12); the per-core program address `xp /1wx 0x83e0f5e0` = `0x9ec00000`.
   With option B instead: result `0101411c 00000201 00010000 …` (int_fact 1), repeated cycles, address 0x9EC00000,
   no ODL request for program 2.
7. **Regression (mode 2 path unchanged):** with the stock loader the warm branch frees 0x40B3 (error) and ODL loads
   program 2 to PA 0x91E00000 (`xp /4wx 0x83e0f5d8` third word `0x91e00000`).

---------------------------------------------------------------------------------------------------------------------

## 6. Reproduce

* Run pair (separate ports, same host load): `A5K_SHELL=<p+2> A5K_LOADER=<mode-1 or stock loader.cpio>
  A5K_NAND=<clone> A5K_QEMU=<copy> ./run.sh -display none -monitor tcp:127.0.0.1:<p>,server,nowait
  [-d plugin -D <log> -plugin tools/liba5ktrace.dylib,fn=<file>,fncap=300,fnr1w=8]`.
* SA manager probes: `43ac7d84 SAreq`, `4391b254 SAres`, `4391b870 SAfree`, `4391b77c SAopen`, `43ac9084 SAcold`,
  `43ac8ff2 SAwarm`, `43ac8d32 SAloadDecide`, `43ac90e6 coldFreeRet` (r0), `43ac92b8 kindState` (r0),
  `43ac85ae resDecide` (r0 = r7).
* Key poll rate: probe `43ac73ae` with a large `fncap`, count hits against `/proc/uptime`.
* Tables (kernel VA, PA = VA + 0x40000000): kind state `0x43E0F15C`, handle A `0x43E56434` (u16), address
  `0x43E0ED5C`, handle B `0x43E559FE` (u16), unit type `0x43E0E960`, per-core state `0x43E0D4F0[255·core + prog]`,
  per-core address `0x43E0F5D8[255·core + prog]`, ODL-enable byte `0x43E74D0B`. Indexes: program id.
* Option-B diagnostic (m1q): copy of the hand-off blob with bytes +0xE8..+0xEB = 0 via `A5K_HANDOFF=<copy>`, plus
  `-device loader,file=<sa_dfdet.bin>,addr=0x9ec00000,force-raw=on`.
* HMP file names containing `-` must be quoted (`pmemsave 0x91e00000 0x40000 "<file>"`).

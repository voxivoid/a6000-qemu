# lld.ko ↔ ARM7 NAND sub-CPU / NAND controller protocol (a5000 fw 1.10)

Static RE of `initrd.img/kmod/lld.ko` (BOSS "lld_131H"), cross-checked against
`nand.ko` (MUMIN FTL), the kernel `vmlinux.elf`, and lld/nand `.bss` in the
real-camera WBI RAM snapshot (`re/wbi/ram.bin`). The goal is an HLE device model
that replaces the ARM7 and its firmware.

Citations are `function+0xOFF` or absolute `.text` offsets in lld.ko (`@0x1234`).
**[S]** = value seen in the snapshot, **[?]** = inference, not proven.

---

## 0. TL;DR for the HLE implementer

* **On a normal cold boot (nand.ko `boot_mode=0`, which is the default; `/sbin/init`
  loads nand.ko with no arguments) lld never resets or loads the ARM7.** No call to
  `mumin_devif_init`, `mumin_devif_firm_reset` or `mumin_devif_load` happens. The
  kernel assumes the bootloader left behind:
  1. the NAND controller geometry registers (`nand_reg` 0x140/0x150/0x170/0x180/0x270),
  2. the 13-word dev-info blob at ARM7-SRAM +0x3F00,
  3. **ARM7 firmware running with SRAM+0x3F40 (firm_status) = 2**,
  4. a bad-block table (u16 list) at on-chip RAM PA 0xC0000C00 and the 20+200-byte
     `init_param`/`init_reg_param` blob at PA 0xC0001500. Only the resume path uses
     the 0xC0001500 blob (§4.4).
* Every FTL operation is then one **mailbox command**. The main CPU writes cmd/arg into
  SRAM+0x3F48..0x3F54 and writes `1` to **arm7io+0x4** (doorbell). The ARM7 writes the
  result to SRAM+0x3F5C (and +0x3F68/+0x3F6C), then sets **arm7io+0x0 = 1**, which
  raises GIC IRQ 170. The CA5 IRQ handler acks by writing 0 to arm7io+0x0.
* Arguments are trees of small structs **copied into lld's `lld_arg_area` with every
  pointer rewritten to a DDR physical address** (PA = kernel VA + 0x40000000, DDR at
  PA 0x80000000). Data buffers are scatter/gather lists `{len, phys}`. Each page has an
  8-byte "extra" (user OOB) area in a separate flat buffer.
* Result word 0 means success. §2.3 has the error table.
* Page geometry: **4096-byte main page, 256-byte spare, 64 pages/block, 2048 blocks,
  1 die, 1 plane, 8 bytes of FTL "extra" per page.** HARDWARE.md says otherwise
  ("2048-byte pages, 0x170 = blocks"). That is wrong: lld uses 0x170 as the page size
  (`get_sector_per_page` = reg[0x170]>>9 = 8 sectors, `mumin_devif_load+0x330`).
* The direct controller path (`boss_hw_nandc_*`, `PNAND_*`) is used only when
  firm_status ∈ {0,1,9}, after an error recovery, or by debug/"safe" ioctls. An HLE that
  keeps firm_status = 2 can ignore it for booting.
* ldec (cmd 0) feeds NAND data through the hardware LZ77 decoder. It backs `muminlzp`
  block devices: partitions of type 3 with an `LZPT` header. `/sbin/init` mounts
  "compressed /usr" and "compressed /system", so **cmd 0 is very likely needed to reach
  userspace** (§5).

---

## 1. Resources (lld_probe @0x3e88)

`lld_probe(dev, idx)` stores `resource[idx].va` (udif resource stride 92 bytes, VA at +16)
into the table at `.data+0x14[idx]`:

| idx | getter | name | PA | IRQ |
|---|---|---|---|---|
| 0 | `boss_lld_get_base_addr_nandc_reg` | nand_reg | 0x00020000 | 172 (`boss_hw_nandc_request_irq`) |
| 1 | `boss_lld_get_base_addr_nandc` | nandc window | 0x10000000 | – |
| 2 | `boss_lld_get_base_addr_arm7_gpio` | arm7io | 0x00011000 | 170 (`boss_hw_arm7_comm_request_irq`) |
| 3 | `boss_lld_get_base_addr_arm7_sram` (+`_phys` from res+8) | arm7_sram | 0x00000000 | – |
| 4 | `boss_lld_get_base_addr_clkrst3` | clkrst3 | 0xF2900000 | – |

Snapshot lld `.bss+0x17c` = 0xAA (170) and `.bss+0x19c` = 0xAC (172) **[S]**.
On idx 3, probe calls `boss_lld_direct_get_dev_info_save_sram` (@0x808). It copies
SRAM+0x3F00..0x3F33 (13 words) into lld `.bss+0x00` (the "dev_info" struct, §4.2).
Both IRQs are requested and then immediately **disabled** (`request_irq+0x74`). The ARM7
IRQ is enabled by `mumin_devif_get_device_info+0x1bc`.

`lld_init` (@0x4004) allocates `lld_work_area` (128 KiB), `firm_work_area` (10 KiB,
phys kept in `.bss+0x12c`) and `lld_arg_area` (10 KiB). It then sets init_flag
(`.bss+0x130`) = 1. Snapshot: work 0x4ae00000, firm 0x4be74000 (phys 0x8be74000), arg
0x4b158000 **[S]**.

Physical address conversion: `boss_lld_local_virt_to_phys` (@0x2034) is `va + 0x40000000`
(kernel PAGE_OFFSET 0x40000000 → DDR PA 0x80000000).

---

## 2. ARM7 IPC

### 2.1 arm7io registers (PA 0x00011000)

| off | name (lld) | CA5 access | meaning |
|---|---|---|---|
| +0x0 | `state_to_ca5` | R: `get_state_to_ca5` (@0x585c). W 0: `set_to_ca5` (@0x58a4), IRQ handler `comm_process+0x64`, polling ack `set_wait_trans+0x160` | ARM7→CA5 "done" flag. The ARM7 writes 1. That (presumably level) drives IRQ 170 **[?]**. The CA5 clears it by writing 0. Polling mode tests `==1`. |
| +0x4 | to_arm7 doorbell | W 1: `set_to_arm7+0x6c` | CA5→ARM7 "new request". The CA5 only ever writes 1 and never reads or clears it, so the ARM7 presumably clears it **[?]**. HLE: treat a write of 1 as a pulse. |

No other arm7io offsets are touched.

### 2.2 clkrst3 (PA 0xF2900000) — ARM7 / NAND reset

| off | use |
|---|---|
| +0xD0 | read-back status (`recovery_check+0x88`, `+0xdc`) |
| +0xD4 | **clear** reset bits: `rst_deassert` writes 1 (bit0 = ARM7 runs); `recovery_check` writes 0x1F00 |
| +0xD8 | **set** reset bits: `rst_assert` writes 1 (bit0 = ARM7 held in reset); `recovery_check` writes 0x1F01 |

bit0 = ARM7 core reset. bits 8..12 = NAND-controller-side resets **[?]**.

### 2.3 ARM7 SRAM map (PA 0x0, 16 KiB)

| off | name (from `lld_read_proc` strings) | writer | meaning |
|---|---|---|---|
| 0x0000–0x3EEF | firmware image | `mumin_devif_load` / bootloader | ARM7 code + data. `load` accepts up to 0x4000 bytes and then overwrites 0x3F00 |
| 0x3EF0 | `firm_trup_info` | ARM7 **[?]** | proc prints only the constant 0x3EF0. Contents unknown (trap/trouble log?) |
| 0x3F00–0x3F33 | dev_info (13 words) | bootloader; CA5 in `mumin_devif_init+0x110`, `mumin_devif_load+0x19c` | §4.2. The ARM7 reads geometry from here **[?]** |
| 0x3F40 | `firm_status` | ARM7; CA5 (`set_firmware_status`) writes 0 before reset/load/recovery | 0 = stopped/reset, **2 = running and ready**, 3 = fatal (every op returns −10006). For 0/1/9 the read path bypasses the ARM7 (`read_pages_in_pblkgroup+0x7c`). Exact meaning of 1/9 unknown **[?]** |
| 0x3F44 | – | – | unused by CA5 |
| 0x3F48 | `firm_cmd_type` | CA5 | command code (§3) |
| 0x3F4C | (work size) | CA5 | always 10240 (size of firm_work_area) |
| 0x3F50 | `firm_work` | CA5 | phys of 10 KiB `firm_work_area` (ARM7 scratch in DDR) |
| 0x3F54 | `firm_api` | CA5 | **phys of the command's argument block** (§3) |
| 0x3F58 | – | – | unused by CA5 |
| 0x3F5C | `firm_result` | ARM7 | result code, mapped by `boss_lld_local_change_lld_ans_from_firm` (@0x2154) |
| 0x3F60 | `firm_result_detail_num` | ARM7 | getter exists, never called |
| 0x3F64 | `firm_result_detail` | CA5 (`set_firmware_result_detail`, ldec only) | phys of the per-item ECC-status word array for cmd 0 |
| 0x3F68 | pout_data | ARM7 | per-command output 1 (§3) |
| 0x3F6C | pout_data2 | ARM7 | per-command output 2 (§3) |

`set_firmware_api_param(cmd, size, work, api)` (@0x5ee0) writes +0x3F48, +0x3F4C,
+0x3F50 and +0x3F54 in that order, each after a `dsb` and an outer-cache sync.

**Result mapping** (`change_lld_ans_from_firm`: `idx = result-11`; if idx>34 the
answer is **0**; otherwise `.rodata[idx]`). Any value outside 11..45, including 0,
means success:

| ARM7 result | lld/MUMIN code | meaning (inferred from callers) |
|---|---|---|
| 11 | 0xFFFDFFFA | ECC corrected, bit-flips ≥ threshold (data valid, refresh advised). Callers treat it as success |
| 12 | 0xFFFDFFFB | ECC uncorrectable |
| 13 | 0xFFFDFFF0 | blank/erased page **[?]**. The search commands remap it to 0xFFFDFFF7 |
| 14 | −10001 (0xFFFFD8EF) | parameter error |
| 15–30, 37–39 | 0 | success variants |
| 31–33 | 0xFFFDFFFF | device failure. Triggers `recovery_check` (NAND reset, ARM7 halted, status 0) |
| 34 | 0xFFFDFFF8 | erase failure |
| 35 | 0xFFFDFFF9 | program failure |
| 36 | −10003 (0xFFFFD8ED) | timeout. Also triggers `recovery_check` |
| 40–45 | −10004 (0xFFFFD8EC) | ? |

Other lld codes: −10002 no memory, −10006 firm_status==3, −10007 PIO read OK after
retry, −10008 not initialised / no buffers.

### 2.4 Kernel-side state (`.bss+0x158`, "arm7_lock")

| off | field |
|---|---|
| +0x00 | wait-active flag. Set 1 by `set_wait_trans`/`set_ldec_read_wait`, cleared by the IRQ handler. `confirm_state()` returns it |
| +0x04 | "completion seen" (IRQ sets 1). Cleared by `set_to_arm7`. `set_wait_trans` returns 0 only if ==1 |
| +0x08 | mode: 3 = asynchronous ldec read, otherwise synchronous |
| +0x0C | done flag for the sleeping waiter |
| +0x10 | raw spinlock. +0x18 = waiting task |
| +0x1C | IRQ enabled. +0x20 = `lock_state` counter (`spin_lock`/`_n`). +0x24 = IRQ number |
| `.data+0xC0` | mode_switch: 1 = IRQ mode (default), else polling. `.data+0xC4` = rt spinlock |

### 2.5 One synchronous request (what every `mumin_devif_*` does)

1. `boss_lld_udm_mutex(1)`, then check `firm_status != 3`.
2. Build the arg block in `lld_arg_area` (§3), rewrite pointers to phys, clean the cache
   over the arg block, clean write data / invalidate read buffers.
3. `set_firmware_api_param(cmd, 10240, firm_work_phys, arg_phys)`.
4. `boss_hw_arm7_comm_set_wait(60 000 000 µs)` (@0x5b70):
   * `set_to_arm7`: state[+0xC,+0x10,+0x14,+0x18,+0x4] = 0, `dsb`, **arm7io+4 = 1**.
   * `set_wait_trans` (@0x59c0): state[0] = 1.
     * **IRQ mode:** sleep (timeout `ceil(us/1e6)*HZ`, HZ=100, i.e. 6000 jiffies) until
       state[+0xC] != 0. Then return 0 if state[+4]==1 (and clear it), else −10003.
     * **Polling mode:** loop `us/10` times: if arm7io+0 == 1, write arm7io+0 = 0 and
       return 0; else `udelay(5)`. Timeout returns −10003.
5. On 0: `change_lld_ans_from_firm()`, read pout_data/pout_data2 if the command has them.
6. `boss_hw_nandc_recovery_check(result)`, then `boss_lld_udm_mutex(0)`.

**IRQ handler** `boss_hw_arm7_comm_process` (@0x564c): state[0]=0, state[4]=1, `dsb`,
**arm7io+0 = 0**. If state[8]==3, set state[8]=0 and call `boss_lld_ldec_read_process`.
Otherwise set state[+0xC]=1 and wake the waiter. It does not read any register, so
every IRQ 170 counts as a completion.

**HLE requirements:**
* Complete a request **in response to the doorbell write**, not earlier. `set_to_arm7`
  clears the soft flags just before ringing, so a completion that comes before the
  doorbell (for example on reset release) is lost and the CA5 times out after 60 s.
* Completing synchronously inside the doorbell MMIO write is fine. The IRQ handler then
  sets state[+0xC] before `set_wait_trans` checks it.
* Model arm7io+0 as a readable latch. Raise IRQ 170 while it is 1 and lower it on the
  write of 0, so both IRQ and polling mode work. nand.ko switches modes with
  `mumin_devif_mode_switch` (probe/resume/suspend/`wbi_close`).

### 2.6 Firmware (re)start — `mumin_devif_firm_reset` (@0x184; resume path only)

`disable_irq` → `rst_assert` (clkrst3+0xD8=1) → `firm_status=0` → `enable_irq` →
`set_to_ca5` (arm7io+0=0) → `rst_deassert` (clkrst3+0xD4=1) → `set_wait(60 s)`
(rings the doorbell) → **require `firm_status == 2`**, else return −10003 (after
`recovery_check`).

HLE: on clkrst3+0xD4 bit0, mark the ARM7 "booting". On the next doorbell, set
SRAM+0x3F40 = 2 and complete without running a command.

### 2.7 Error recovery — `boss_hw_nandc_recovery_check(err)` (@0x7a10)

This runs only for err ∈ {0xFFFDFFFF, −10003}:
clkrst3+0xD8 = 0x1F01, read +0xD0, **SRAM+0x3F40 = 0**, ~0.5 µs delay,
clkrst3+0xD4 = 0x1F00 (NAND out of reset, **ARM7 stays in reset**). It then polls nand_reg
0x410 (write-back-to-clear) for bit13 (0x2000), up to 6e6 × 5 µs. After this, reads take
the direct path (status 0) and ARM7 commands time out. Nothing restarts the ARM7 except
the resume path.

---

## 3. Command set

Common sub-structures. Layout comes from the copy code. Names are mine. "*" marks a
pointer, which the ARM7 sees as a DDR phys.

```
PBLK      (12) { u32 die; u32 plane; u32 pblk; }       // plane = index into GINFO.pblks [?]
PPAGE     ( 8) { PBLK* blk; u32 page; }
PBLKGROUP ( 8) { u32 g0; GINFO* info; }
GINFO     (12) { u32 i0; u32 n_pblks; u32* pblks; }    // pblks[] copied as 2 words (≤2 planes)
PAGEGROUP (16) { u32 die; u32 plane; u32 page; PBLKGROUP* grp; }
SG        ( 8) { u32 count; SGE* e; }    SGE (8) { u32 len; u32 addr; }
```

Facts that hold for every command:
* Data length = `npages × 4096` (`sector_per_page×512`). Data goes through `SG.e[]`
  (`addr` phys, `len` bytes). Read buffers are invalidated before and after
  (`boss_lld_local_udif_cache_invalidate_sg`), write buffers cleaned (`_flush_sg`).
* Extra/OOB = **8 bytes per page**, flat phys buffer. get_device_info reports
  extra size 8 (`+0xb0`), and every caller sizes it `n<<3`.
* "single-plane" fallback: when `GINFO.n_pblks == 1`, the *group* commands are reissued
  as the per-block command with `PBLK{0,0,pblks[0]}`
  (`boss_lld_local_change_pblkgroup` @0x22e8, `change_start_pblkgroup` @0x2248,
  `change_extragroup` @0x236c). **On this device (1 plane) the group commands are
  therefore rarely or never sent.** The HLE should still accept them.
* Arg offsets below are relative to the arg block whose phys is in SRAM+0x3F54.
  "→+N" means "phys pointer to the copy at +N of the same block".

| cmd | devif function (lld) | arg block layout | outputs |
|---|---|---|---|
| **0** | `mumin_devif_ldec_read` (async, §5) | see §5 | result + per-item ECC words at `firm_result_detail` |
| 1 | `mumin_devif_ldec_write` (@0x18b8; no caller in fw) | – | – |
| **2** | `mark_bad_pblkgroup(grp, buf16, buf4)` | +0 →+12 PBLKGROUP; +4 phys buf16 (16 B); +8 phys buf4 (4 B); +12 PBLKGROUP{g0,→+20}; +20 GINFO{i0,n,→+32}; +32 pblks[2]. 48 B | result |
| **3** | `mark_bad_pblk(blk, buf8)` (helper @0x4c08) | +0 →+8 PBLK; +4 phys buf (8 B, marker extra); +8 PBLK. 20 B | result |
| **4** | `read_extra_of_top_pages(blk, n, extra, stat)` @0x287c | +0 →+16 PBLK; +4 n; +8 phys extra (n×8); +12 phys stat (n bytes); +16 PBLK. 28 B | extra[n×8]; stat[n] (u8 per page, probably 0 ok / 1 corrected / 2 uncorrectable — encoding inferred from `read_extra_pagegroup+0xc8` **[?]**) |
| **5** | `read_pages_in_pblkgroup(pg, n, *out, sg, extra)` @0x3194 (ARM7 path @0x3644) | +0 →+20 PAGEGROUP; +4 n; +8 –; +12 →+64 SG; +16 phys extra (n×8); +20 PAGEGROUP{die,plane,page,→+36}; +36 PBLKGROUP{g0,→+44}; +44 GINFO{i0,n,→+56}; +56 pblks[2]; +64 SG{count,→+72}; +72 SGE[count]. 72+8·count B | data, extra; `*out = pout_data` = pages read (the direct path stores the same quantity, `+0x450`) |
| **6** | `read_pages_in_pblk(pp, n, *out, sg, extra)` (helper @0x2e24) | +0 →+20 PPAGE; +4 n; +8 –; +12 →+40 SG; +16 phys extra; +20 PPAGE{→+28,page}; +28 PBLK; +40 SG{count,→+48}; +48 SGE[count]. 48+8·count B | data, extra; `*out = pout_data` |
| **7** | `read_extra_of_page(pp, extra8)` (helper @0x2a18) | +0 →+8 PPAGE; +4 phys extra (8 B); +8 PPAGE{→+16,page}; +16 PBLK. 28 B | extra[8] |
| **8** | `write_pages_in_pblkgroup(pg, n, *out, sg, extra, *out2)` @0x5320 | +0 →+24 PAGEGROUP; +4 n; +8 –; +12 →+68 SG; +16 phys extra; +20 –; +24 PAGEGROUP{…,→+40}; +40 PBLKGROUP{g0,→+48}; +48 GINFO{i0,n,→+60}; +60 pblks[2]; +68 SG{count,→+76}; +76 SGE[]. 76+8·count B | `*out = pout_data` (pages written [?]); `*out2 = pout_data2` = failed-block bitmask. The single-plane fallback stores 0x80000000 on 0xFFFDFFF9, so the mask is MSB-first |
| **9** | `write_pages_in_pblk(pp, n, *out, sg, extra)` (helper @0x4fc8) | same layout as cmd 6 | `*out = pout_data` |
| **10** | `write_extra_of_page(pp, extra8)` @0x4aa4 | same layout as cmd 7 | result |
| **11** | `erase_pblkgroup(grp, *out)` @0xa40 | +0 →+8 PBLKGROUP; +4 –; +8 PBLKGROUP{g0,→+16}; +16 GINFO{i0,n,→+28}; +28 pblks[2]. 36 B | `*out = pout_data` = failed-block bitmask (single-plane: 0x80000000 on 0xFFFDFFF8) |
| **12** | `erase_pblk(blk)` (helper @0x8e4) | +0 →+4 PBLK; +4 PBLK. 16 B | result |
| **13** | `search_last_written_pages_in_pblkgroup(grp, out[2])` @0x3bf8 | same layout as cmd 11 | out[0]=pout_data, out[1]=pout_data2. Result 0xFFFDFFF0 is turned into 0xFFFDFFF7 |
| **14** | `search_last_written_page_in_pblk(blk, *out)` (helper @0x3a28) | +0 →+8 PBLK; +4 –; +8 PBLK. 20 B | `*out = pout_data` = last programmed page index **[?]**. 0xFFFDFFF0 becomes 0xFFFDFFF7 ("nothing written" [?]) |
| **15** | `read_extra_pagegroup(grp, page, extra16, stat)` @0x2b38 | +0 →+16 PBLKGROUP; +4 page; +8 phys extra (16 B); +12 phys stat (GINFO.n bytes); +16 PBLKGROUP{g0,→+24}; +24 GINFO{i0,n,→+36}; +36 pblks[2]. 44 B | extra, stat |
| **16** | `dbg_read_raw_page(pp, sg)` @0x2624 | +0 →+8 PPAGE; +4 →+28 SG; +8 PPAGE{→+16,page}; +16 PBLK; +28 SG{count,→+36}; +36 SGE[]. Raw page incl. spare **[?]** | data. Result −10003 calls `nandc_restore_raw` |
| **17** | `dbg_write_raw_page(pp, sg)` @0x2424 | same as 16 | – |

The "lower page" variants (`read/write_lowerpages_*`, `read_extra_of_lowerpage`,
`write_extra_of_lowerpage`, `search_last_written_lowerpage*`) are stubs that return
0xFFFDFFFF (MLC-only feature, unsupported on this SLC part).

Block address: `boss_lld_local_get_block_to_nanddev(plane, pblk, startblk, planes)`
(@0x20c0) returns `pblk` when planes == 1. Otherwise it returns
`pblk + startblk·(plane + (pblk/startblk)·(planes-1))`. NAND row = `pblk<<6 | page`.

---

## 4. Geometry, init and the direct NAND controller

### 4.1 nand_reg map (PA 0x00020000)

Values marked **[S]** come from nand.ko `init_reg_param` in the snapshot
(VA 0x3f03bb4c, 200 B). `boss_hw_nandc_set_init_reg` (@0x6810) writes them as
`reg[0x10+4p] = p[p]` for p=0..52 and `reg[0x20+4p] = p[p]` for p=56..164 (0xF0 is
skipped), plus three specials.

| reg | param off | [S] value | use |
|---|---|---|---|
| 0x000 | – | – | command/start: `execute_reset+0xc8` writes 1 = controller+NAND reset |
| 0x010…0x060 | p0…p20 | 0,1F4,1F4,1F4,1F4,1 | timings **[?]** |
| 0x070 / 0x080 bit0 | (p24/p28 → dev[32]/[36]) | 0/0 | multi-plane enables, toggled by helper @0x640c in mode 2 |
| 0x090…0x0D0 | p32…p48 | 0,0,0,1,1 | ? |
| 0x0E0 | p52 (\|1, &~0x7000) | 1 | ECC/format control. PIO ReadPage/WritePage set 0x4001. `restore_raw` sets 1 |
| 0x0F0 | – | – | bit0 = DMA-mode op active, bit4 = idle/CE-off **[?]** (helpers @0x66b0, @0x7d00) |
| 0x100…0x130 | p56…p68 | 1F1F,B4D,1A,C | timings **[?]** |
| **0x140** | p72 | 0 | **planes-1** (`get_plane_num`) |
| **0x150** | p76 | 0x40 | **pages per block** (`get_page_boundary` → shift 5/6/7/8 for 32/64/128/256) |
| 0x160 | p80 | 1 | ? |
| **0x170** | p84 | 0x1000 | **main page size in bytes**. Also `reg[0x6D0] = p84>>9` |
| **0x180** | p88 | 0x100 | **spare size** (devinfo "full page" = 0x170+0x180 = 0x1100) |
| 0x190…0x1B0 | p92…p100 | 0,0,0x00420010 | 0x1B0 low 5 bits forced 0x10 by PIO |
| 0x1C0/0x1D0 | p104/p108 | 0/0 | (helper @0x6548) |
| 0x1E0…0x260 | p112…p144 | 0,8,3,1,1A000,0,FFFF,1,0 | ? |
| **0x270** | p148 | 0 | **plane start block** (`get_plane_startblk`) |
| 0x280 | p152 | 0 | write-protect release: 1 around PIO program/erase (helper @0x7ba0) **[?]** |
| 0x290/0x2A0 | p156/p160 | 1A / 001F013B | ? |
| 0x2B0 | p164 | FFFF | timeout counter **[?]**. `execute_reset` writes 0xFFFF |
| 0x300 | p168 | 0xEC | **NAND maker ID (Samsung 0xEC)** [?] |
| 0x3C0 | p172 | 0 | ? |
| **0x410** | – | – | **interrupt/status, write-back-to-clear.** bit0 PIO ECC-uncorrectable, bit3 abort/error (waits return 9), bit4 program-fail, bit5 erase-fail, bit6 PIO read done, bit7 program done, bit8 erase done, bit10 status-fail, bit12 ? , **bit13 reset done**. Snapshot accumulated 0x3000 **[S]** |
| **0x420** | – | – | IRQ enable. bit3 set while an op is in flight (@0x66b0, @0x7d00) |
| 0x650 | – | – | PIO ECC result: bits[6:0] corrected bits, bit7 uncorrectable |
| 0x690 / 0x6C0 | – | – | `restore_raw`: 0x6C0=0x200, 0x690=2 |
| 0x6D0 | – | 8 | sectors per page (PIO sets 3) |
| 0x6E0 | – | 8 | extra bytes per page (PIO sets 0) |
| 0x700 bit0 | – | – | descriptor-DMA engine enable (set for each `read_data`, cleared in ISR/cleanup) |
| **0x720** | – | – | DMA status. bit1 = chain complete (wait mask 2). Acked by writing 2. Snapshot accumulated 0x40 **[S]** |
| 0x730 bit1, 0x7D0 bit0 | – | – | DMA IRQ enables (IRQ mode only) |
| 0x780 | p176 | 0x10 | ? |
| 0x7C0 | – | – | write 1 = DMA ack/kick after completion |

The **0x10000000 window** is a command/address port:
* `+0x00` = `(type<<26) | row`, type 2 = command/address phase, type 1 = data phase
  (helper @0x7c08).
* `+0x10` = command / data FIFO (32-bit).

Command words seen: 0x2001 = page read, 0x2101 = page program, 0x0001 = block erase,
0x0080 = "start descriptor chain" (followed by the descriptor phys and 0). Mode 5 also
sends 0x47 before and 0x42 after (helper @0x6344). Exact semantics **[?]**.

`nandc` IRQ 172 handler `boss_hw_nandc_process` (@0x619c):
acc0 |= reg[0x410]; reg[0x410] = acc0 (ack); 0x700 &= ~1; 0x730 &= ~2;
acc1 |= reg[0x720]; reg[0x720] = 2; 0x7D0 &= ~1; 0x7C0 = 1. Then it wakes the waiter.
The waiter (@0x727c) tests `acc & mask`. Without the IRQ it polls 0x410 (and 0x720),
6e6 × 5 µs. IRQ mode is the default (`.data+0xE0`=1).

### 4.2 dev_info (lld `.bss+0`, = SRAM+0x3F00 blob)

`[0]` start_die=0, `[1]` ibt=0xC0000C00 (u16 bad-block list, VA = PA on-chip RAM),
`[2]` ibt_size=6, `[3]` n_dies=1, `[4]` n_pblks/die=0x800, `[5]` max_bad=0x2E,
`[6]` 5, `[7]` 50000, `[8..11]` = init_reg p24/p28/p104/p108 (0),
`[12]` ECC threshold = 4 (`boss_lld_direct_get_ecc_thresh`).

`get_device_info` (@0x5a0) builds nand.ko's devinfo at lld `.bss+0xA4` [S]:
`{0, start_die, n_dies, 1, planes(1), 0x1100, 0x1000, 8 (extra), 0x40, 0, n_pblks/planes, max_bad, 5, 50000}`.
It also builds the ops table (`.bss+0x34`, 28 function pointers, order at
`get_device_info+0xd4`).

`get_initial_bad_pblks` (@0x84) walks `ibt[0..ibt_size)`. On the real device the list is
the factory bad blocks plus the 6 reserved blocks `{0,1,2,3,4,5}` (nand.ko
`mumindrv_init` appends `.rodata+0x10` and bumps collisions). **The HLE board should
place `00 00 01 00 02 00 03 00 04 00 05 00 FF FF` at PA 0xC0000C00** **[?] reconstructed**.

### 4.3 Direct read: `boss_hw_nandc_read_data(req, nplanes)` (@0x759c)

Used by `mumin_devif_load` and by `read_pages_in_pblkgroup` when status ∈ {0,1,9}.
req: `[0]` block, `[4]` page, `[8]` page shift (6), `[16]` mode (3 = full pages, 5 =
sector range, 2 = multi-plane), `[20]`=64, `[24]` count, `[28]`=1, `[32]` data phys,
`[36]` extra phys. It builds a 64-byte descriptor (32-aligned kmalloc):

```
+0  0          +4  0 (next?)       +8  row = (blk<<shift)|page|0x08000000
+16 u16 count|0x2000   (mode 5: count = sector_off<<4 | nsectors)
+18 u16 0x40|0x100|mode<<12        +24 data phys     +56 extra phys
+32 u16 STATUS (written by HW)     everything else 0
```

Sequence: `0x66b0(1)` (0x420|=8, 0xF0 = (…&~0x10)|1), 0x700|=1, [IRQ mode: 0x730|=2,
0x7D0|=1, enable IRQ]. Window: +0=0x08000000, +0x10=0x80, +0x10=desc_phys, +0x10=0.
Wait (0x720, mask 2), then `0x66b0(0)`. Then poll the descriptor's STATUS (up to
100000 × cache-invalidate) for bit15. STATUS decode:
* bit14 set: bit1 → code 4; else bit10 → code 9; else code 7.
* bit14 clear: bit11 → code 8; else bits[9:4] = corrected bits, ≥ thresh → code 1,
  else code 0.

Codes map via `.rodata+0x100`: 0→0, 1→FFFDFFFA, 2→−10007, 3→−10001, 4→FFFDFFFF,
5→FFFDFFF9, 6→FFFDFFF8, 7→FFFDFFFB, 8→FFFDFFF0, 9→−10003.

`read_pages_in_pblkgroup` direct path (@0x321c): for each SG chunk it issues mode 3
(whole pages) or mode 5 (partial sectors). The extra phys points at the caller's extra
buffer or a stack scratch.

### 4.4 Init / reset / PIO

* `boss_hw_nandc_execute_reset` (@0x7490): reg 0x410 = 0x2000; IRQ on (IRQ mode);
  0x2B0 = 0xFFFF; `0x66b0(1)`; **reg 0x000 = 1**; wait 0x410 bit13 (0x2000); `0x66b0(0)`.
* `mumin_devif_init(init_param)` (@0x420; boot_mode=1 and resume only): requires
  status != 3. Calls `execute_reset`, then `set_init_reg(init_param[4])`. It refills
  dev_info from init_param/init_reg (`[0..2]`←ip[0..2], `[12]`←ip[3], `[3..7]`←reg
  p180..p196, `[8..11]`←p24,p28,p104,p108) and copies dev_info to SRAM+0x3F00.
  `init_param` comes from on-chip RAM PA 0xC0001500 (20 B) + 0xC0001514 (200 B,
  `set_devif_init_param` in nand.ko, which forces p4,p8,p12=500). Snapshot copy:
  `ip = {0, 0xC0000C00, 6, 4, &reg}`. `reg` = the 50 words listed in §4.1, then
  p180..p196 = `1, 0x800, 0x2E, 5, 0xC350`.
* PIO `boss_hw_nandc_ReadPage(row, n, buf)` (@0x7e44, via `PNAND_ReadPage(blk, page, n,
  shift, buf)`): saves 0xE0/0x1B0/0x6D0/0x6E0. Sets 0x6E0=0, 0x6D0=3, 0xE0=0x4001,
  0x1B0=(…&~0x1F)|0x10. Per page (≤6 retries): 0x410=0x41; win+0=(2<<26)|row;
  +0x10=0x2001; wait 0x410&0x41; win+0=(1<<26)|row; read **384 words (1536 B)** from
  +0x10. Status from 0x410 bit0 / 0x650. PIO pages are therefore a special 3-sector,
  no-extra format (boot-area / "safe" ioctls).
* PIO WritePage (@0x8220): 0x280=1; win+0=(2<<26)|row; +0x10=0x2101; 0x410=0x490;
  win+0=(1<<26)|row; write 384 words; wait 0x410&0x490; fail if `status & 0x410`.
* PIO Erase (@0x8568): 0x280=1; 0x410=0x520; win+0=(2<<26)|row; +0x10=1; wait
  0x410&0x520; fail if `status & 0x420`.
* `PNAND_*` (@0x38c8..) are used only by nand.ko `nand_ioc_p{read,write,erase}_safe`
  ioctls.

---

## 5. ldec — `mumin_devif_ldec_read` (@0xecc), cmd 0

**What it is:** a batched, **asynchronous** "read NAND pages through the LZ77 hardware
decoder (ldec @0xF2903000, IRQ 171) into a destination buffer" request. The caller is
nand.ko `muminlzp_request` (block devices for partitions of type 3 whose header magic is
0x4C5A5054 "LZPT", `muminlzp_register`), then `mumin_devif_wait_trans`. lld only calls
`boss_hw_ldec_lock(1/0)`. nand.ko imports no ldec symbols. **So the ARM7 firmware
itself programs the ldec hardware.**

The kernel has a software `lz77_inflate` (vmlinux 0x4018d550) and `lz77_deflate`
(used by `ldec_write`). That is very likely the same stream format, so an HLE can port
it **[?]**.

**Needed for cold boot? Very likely yes.** `/sbin/init` mounts "/usr as ext2
(compressed)" and "compressed /system", and falls back to "no-compress"/cramfs/vfat only
on failure.

Request entry (allocated from `lld_work_area`+16 KiB, 32-aligned, `56·n+44` bytes):
```
+0/+4  list_head          +8  kfree ptr          +12 ECC-status array ptr (1 u32/item)
+16 → +20 (the API ARG: SRAM+0x3F54 = phys(entry+16))
+20 LDEC_REQ copy {n_items, items*→+44, req[2], req[3], dst_phys(+36), dst_len(+40)}
+44 items[n] (12 B each) {PAGEGROUP* →item area, w1, w2}
+44+12n  per item 44 B: PAGEGROUP(16) PBLKGROUP(8) GINFO(12) pblks[2](8)
```
`req[2]`, `req[3]`, items.w1 and items.w2 are not interpreted by lld. They are probably
the source page count/offset and compressed length **[?]**. Look at nand.ko
`mumin_map_tulist` (@0x19ac) to pin them down.

Launch (@0xc90): `set_firmware_result_detail(phys(entry[12]))`,
`set_firmware_api_param(0, 10240, firm_work, phys(entry+16))`,
`set_ldec_read_wait()` (state[0]=1, state[8]=3, doorbell). The completion IRQ goes to
`boss_lld_ldec_read_process` (@0x175c). It maps the result (FFFDFFFB/FFFDFFFA are
recorded as soft ECC events, others are fatal), invalidates dst, and chains the next
queued entry. When the list is empty it sets int_state (`.data+0x10`) = 2,
`ldec_lock(0)` and wakes `wait_trans`.

HLE for cmd 0: read the listed pages, inflate into `dst_phys` (≤ dst_len), write 0 into
each item's ECC word, result 0, raise IRQ 170. Polling mode cannot carry ldec.

---

## 6. Minimal HLE behaviour

**Board (bootloader contract):**
1. ARM7 SRAM: dev_info blob at +0x3F00 (already present) and **+0x3F40 = 2**.
2. nand_reg reset values: 0x140=0, 0x150=0x40, 0x170=0x1000, 0x180=0x100, 0x270=0,
   0x6D0=8, 0x6E0=8. Ideally load the whole §4.1 [S] table.
3. On-chip RAM: BBT at 0xC0000C00 (§4.2). init_param + init_reg_param at 0xC0001500
   (§4.4), needed for resume and boot_mode=1.

**ARM7 HLE (inside `cxd90014-nand`):**
* arm7io+4 write 1 → if "booting" (clkrst3 bit0 just released): status=2, else execute
  `cmd=SRAM[0x3F48]` on `arg=SRAM[0x3F54]` (DDR phys) against a backing store (per
  page: 4096 B data + 8 B extra, erased = 0xFF). Then write SRAM[0x3F5C]=result (0 OK,
  12 uncorrectable, 34 erase-fail, 35 program-fail) and pout words, set arm7io+0=1, and
  raise IRQ 170.
* arm7io+0: read returns the latch, write 0 clears and lowers IRQ 170.
* clkrst3 +0xD4/+0xD8 bit0 → ARM7 run/reset. Bits 8–12 NAND reset → set nand_reg
  0x410 bit13 when released (for `recovery_check`).
* Must support cmds 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 2, 3, 15 and 0 (ldec).
  16/17 are debug only.

**Backing store caveat:** `backup/NAND.BIN` (506 462 208 B) is a *logical* partition
dump, not raw pages + extra. MUMIN keeps its mapping in the 8-byte extras, so booting
needs either a raw dump or an image rebuilt into MUMIN on-flash format.

**Not needed for cold boot (boot_mode 0):** `mumin_devif_load`, `firm_reset`,
`mumin_devif_init`, `execute_reset`, `set_init_reg`, the descriptor-DMA path and PIO —
unless firm_status is left ∈ {0,1,9} or an error triggers `recovery_check`. dmac.ko is
not used by lld.

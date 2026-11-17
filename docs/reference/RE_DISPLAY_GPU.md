# RE: graphics & display output (GPU / MA allocator / scan-out / HDMI / CEC)

Static RE for the `sony-a5000` QEMU machine, firmware 1.10. Everything here comes from the stock binaries:
`rootfs_usr/kmod/{grm_gles,grm_ma,hdmi,cec}.ko`, `lib/libObj.so` (the decompile is in `re/ida/libObj.so.c`),
Android `nflasha16/lib/{libDMPSUGIRM.so,egl/libGLESv2_dmp.so}`, `nflasha3/av-cam.bin`, `emulator/re/vmlinux.elf`,
and the real-camera WBI RAM snapshot (`emulator/tools/wbi.py`).
Claims marked **[?]** are inferred and not yet proven. New IDA outputs are in `re/ida/gfx/gfx_*` (windev `C:\a5000\emu\gfx_*`).

---------------------------------------------------------------------------------------------------
## 0. Big picture (read this first)

```
 Linux user space                                   Linux kernel                 hardware
 ───────────────────────────────────────────        ─────────────────────        ─────────────────────────────
 im.elf → libObj.so  (GRM / ux gfxeng / Rigel WS)                                 GPU "GPE" = DMP SMAPH-S30
   rm_reg_*  ── mmap(/dev/mem, PA 0xF1000000, 16K) ──────────────────────────────▶ regs 0xF1000000..0xF1003FFF
   ioctl(/dev/dmpgles2) wait-IRQ/suspend/sem ───▶ grm_gles.ko (IRQ 76, clock/rst) ◀ IRQ 76 (GIC SPI 44)
   libma  ── /dev/mem + grm_ma.ko (memspace bookkeeping; memory from dmm.ko)       DDR carve-outs 0x8D7A0000..
 system_server/surfaceflinger (Android chroot)
   libDMPSUGIRM.so  (same rm_reg_* driver, same GPU, /android/dev/dmpgles2)
   gralloc.diadem → Rigel window (WS_IMAGE buffers in ws_image_space)
        │ OSD buffer PA by osal message
        ▼
 av-cam.bin  ("LIRO" RTOS image, loaded by liro.ko, runs as a kernel thread; linked at VA 0x43541000)
   VDF (display), SDF, VEF, ADF ...  ── programs the scan-out/panel hardware ─▶ panel controller (GPIO + SIO serial),
                                                                             scan-out block: NOT YET IDENTIFIED [?]
 hdmi.ko / cec.ko : HDMI TX link layer (HPD/EDID/TMDS/infoframes), polled, no IRQ ─▶ 0xF2A24000 / 0xF3C10000
```

Key consequences for the emulator:
1. **The GPU is driven from user space.** `grm_gles.ko` only does clock/reset, IRQ acknowledge, a 128 KiB shared page,
   semaphores, and an IRQ-wait ioctl. Command submission is plain register writes by `rm_reg_*` (libObj / libDMPSUGIRM)
   through `/dev/mem`. A GPU model has to sit at the MMIO + DMA boundary.
2. The GPU register layout is **the same family as DMP PICA200 (Nintendo 3DS GPU)**: PSC memory-fill at +0x10,
   PPF display-transfer at +0xC00, P3D internal registers at +0x1000 (reg index·4), and the user-space names `rm_reg_kick_ppf`,
   `rm_reg_kick_3d`. The public 3DS documentation (3dbrew "GPU/External Registers", Citra) therefore describes most of the
   2D blocks. The shader ISA (ES 2.0, vertex and fragment) is a different, DMP-proprietary design.
3. **Linux does not program the display controller.** Scan-out (LCD panel, HDMI video path, OSD/YUV planes) is set up by
   **VDF inside av-cam.bin**, which runs as kernel code (`liro.ko` loads it with `init_module`-style params
   `adr=… fn=/system/av-cam.bin load=1`, see im.elf `sub_B368` @0xB368). So emulating av-cam's peripherals is what will
   eventually light up the "real" screen. Until then a **scan-out HLE** that shows known guest-physical OSD/window buffers is
   the practical way to get a QEMU window (§5).

---------------------------------------------------------------------------------------------------
## 1. Address map relevant to graphics

Static kernel mapping (`cxd90014_map_io` @0x4033b7f0, `map_desc` table @0x40357030):

| VA | PA | size | note |
|---|---|---|---|
| 0xF0000000 | 0xF0000000 | 64 MiB | **identity**: all APB/AHB peripherals 0xF0000000–0xF3FFFFFF (GPU, HDMI, CEC, clocks) |
| 0xF8000000 | 0xF8000000 | 1 MiB | MPCore private (SCU/GIC/TWD), L2C |
| 0xFA000000 | 0xFA000000 | 64 MiB | (av-cam also uses 0xFC0xxxxx) |
| 0xF8100000 | 0x38000000 | 1 MiB | ? |

Graphics-related MMIO:

| Block | PA | size | used by | IRQ |
|---|---|---|---|---|
| GPU (SMAPH-S "GPE") | **0xF1000000** | 0x4000 | grm_gles.ko (hard-coded, not in UDIF table), libObj/libDMPSUGIRM via /dev/mem | **76** (GIC ID; SPI 44), `request_threaded_irq(76, sugilite_interrupt, NULL, 0x84 = IRQF_SHARED\|TRIGGER_HIGH, "dmpgles2")` |
| GPU clock/reset | 0xF3400080/84, 0xF3400280/84 | — | grm_gles `sugilite_clock_up/down_internal` | — |
| GPU clock dividers [?] | 0xF28040C4, 0xF28041A8, 0xF28050B8, 0xF280518C | — | same | — |
| HDMI TX | 0xF2A24000 | 0x4000 | hdmi.ko (UDIF "hdmi") | none (polled) |
| HDMI audio ("hdmiaudi") | 0xF2A20000 | 0x2C | hdmi.ko `hdmi_task_initialize` @0xb08c | — |
| HDMI "hdmicolo" | 0xF292C000 | 0x0C | hdmi.ko `hdmi_task_initialize` | — |
| CEC | 0xF3C10000 | 0x190 | cec.ko (UDIF "cec") | none in the UDIF table |

---------------------------------------------------------------------------------------------------
## 2. GPU: DMP SMAPH-S30 ("SUGILITE" driver, "GPE HW")

### 2.1 Identification
* `grm_gles.ko` modinfo: `author=DMP/Sony description=SUGILITE Driver version=0.6`, source `/vobs/Guiengine_131H/driver/grm_gles/src/sugi_{hw,dev}.c`.
* Runtime: `GL_RENDERER=SMAPH-S30`, OpenGL ES 2.0, EGL 1.4 (NOTES §11.4).
* User-space driver symbols (libDMPSUGIRM.so, statically also in libObj.so): `rm_reg_init/exit/read/write/write8/kick/kick_3d/kick_ppf/kick_bypass/wait_interrupt/suspend/resume/signal_send/signal_wait/mlock_gain/mlock_release/shm_gain/tv_gen_start/stop`, `rm_rqueue_*`, `rm_kick_thread_init`, `dmp_allocator_*`. The GLSL compiler runs on the device: `dmp_glslc_compile_shader/link_program/shader_assembly` in libGLESv2_dmp.so.
* The **PICA200-style layout** (PSC/PPF/P3D) is shown by the register use in §2.4.

### 2.2 Kernel driver `grm_gles.ko` (source of truth for the kernel/IRQ contract)
`init_module` @0x144 (.init.text):
1. `Backup_nread(0x3E0003,4)` and `Backup_nread(0x3E0009,4)`: **if either fails → `BUG()`** (sugi_hw.c:1111/1113).
   `async_fix = (v1 == 0x500 && v2 == 0)`, stored in `.data[0]`. Real camera: `.data[0] = 0` (snapshot VA 0x3f347ab0), so async fix is **disabled**.
   Printed as `"!!! GPE HW: async fix 0x%08lx 0x%08lx -> %s."`; when enabled it also prints `arb. setting = 0x22222113, bus bandwidth 4/128`.
2. `sugilite_clock_up_internal` @0x248:
   ```
   for r in (0xF28040C4, 0xF28041A8, 0xF28050B8, 0xF280518C): r = (r & ~0xF) | 6     ; (down: |7)
   *0xF3400080 = 0x15 ; udelay ; *0xF3400280 = 0x10
   if (*0xF3400080 != 0x15) BUG()        ; sugi_hw.c:221 → read-back must be exactly 0x15
   ```
   `sugilite_clock_down_internal` @0x1a8: `*0xF3400084 = 0x15; *0xF3400284 = 0x10;` plus the dividers `|7`.
   ⇒ model 0xF3400080 as a register that reads back the last value written (or set/clear pairs +0x80/+0x84, +0x280/+0x284 with
   +0x80 starting at 0 so it reads 0x15 after the set). [? set/clr semantics]
3. `sugilite_dev_init` @0x13c4: kmalloc dev (0x10C bytes): `+0xEC=2` (version), `+0xF4=0, +0xF8=0xF1000000` (bar0 VA), `+0xFC=0, +0x100=0xF1000000` (bar0 PA),
   `+0x104=0, +0x108=0x4000` (size); `vmalloc(0x20000)` shared area (+0xAC); 3 semaphores (+0xB0/+0xC4/+0xD8, 20 B each).
4. `sugilite_chdev_register` (.init @0x0): cdev `udif_devnodes["dmpgles2"]` (real devt 248:29), `request_threaded_irq(76, …, 0x84, "dmpgles2", dev)`, then `sugilite_register_init`.

`sugilite_register_init` @0x54 (also run after every resume/reset). Writes (offsets from 0xF1000000):
```
+0x10C0 = 0xFFFFFFFF      +0x10C4 = 0xFFFFFFFF      +0x1000 = 0
+0x1080 = 0x12345678      +0x10C0 = 0xFFFFFFF0      +0x10D0 = 0
+0x1100 = 1               +0x1128 = 0               +0x1110 = 0
+0x1114 = 0x76543210      +0x1118 = 0xFEDCBA98      +0x1410 = 0
if async_fix: +0x50 = 0x22222113, +0x54 = 0x80, +0xC0 = 4
+0xC1C = 0                +0x04 = 0x00070100        +0x1000 = 0
+0xC18 = 0                +0x1C = 0x00010000        +0xC0 = dev[+0xF0] | 0x20000000
```
No register is read back in init, so **a plain RAM register file is enough for probe**.

`sugilite_interrupt` @0x644 (IRQ 76):
```
st = rd(+0x34); if (st == 0) BUG()           ; sugi_hw.c:283 → never assert IRQ 76 with +0x34 == 0
if (st & 0x80000000) wr(+0x1000, 0)          ; P3D done  → ack
if (st & 0x40000000) wr(+0xC18, 0)           ; PPF done  → ack (clears trigger reg)
if (st & 0x04000000) wr(+0x1C, 0)            ; PSC0 done → ack (clears PSC ctrl)
if (st & 0x0000000F) { wr(+0x3C,1); wr(+0x44,1) }   ; other sources (bits 0-3) [?]
st2 = rd(+0x34)
dev->int_status(+0xA8) |= (st & 0xFFFF0000) | (st2 & 0xFFFF0000)
wake_up(&dev->wq)                             ; return IRQ_HANDLED
```
So the IRQ is level-high and **must drop once the acks above are written** (otherwise it storms). Status bits 16–31 are the
completion flags that user space waits for.

`sugilite_ioctl` @0x8e8 — magic 0x82, all args are one `u32`:

| cmd | nr | action |
|---|---|---|
| 0x80048200 | 0 | get version (`+0xEC` = 2) |
| 0xC0048201 | 1 | get bar *VA* [idx] (`+0x104+4·idx`, idx1 → 0xF1000000? [?]) |
| 0xC0048202 | 2 | get bar *PA* [idx] (`+0xFC+4·idx`; idx 0 returns 0x80000000; **idx 1 → 0xF1000000**, used by rm_reg_init) |
| 0xC0048203 | 3 | **wait IRQ**: mask = arg & 0xC4000000; sleeps on `int_status & mask`; timeout = 3000 jiffies (or `100·tbl[n]/5` if async_fix). **On timeout: clock down/up, `sugilite_register_init`, osal error report, returns the mask with bit24 (0x01000000) set**. Up to 10 retries. Returns the matched bits and clears them in `int_status`. |
| 0x40048204 | 4 | nop |
| 0x80048205 | 5 | get `+0x104` |
| 0x80048206 | 6 | get bar size (0x4000) |
| 0x80048207 | 7 | returns 0x200 |
| 0xC0048208 | 8 | suspend (clock down, `+0x48=1`) |
| 0xC0048209 | 9 | resume (clock up + register_init) |
| 0x8004820A | 10 | get suspended flag (`+0x48`) |
| 0xC004820C | 12 | atomic ++`+0x44` |
| 0xC004820D | 13 | wait per-pid event (completion, 10 000 000 jiffies) |
| 0xC004820E | 14 | signal event to a pid (returns 0x1FFFFFFF if none) |
| 0xC004820F | 15 | "hw bypass": arg∈{0,1} → `+0xF0=arg`, **wr(+0xC0, arg\|0x20000000)** |
| 0xC0048211 / 12 | 17/18 | down / up semaphore[arg∈0..2] |
| 0x80048263 | 0x63 | get async_fix flag |

`mmap` (`sugilite_mmap_dev` @0x1248): length 0x4000 → `remap_pfn_range(PA 0xF1000000)`; length 0x20000 → the vmalloc shared page.
(rm_reg_init actually maps the registers through `/dev/mem`, `mmap(NULL, 0x4000, RW, SHARED, memfd, 0xF1000000)`, libObj @0x5e97f0.)

### 2.3 User-space submission path (libObj.so `rm_*`, @0x5e97f0…0x5ea4c0)
* `rm_reg_init` @0x5e97f0: open `/dev/dmpgles2`, open `/dev/mem`, ioctl 6 (size), ioctl 2 idx 1 (PA), mmap the regs, ioctl 0x63
  (workaround flag: prints `"!!! GPE HW: PSC dummywrite %s."`; if set allocates a dummy-write buffer).
* Register write `sub_5E9DC0(off,val)` (writes are skipped for off 0x1800 while in "tv" mode); read `sub_5EA010`.
* Request execution `sub_5EA170(req)` @0x5ea170, `req[1]` = type:

| type | engine | registers written (offset: value) | completion bit / ack |
|---|---|---|---|
| 1, 7, 8 | **P3D command list** | `+0x1804 = req[16]` (list address), then `+0x1800 = req[17]` (size/kick) — `sub_5E9F90` | bit31 (0x80000000), ISR writes `+0x1000=0` |
| 2 | **PPF display transfer / texture copy** (`sub_5E9E00`) | `+0xC00 = src>>2`, `+0xC04 = dst>>2`, `+0xC08 = w0\|h0<<16`, `+0xC0C = w1\|h1<<16`, `+0xC10 = flags`, `+0xC18 = trigger (default 1)`; if `flags&8` (texture copy): `+0xC20 = size`, `+0xC24`, `+0xC28` instead of C08/C0C | bit30 (0x40000000), ISR writes `+0xC18=0` |
| 2 (small, w<32 or h<32 and not copy) | CPU fallback `sub_5E92E0` (linear↔8×8-tiled, 16/32 bpp) | none | — |
| 3 | **PSC memory fill** | `+0x10 = start>>3`, `+0x14 = end>>3`, `+0x18 = value`, `+0x1C = ctrl` (default 0x0F010201) | bit26 (0x04000000), ISR writes `+0x1C=0` |
| 9 / 10 | bypass on/off (ioctl 15) | `+0xC0` | — |
| 6 | event/signal | ioctl 14 | — |

`sub_5EA030` maps type → wait mask (1/7/8→0x80000000, 2→0x40000000, 3→0x04000000) and calls ioctl 3. If the returned
value has bit24 set (timeout), `sub_5EBEF0` runs recovery and `sub_5E9CD0` logs `52`.

PSC "dummy write" workaround (only when the flag from ioctl 0x63 is 1): before/after a 3D job it fills a small buffer with
0xDEAD0634 / 0xCAFECAFE, ctrl 0x80001 / 0xF0001.

On the real camera the flag is 0, so this path is not used.

Since the same layout appears in PICA200, the 3DS documentation should apply [? to be checked against sub_5E92E0]:
* PSC ctrl: bit0 = start/busy, bit1 = done, bits 8–9 = fill width (2 = 32 bit), bits 16+ = ? (0x0F01xxxx).
* PPF flags (+0xC10): bit0 flip-V, bit1 linear→tiled, bit3 texture-copy, bits 8–10 input format, bits 12–14 output format
  (0 RGBA8, 1 RGB8, 2 RGB565, 3 RGB5A1, 4 RGBA4), bits 24–25 downscale. **Addresses are `>>2` here, whereas on 3DS they are `>>3`.**
* The tiling (`sub_5E92E0`) is 8×8 blocks, 16 or 32 bpp, consistent with PICA Morton 8×8 tiles.

### 2.4 Register map summary (0xF1000000 + off)

| off | name (by analogy) | access | meaning / evidence |
|---|---|---|---|
| 0x0004 | ? | W | init 0x00070100 |
| 0x0010–0x001C | PSC0 start>>3 / end>>3 / value / ctrl | W | type 3; ack: ISR writes 0 to 0x1C on bit26 |
| 0x001C | PSC0 ctrl | W | init 0x10000 |
| 0x0034 | **IRQ status** | R | bit31 P3D, bit30 PPF, bit26 PSC0, bits0-3 other; ISR BUGs if 0 |
| 0x003C, 0x0044 | IRQ ack for bits0-3 [?] | W | ISR writes 1 |
| 0x0050, 0x0054 | bus arbiter / bandwidth | W | 0x22222113 / 0x80 (async fix only) |
| 0x00C0 | bus/bypass ctrl | W | `0x20000000 \| bypass`; 4 when async fix |
| 0x0C00–0x0C28 | **PPF** (display transfer / texture copy) | W | type 2; 0xC18 = trigger, cleared to 0 by ISR; 0xC1C init 0 |
| 0x1000 | P3D IRQ ack / "GPUREG 0x000" | W | init 0, ISR writes 0 on bit31 |
| 0x1080 | P3D reg 0x20 | W | 0x12345678 (init pattern) |
| 0x10C0/0x10C4 | P3D reg 0x30/0x31 (irq mask?) | W | 0xFFFFFFFF, then 0xFFFFFFF0 |
| 0x10D0 | P3D reg 0x34 | W | 0 |
| 0x1100–0x1128 | P3D regs 0x40–0x4A | W | 1, 0, 0x76543210, 0xFEDCBA98 (reg 0x45/0x46 ≈ PICA attribute permutation?) [?] |
| 0x1410 | P3D reg 0x104 | W | 0 |
| **0x1800 / 0x1804** | P3D reg 0x200/0x201: **command buffer size / address** | W | type 1/7/8 kick, 0x1800 written last |
| 0x2000–0x3FFF | ? (inside the 16 KiB BAR, never touched by the kernel) | — | |

The command-list format (what 0x1804 points to) is the P3D register-write stream generated by libGLESv2_dmp
(`dmp_callback_command`, `__ctx_*`). It is not decoded yet. In PICA200 it is a list of
`[value][hdr: reg id(16) | byte-mask(4) | extra-count(8) | consecutive(1)]` words [? same here].

### 2.5 GPU memory
* All GPU addresses are **guest-physical** (DDR). The GLES heap is the MA memspace `gl_common` at **PA 0x8D7A0000, 8 MiB**.
  Window buffers are in `ws_image_space` at **0x8E750000, 16 MiB** (§3).
* The 128 KiB shared vmalloc page (ioctl mmap 0x20000) is IPC between im.elf and surfaceflinger (locks, pids, tv logging). It is not hardware.

### 2.6 Can we run GLES2 without a real GPU model? Options
1. **Probe-only stub** (needed regardless): RAM register file plus the clock regs. Boot then continues, but **every job
   blocks for 3000 jiffies (30 s at HZ=100) and then triggers a GPU reset**, so the UI is unusable.
2. **Completion stub** (cheap, recommended first): on a write to 0x1800 (P3D), 0xC18 (PPF trigger) or 0x1C (PSC ctrl, bit0),
   set the matching status bit in 0x34 and raise IRQ 76 after a short delay. Clear the bit (and lower the IRQ when 0x34 reaches 0)
   on the ISR acks (write 0 to 0x1000 / 0xC18 / 0x1C). Nothing hangs. Implement **PSC and PPF functionally**: they are simple
   (fill; linear/tiled copy with format conversion; documented by the 3DS analogy), and Rigel/GRM 2D paths
   (`GRM_gpermFillrect/Rectblit`, `DMP_2D_Initialize`) probably rely on them [?]. P3D output stays blank.
3. **Full P3D software rasteriser at the register boundary**: decode the command list (P3D register writes), implement the
   fixed-function state (viewport, depth/stencil, blend, texture units, framebuffer formats incl. 8×8 tiling), plus a
   **DMP vertex and fragment shader ISA interpreter**. The ISA is undocumented, but the on-device assembler
   (`dmp_glslc_shader_assembly` in libGLESv2_dmp.so, 1.2 MB) emits it. Its encoder tables can be reverse-engineered
   statically, or recovered by running the compiler in the emulator and diffing outputs. Feasible but large
   (estimate: weeks); only worth doing if the Rigel compositor (`WindowServerEGL`) really draws the final OSD with 3D.
4. An "HLE GLES" at the ioctl boundary is **not possible**: the command stream goes through MMIO and RAM, not ioctls.

---------------------------------------------------------------------------------------------------
## 3. Memory allocator `grm_ma.ko`
* No MMIO. Pure software: a dlmalloc `mspace` per "memspace" (`create_mspace_with_base`, `mspace_*`), a list (`head` @.data+0x80),
  `/proc/ma_memspace_info`, cache maintenance (`ma_cache_flush/invalidate_impl` → `___dma_single_cpu_to_dev`).
* Physical memory comes from **`dmm.ko`** via osal messages (`ma_dmm_open_send_recv` @0x1fc / `ma_dmm_close_send_recv` @0x3b8;
  dmm is loaded with `DmmConfig=/usr/share/dmm/DmmConfig.bin`, a copy is in `re/backup/setting/DmmConfig.bin`). So grm_ma needs
  nothing from the machine model; dmm needs whatever its config describes (out of scope here).
* Memspace object (from `ma_list_add` @0xd5c and the procfs callback @0x1170): `+0 u16 uhdl, +2 s16 refs, +8 phys, +0xC size,
  +0x10 type, +0x14 list_head, +0x1C name[16], +0x2C has_mspace`.
* **Live list on the real camera** (WBI snapshot, list head 0x3f341dcc):

| name | uhdl | phys | size | kind |
|---|---|---|---|---|
| gl_common | 0x421 | 0x8D7A0000 | 8 MiB | mspace (GLES heap) |
| ws_image_space | 0x427 | 0x8E750000 | 16 MiB | mspace (Rigel window buffers) |
| mpMemoryHeap | 0x429 | 0x8DFB0000 | 3 MiB | mspace |
| mpTextHeap | 0x42A | 0x8E6B0000 | 640 KiB | mspace |
| mpMemoryHeap2 | 0x42B | 0x8E2B0000 | 4 MiB | mspace |
| (unnamed, monolithic) | 0x428 | **0x8F758000** | **0x96000** | 640×480×16bpp: OSD buffer A [?] |
| (unnamed, monolithic) | 0x42C | **0x8F7EE000** | **0x96000** | OSD buffer B [?] |
| BufferManager ×2, unnamed ×2 | — | 0x8E7501E8, 0x8E9B2B00, 0x8EC14E00 | 0x68C, 0x131080 | sub-allocations (window buffers) |

* Window buffer descriptors found at the start of ws_image_space (snapshot @0x8E750204…):
  `{size, kind, PA, stride, (h<<16|w), fmt, flag, pad}`. Examples:
  `0x131080, 1, 0x8E750900, 0xA00, 0x01E00280 (640×480), 0x1000, 0, 0x5080` and the same at 0x8E881A00 (32 bpp, gralloc/Android);
  `0x131080, 2, 0x8E9B2B00 / 0x8EC14E00, 0xA00, 640×480, 0x133, 1, 0x5080` (32 bpp double buffer, UX main window [?]);
  `0x98880, 4, 0x8EAE3C00 / 0x8EB7C500, 0x500, 640×480, 0x133, 0, 0x2880` (16 bpp double buffer).
  Buffer 0x8E750900 is filled with 0xFF000000 (opaque black ARGB) in the snapshot, so 32-bit pixels are **A8R8G8B8 little-endian words** [?].

---------------------------------------------------------------------------------------------------
## 4. Display controller / panel (scan-out)

### 4.1 Who programs it
* No Linux kernel module touches a display controller (no fbdev, no `/dev/fb0`; the modules list in the snapshot is:
  devno dmac ldec lld nand osal_utm osal_uipc utimer backup osal_ulogio input udate indctr i2c sircs stream stream2
  usb_portMonitor usbg_sen usb_extcmd kikilog dmm upm nfc grm_ma grm_gles hdmi cec accel mag compass mcmn_drv ms_drv mmc_drv sprop liro).
* libObj only talks to VDF through osal messages (`vdf_if_port_init` @0x5dc640, `OBJAVBBCBASECLASS::VdfCh::*`:
  `setYUVLayout, getPanelOut, getOSDLayout, setPanelColor, drawFocusLockFrame…`).
* **VDF lives in `av-cam.bin`**: classes `VDF::OsdImageProcessor, VdfDisplayCmdGetPanelEvf, VdfDisplayCmdSetHdmiInput,
  VdfSrvcDrawOsdManager, YuvIndexDoubleBuffer`, `PanelSysvPrmGen(DISP_CMD_PANEL_SYSV*)`, `[PNL] …` panel sequencing,
  `disp_evf_*`, `tsk_sdf_vsync`.
* av-cam.bin: raw image, magic `"ORIL"` at +4, **linked at VA 0x43541000** (string-xref base search: 812 hits vs ≤42 for other bases),
  i.e. kernel lowmem, PA 0x83541000. It is loaded and started by `liro.ko` ("LIRO … 4-MUSASHI", params `adr fn load debug delay alba
  resume_cpu`, `tsk_entry`, `os_thread_bind`). It runs at kernel privilege on a Cortex-A5 core and uses the kernel's identity map
  for 0xF0000000–0xF3FFFFFF. (The other agent's `boot_liro.*` on windev covers liro.ko itself.)

### 4.2 What is known about the panel path
* Panel control (`[PNL]` functions, e.g. `panel_out_set` @0x4361D710, `panel_set_backlight` @0x4361D898, `sub_436200B0`) only
  records state and calls an event/log helper `sub_439FE1C4(0xF0000404?, id, a, b)` [?]. The real I/O is **GPIO** (0xF2910000/
  0xF2912000 accessors @0x43917C48…0x43917DF0) and **SIO serial** to the panel driver IC (`disp_sysv_set_sio_map_data` @0x4361A774,
  `[PNLEVF]sio[ %d ]=%02X`, OLED/LCD soft reset, CALDAC). The panel IC is an external serial-configured device; its own registers
  need not be modelled beyond accepting SIO writes.
* The a5000 has only the 3.0" LCD (no EVF; the EVF code is shared with other models). Panel mode seen by Android/Rigel: **640×480,
  RGBA8888, 30 fps** (sftest). OSD buffers in RAM are 640×480 (§3).

### 4.3 Scan-out hardware: candidates (NOT identified) [?]
MMIO blocks referenced by av-cam (literal-pool scan, `re/ida/gfx/gfx_avcam_mmio.tsv`, context `gfx_avcam_ctx.txt`):

| block | refs | evidence | guess |
|---|---|---|---|
| 0xF3440000–0xF3446FFF | 1105 accessor funcs | auto-generated get/set library; also in DMA-channel table @0x43B2DBCC (0xF3442000, 0xF3443000) | **best candidate for the display/"VIP" block or the YC/ISP pipeline** [?] |
| 0xF3850000 / 0xF3950000 / 0xF3A50000 | 3 identical instances, ~200 funcs | callers are image-pipeline stages (SPICA, KKNR, LFNR, Miniature…) | resizers / YC processors, not display |
| 0xF34A0000/B0000/C0000, 0xF35C0000 | 4 identical | "SA" (DSP) firmware load, SA_JPEG | SA DSPs |
| 0xF3800000 / 0xF3900000 | | VFX "ddl_VfxEffectTransfer" | 2D effect engines |
| 0xF3020000/0xF3030000 | 432 / 106 | IDT/DFE/OPD | sensor front end |
| 0xF3120000, 0xF3C00000, 0xF3E00000 | | SIF/GIF/MPHY/SubLVDS, Imager | sensor interface |
| 0xF3420000 | 104 | AudioIp | audio |
| 0xF2800000 (0xF2804xxx–0xF2806xxx) | | huge register-list tables @0x43ECEA28…; grm_gles uses 0xF28040C4/41A8/50B8/518C | clock / power-domain controller |
| 0xF2A01000–0xF2A32000 | | `sub_438BCD40/CF08/CB48` near the HDMI PA | HDMI PHY/clock side |

Next step to close this item: decompile the VDF "SET_PANEL_OUT" / OSD-layout handlers down to the register writes. They are
reached through vtables (`VDF::…Cmd…::Execute`), so a call-graph BFS from strings did not reach them; follow the vtables instead.
Alternatively, in the emulator, trace av-cam MMIO writes to unknown blocks while VDF starts. A logging "unimp" region over
0xF3440000 will reveal the plane-address, size and vsync registers as soon as av-cam runs.
The vsync IRQ is likewise unknown [?]: av-cam registers its own GIC handlers. Log GIC enables made by av-cam.

---------------------------------------------------------------------------------------------------
## 5. Getting a picture in a QEMU window without the scan-out model
Firmware stays unmodified; this is a debug "virtual monitor" device that only **reads** guest RAM:
* `sony-a5000,fb-pa=…,fb-fmt=…`: every 33 ms, blit a guest-physical 640×480 buffer to a QEMU graphic console.
  Default candidates in order: Rigel main windows 0x8E9B2B00 / 0x8EC14E00 (32 bpp ARGB, stride 0xA00, image at +0? / header 0x5080
  bytes at end?) [?], OSD 0x8F758000 / 0x8F7EE000 (16 bpp, stride 1280) [?]. These PAs are deterministic as long as dmm/MA
  allocate in the same order.
* Better: **follow the GPU PPF**. The final composition step to the panel OSD is very likely a PPF display transfer (tiled→linear,
  +0xC04 = dst>>2, +0xC08 = dims, +0xC10 = format) [?]. The GPU model can latch the last PPF destination of size 640×480 and
  present it. This needs no knowledge of the display controller.
* Once §4.3 is solved, replace it with the real plane registers (address/format/enable) and generate vsync IRQs at 30/60 Hz.

---------------------------------------------------------------------------------------------------
## 6. HDMI / CEC at boot
* Both are loaded by `libIMDB.so` (`/kmod/hdmi.ko`, `/kmod/cec.ko`) and controlled by `libInfraHdmi.so` / `libInfraCec.so`.
* **hdmi.ko** (`re/ida/gfx/gfx_hdmi.ko.c`): UDIF driver "hdmi" → `udif_ioremap` of the table entry (PA 0xF2A24000, 0x4000).
  `hdmi_task_initialize` @0xb08c additionally ioremaps "hdmiaudi" PA 0xF2A20000 (0x2C) and "hdmicolo" PA 0xF292C000 (0xC), then
  writes (via shadow and `hdmi_core_set_register_sync`): `+0x4F4=0, +0x020=1, +0x2008=0, +0x2020=1`. All accesses are direct APB
  (`hdmi_host_apb_*`), byte or word ("addr_XXXX_bit" descriptors: 0x0020, 0x0024 bit2 = RSEN, 0x0120, 0x01C4 (HPD interrupt
  status/clear, mask 0x40), 0x04BC, 0x04F4–0x0520, 0x2004–0x2030).
  **No IRQ**: HPD and RSEN are polled from a state machine (`hdmi_core_status_task_detect_hpd`, timers). With registers reading 0
  the driver stays in "HPD wait", which is the correct "no TV attached" state.
* **cec.ko**: UDIF "cec" (PA 0xF3C10000, 0x190), registers 0x40 bit4, 0x94 bit4, 0x9C, 0xA0 bit3. Protocol logic waits for HDMI HPD,
  so it stays idle. No IRQ.
* Boot needs: a **RAM-backed register file** (reads return last write, default 0) for all four windows. Unassigned-access
  errors would only appear in logs, but a RAM file avoids surprises. HPD = 0. Hot-plug emulation could come later: set the HPD bit
  in 0x1C4 / 0x024 and serve EDID over the DDC FIFO (`hdmi_core_copyFIFO`, `hdmi_core_getDDCStatus`). Not needed for the LCD.

---------------------------------------------------------------------------------------------------
## 7. Prioritised modelling plan
| P | item | work | unblocks |
|---|---|---|---|
| 0 | RAM register files: GPU 0xF1000000/0x4000; clk 0xF3400000 page (read-back 0x15 at +0x80); 0xF2804000–0xF2805FFF; HDMI 0xF2A24000/0x4000, 0xF2A20000/0x2C, 0xF292C000/0xC; CEC 0xF3C10000/0x190 | trivial | grm_gles/hdmi/cec insmod without BUG; **plus** backup IDs 0x3E0003/0x3E0009 must be readable through backup.ko (else BUG in grm_gles init) |
| 1 | GPU completion model: status 0x34, IRQ 76 level, ack semantics (§2.2), immediate completion of P3D (0x1800), PPF (0xC18) and PSC (0x1C bit0) | small | im.elf/surfaceflinger do not stall 30 s per frame or reset the GPU |
| 2 | Functional PSC fill and PPF display transfer / texture copy (3DS-documented formats, 8×8 tiling, >>2 addresses) | medium | 2D UI paths, the likely final OSD composition |
| 3 | Debug virtual monitor (§5): show the PPF destination or a configured PA | small | a QEMU window with whatever the guest draws |
| 4 | Trace av-cam MMIO (unimp + log) to find the scan-out block, plane registers and vsync IRQ; then model it | RE + medium | "real" panel output, liveview plane, vsync pacing |
| 5 | P3D command-list decoder + software rasteriser + DMP shader ISA (from the dmp_glslc assembler) | large | GLES content (Rigel EGL compositor, Android apps using GL) |
| 6 | HDMI hot-plug + EDID; CEC | optional | external monitor |

Other observations:
* `libIMDB.so` has an `IMDB:qemu` profile next to `IMDB:default/nfs/USBJ/RE-SUB`. Sony seems to have had a QEMU bring-up
  configuration. Which modules it skips might show what Sony itself stubbed [?].
* In the snapshot the GPU is in the suspended state (`dev+0x48 = 1`, two opens), as expected for WBI.

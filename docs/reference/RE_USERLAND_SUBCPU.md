# Cold-boot userland, av-cam (LIRO) and the "sub-CPU" side — static RE (fw 1.10)

Scope: everything after the kernel has mounted the initrd and executed `/sbin/init`,
up to the camera UI (`appFw.so` inside `im.elf`), plus the non-Linux code images
(`av-cam.bin`, `sabin/*.bin`) and the hardware boundary of the camera engine.

Legend: **[S]** = confirmed from the real-camera WBI snapshot (`re/wbi/ram.bin`) or the
real `/setting` backup, **[?]** = inference, not proven. Addresses are `func@addr`
in the named binary. New decompiles produced for this report:

| file | what |
|---|---|
| `re/ida/liro.ko.c` | Hex-Rays of `/usr/kmod/liro.ko` (all functions) |
| `re/ida/upm.ko.c` | Hex-Rays of `/usr/kmod/upm.ko` (all functions) |
| `re/ida/av-cam.boot.c` | av-cam: liro-API slot usage table, entry_pre/main/suspend/resume, C++ init table, ITRON task 1/8 (depth 2) |
| `re/ida/av-cam.mmio.tsv` | every IO-range constant (0xF0000000–0xFEFFFFFF) referenced by av-cam code, per 4 KiB page + exact values |
| `re/ida/libIMDB.imdb_raw.txt` | decoded `imdb_raw` (the im.elf boot table, 147 rows) |

IDA databases on windev: `C:\a5000\emu\boot_liro.ko.i64`, `boot_upm.ko.i64`,
`boot_avcam.bin.i64` (raw ARM, base 0x43541000, 56 973 functions after a Thumb
prologue sweep — scripts `boot_avcam*.py`).

> `emulator/tools/wbi.py -v` is broken (calls `rdv` before defining it) and its
> `walk()` does not handle 16 MiB supersections (the kernel linear map uses them:
> L1 = `0x8305140E` for VA 0x435xxxxx). The linear map is simply PA = VA + 0x40000000.

---

## 0. TL;DR — prioritized blockers for "unmodified firmware reaches the UI"

1. **Bootloader-side RAM preloads (boot contract, not peripherals).** Userland and modules
   *assume* the Sony loader already did these; nothing in Linux does them on a normal boot:
   * **`av-cam.bin` copied to PA 0x83541000** (VA 0x43541000). `liro.ko` only loads the file
     itself when im.elf runs with `target=qemu`; on hardware it gets just `adr=0x83541000`
     (`im.elf sub_B368@0xb368`, Backup prop 0x00FA007C = `00105483` [S]). Without it:
     `tsk_entry` prints "Magic unmatched" and av-cam never starts → every camera-engine
     request from userland times out/blocks.
   * **`/setting/Backup.bin` copied to PA 0x82EC0000** (word 0 of the file, cookie 0xAAAAAAAA [S]).
     `backup.ko` is loaded with `Backup_load=0`, which means "image already in RAM"
     (`backupfile_init@0x4250`: only reads the 256-byte header from the file, then uses
     `arch_phys_to_cache(hdr[0])`). Without it every `Backup_read` fails (boot mode, memory
     map sub 0xFA, settings) and most of im.elf degrades or aborts [?].
   * In that RAM image the loader patches **loader-owned props (subsystem 0x051, attr 0x02)**,
     at least **0x0051000F = LDR boot mode** (1 COLD, 2 HOT, 3 UPDATER, 4 CIPA, 5 ADJUST,
     6 USB_CHARGE, 10 WARM, 11 SS, 12 BIS — `upm.ko ldr_mode2str@0xb9ec`). Saved file has 2 (HOT).
     For a cold boot write 1 [?].
   * Loader mailbox words at PA 0x80021048.. (`0x00FA0029` → 0x8002104C, read by upm
     `@~0x8480` via `arch_phys_to_noncache`); snapshot has `0, 7, 1, 0x800523CC(pm.resume),
     0xFFFF1100` at 0x80021048..58 [S]. Meaning of 7 not decoded [?].
   * The whole `memrsv=0x13A7000@0x82D90000` and `memrsv=0x13800000@0x8C800000` regions must
     be plain RAM (av-cam, Backup image, DMM frame buffers live there).
2. **`dnflasha*` (LZ-compressed partition view via ldec, cmd 0 of the ARM7 mailbox).** `/rootfs`,
   `/usr`, `/system` and Android `/system` are first mounted from `dnflasha7/15/3/16`.
   See `NAND_PROTOCOL.md §5`. Not repeated here.
3. **av-cam's hardware** (it is the camera engine: sensor/ISP/VDF/display/SA codecs). It runs as
   Linux kernel threads, so every MMIO access lands in QEMU. Its boot path
   (`entry_pre`) only touches a fill engine, a DMA copier and an SA clock/reset controller and
   survives on "reads return 0" (§2.5). After `act_tsk(1)` the ~113 ITRON tasks touch
   ~80 IO pages (§4). These are the real long tail; you will discover the blocking ones
   by running with logging on the `a5k.io` unimplemented device (§4.3).
4. **SA accelerator cores** (0xF34A0000/0xF34B0000/0xF34C0000/0xF35C0000) — only needed for
   codecs/face detection and for **creating** a WBI (kernel `cmpr` + `ssboot.bin`). Not on the
   path to a first UI frame unless something waits on an SA "open" ack [?] (§3).
5. GPU/Rigel/display, input, i2c etc. — owned by other reports; listed in §1.7 only as
   /dev dependencies.

---

## 1. Cold-boot userland sequence

### 1.1 `/sbin/init` (initrd, `re/ida/init.c`, `main@0x96cc`)

Tables decoded from `.data` (`drivers1@0x131ec`, `drivers2@0x13214`, `mnts@0x1331c`):

| # | action | detail |
|---|---|---|
| 1 | mount `proc` on `/proc` | `mnts[0]` |
| 2 | `open("/proc/snsc_boot_time", O_WRONLY)` | all `profile_entry()` strings go there (Sony boot-time profiler) |
| 3 | env `HOME=/ PATH=/sbin:/bin:/usr/sbin:/usr/bin:/devel/usr/bin TOPRC=/etc/toprc MALLOC_OPTIONS=gfffffffff LT_ABORTREASON=0`, `setrlimit(STACK,2 MiB)`, SIGCHLD reaper | |
| 4 | `check_oreore1`: `/proc/version` contains "qemu" → `is_qemu`; `/proc/mounts` contains "nfs" → `is_nfs`; `/proc/cmdline` contains `console=ttyAM0` → `is_enable_ctrl_c` | stock banner has no "qemu"; is_qemu would skip **all** NAND mounts (Sony's internal QEMU/NFS setup) |
| 5 | `insmod_drivers(drivers1,5)` = `init_module(2)` (syscall 128) of `/kmod/devno.ko`, `/kmod/dmac.ko pri_mode_ch_0_3=1 pri_mode_ch_4_7=1`, `/kmod/ldec.ko`, `/kmod/lld.ko`, `/kmod/nand.ko` | errors only printed |
| 6 | mount `/dev/dnflasha7` → `/rootfs` ext2 RO; fallback `/dev/nflasha7` | then `chdir /rootfs; pivot_root(".", "initrd")` (syscall 218), `chroot .`, `mount --move /initrd/proc /proc` (flags 0x2000), reopen fds on `/dev/console` (or `/dev/ttycons` when ctrl-c enabled) |
| 7 | `/usr`: `dnflasha15` ext2 RO → `nflasha15` ext2 → `nflasha15` cramfs → `nflasha15` vfat | |
| 8 | `/tmp`: `nflasha10` vfat (flags 0xC0ED0490, `posix_attr`) | vfat + ≤0x32000 bytes free → `umount; /sbin/dosfsck -Y dev; mount` (`mount_partition@0x8e8c`) — same for every vfat below |
| 9 | `/system`: `dnflasha3` vfat RO → `nflasha3` vfat | holds `vmlinux.bin`, `initrd.img`, `av-cam.bin`, `sabin/` |
| 10 | `/setting`: `nflasha2` vfat | `Backup.bin`, `DmmConfig.bin`, `ulogio.bin`, `kemco.txt`, `mode/dmode`, `sen/smode`, `env.txt` |
| 11 | `/log`: `nflasha11` vfat `batch_sync,posix_attr` | |
| 12 | `check_oreore2`: read `/setting/mode/dmode`: `'0'`→kernel-only (stop here), `k` kmc preload, `g` gdb, `d` DUMA, `e/E` efence. `set_oreore_env`: `/setting/env.txt` lines → putenv | real `/setting/mode/` is empty [S] → defaults |
| 13 | `write("/proc/udm/drivers", "set", 3)` (`set_mindrv`) | UDIF |
| 14 | `insmod_drivers(drivers2,33)` from `/usr/kmod/`: osal_utm, osal_uipc, utimer, `backup.ko Backup_load=0`, `osal_ulogio.ko ulogio_initfile=/setting/ulogio.bin`, input, udate, indctr, i2c, sircs, stream, stream2, usb_portMonitor, usbg_sen, usb_extcmd, kikilog, `dmm.ko DmmConfig=/setting/DmmConfig.bin`, upm, nfc, grm_ma, grm_gles, hdmi, cec, accel, mag, compass, mcmn_drv, ms_drv, mmc_drv, sata_drv, msen_drv, usbg_adb, sprop | order matters: upm must be in before step 15 |
| 15 | `check_oreore3`: `/proc/udm/upm_bootmode` (upm.ko, `upm_bootmode_read_proc@0x1231c`) "ADJUST"→`is_cho`, "BIS"→`is_bis` (BIS: mkdosfs `/log`, `swap_prepare.sh`, shell). `/setting/sen/smode` == "UU" → `is_forced_senser` | |
| 16 | `run_bg_process("/usr/bin/bootin.elf")` (fork+execv, no args) | `system()` instead when dmode has `g` |
| 17 | `sen.elf` only if forced-senser; `/proc/1/oom_adj = -16`; **`/usr/bin/s_start_android.sh`** unless `is_cho`; `/usr/bin/net_cmdd.elf` (**absent in 1.10 /usr/bin → child exits -1, harmless**) | |
| 18 | mount `sysfs` `/sys`; `LD_PRELOAD=`; `shell_loop_pvt`: `/bin/ash -c "umount /initrd; freeramdisk /dev/ram0; while true; do /bin/ash --login; ...; done"` | init then just waits |

`/dev` is a static directory in the rootfs ext2 (no devtmpfs). Relevant majors (from
`debugfs stat` of nflasha7): `mem 1:1`, `port 1:4`, `console 5:1`, `rmon 10:222`,
`fb0 29:0`, `dmpgles2 248:29`, `hdmi 248:25`, `cec 248:27`, `mcmn 249:1`, `gpio 240:0`,
`ldec 251:205`, `dmac 251:206`, `backup 251:208`, `boss-t1 251:209`, plus `uipc`, `uio0`,
`uinput`, `stream`, `stream2`, `input/`, `jet/`, `sio0..4`, `ttyAM0..2`, `ram0..16`,
`nflasha*`/`dnflasha*`/`anflasha*`/`cnflasha*`, `mmc*`, `ms*`, `sd*`.

### 1.2 `/usr/bin/bootin.elf` (`re/ida/bootin.elf.c`)

`main@0x9c64` → `sub_97D0` (parse) → mount devpts (`sub_92F4`) → `sub_9914` (fork im.elf)
→ `sleep 1` → `sub_941C` (qsi.elf only if /proc/mounts has "nfs") → message loop.

* Links libbackup/libosal_utm/libosal_uipc → needs `/dev/uipc` (osal_uipc.ko) and the
  backup module. `osal_reg_sync_msg_queue_cb()` failure = "Bb bootin init failed" → exit.
* `sub_9460@0x9460` mode parse: `/setting/mode/dmode` chars `0..3` (dmode), `l` LeakTracer,
  `w` wfree, `g` gdb, `s` gdbserver; argv `dmode= amode= file= target= sim lt nolt wfree
  nowfree gdbs gdb killall -v`. `/proc/version` containing "qemu" sets the `sim` bit.
* **dmode → stage letters** (default `off_12AA0 = "3"` [binary .data]):
  `0` "s", `1` "s g r u" (Global infra), `2` "s g r u l" (LIRO), `3` "s g r u l i a" (APP).
* Command built in `sub_9914` (target: Backup flag → `target=resub`, else `target=qemu` with
  the sim bit, else empty; `cho`/`cipa`/`target=usbj`/`dummy_susres` appended from Backup
  flags; IDs lost by Hex-Rays [?]):

  ```
  im.elf debug <app_argv> <target> s g r u l i a lazy-global &      (via system())
  ```
* Loop: `osal_rcv_msg_tmo` → `sub_9B20@0x9b20`: msg 0x940031..0x940037 → `system("im.elf s|g|r|u|l|i|a &")`;
  0x940005 → run the given command line (this is how im.elf's type-0x40000001 IMDB rows
  start `WebApiLauncher.sh`, `network.sh`, `qsi.elf`); 0x940001 → exit loop.

### 1.3 `/usr/bin/im.elf` — the module loader (`re/ida/im.elf.c`)

im.elf itself is 23 KB; the camera is the set of `.so`/`.ko` it loads from the IMDB table
in `libIMDB.so` (`imdb_raw@0xc064`, 44-byte rows: type, target, flag, me(u16), psid(u16),
file, init, exit, sus, res, inact, act; `IMDB_get_entries()` = `imdb_raw+0x84` = row 3).

* `sub_A628@0xa628` argv: letters `s g r u l i a` → bits 0x10000..0x400000 (`bootall` = all),
  `lazy-global` → `dlopen(RTLD_LAZY|RTLD_GLOBAL)`, `target=`, `app_argv=`, `cipa` 0x2000,
  `qemu` 0x1000, `usbj` 0x8000, `cho` 0x4000, `debug`, `trace`, `test`, `killall`,
  `dummy_susres`, `imdb`, `imssi`.
* Target bit = `IMDB_find_target_bit(name)`: default 1, qemu 2, sim 4, nfs 8, set 0x10,
  usbj 0x20, resub 0x40. A row runs only if `row.flag & targetbit`. Under `default`: rows with
  flag `0xFFFFFFF9` (mcmn/ms/mmc/sata/msen kmods, `qsi.elf noevent0`) **are** included; rows
  `flag=6` (plain `qsi.elf`) only under qemu/sim; `subapp.so` (0x40) and `libUsbChargingApp.so`
  (0x20) are excluded.
* `main@0x9ab8`: `sub_9734` sends sync msg 0x9400E1 to queue 0x94006A; reply 0x12345678
  means "a primary im.elf already exists" → this instance just forwards each stage letter to
  bootin (0x940031+n) and exits. Otherwise it **is** the primary and runs the stages itself.
* Stage engine `sub_B63C@0xb63c(idx, marker, enable, tgtmask)`: walk rows from `idx` until
  `type == marker` or `type == 0x28`; run each row via `sub_B530`→`sub_B430@0xb430`:

  | type | action |
  |---|---|
  | 0x10000001 | `init_module(file, params)` (`sub_B2D0`) |
  | 0x10000002 | same, params = `"<init> adr=0x%x [delay=1] [fn=/system/av-cam.bin load=1 debug=1]"` (`sub_B368`) — **liro.ko only** |
  | 0x20000001..3 | `dlopen(file)`, `dlsym(init)()` (`sub_B25C`) |
  | 0x20000004 | `dlopen(file)`, `init(arg)` with arg = `act` ("AppConfig.so") or, if `cho`, `inact` ("AdjChkConfig.so") (`sub_B1A8`) |
  | 0x40000001 | ask bootin (msg 0x940005) to `system("<file> <init> &")` |
  | 3 | putenv |

* Stage contents (row numbers = `libIMDB.imdb_raw.txt`), run by the primary in this order:

  | letter / call | rows | content |
  |---|---|---|
  | s `B6E4(0,1,1)` | — | empty (row 3 is already marker 1) |
  | g `(…,2,g)` | 3–11 | target-name rows (no-op) + `/kmod/dmm.ko`, `/kmod/upm.ko` → **open fails** (rootfs `/kmod` has only the 5 initrd modules; already loaded from `/usr/kmod` by init) |
  | r `(…,3,r)` | 12–21 | `/kmod/stream,stream2,mcmn_drv,ms_drv,mmc_drv,sata_drv,msen_drv,hdmi,cec.ko` → same, fail harmlessly |
  | u `(…,4,u)` | 22–33 | `libosal_uipc/utm/ulogio`, `libbackup`, `libOnDemandLoader` (odl_init), `libInfraKikiLogGenerator`, `libSysDef`, `libMWF` (MWF_Init), `arbiter.so` (ABT_init), `libInfraInput`, `libJiritsu` |
  | l `(…,5,l)` | 34–35 | **`/usr/kmod/liro.ko adr=0x83541000`** → starts av-cam (§2) |
  | — | | `osal_reg_sync_msg_queue_cb`, `osal_snd_sync_direct(cmd 0x920114)` (UPM) |
  | i: threads i0/i1/i2 (`sub_94C8`, `tgtmask` 1/2/4, `B63C(0,5,0,m)` then `(…,7,i,m)`) | 36–100 | CoreSync, grm, gs, DDS, uxengine, Indctr, Tp, Hotshoe, Sircs, Dial, Lanc, InputServant, **ScalarDaemon**, HapiLib, Selector (tgt 1); udate, **InfraAvbbc**, MediaConfig/Common/Fsys/Mount/Detect, mpr*, StillPool, Database, CacheMgr, StillDate, dig_level, ScalarMpr, FuFsys (tgt 2); USB*, sensapi, Hotplug, **InfraHdmi**, Cec, Airplane, Nfc, updater, Wlan*, NetService, gps/compass/geo, HdmiInput (tgt 4); then MediaPwrMgr, CameraBackground, **SyncAndroid** (type 0x20000003) |
  | a: `a_thread` (`sub_9474`, `B63C(0,7,0)` then `(…,8,a)`) | 101–103 | **`appFw.so` `Application_System_Init("AppConfig.so")`** = the camera UI; `subapp.so` skipped |
  | on demand (msg 0x940021 → `sub_B530` by `me`) | 104–145 | Jet/MTP/MSC/PTP/USB-storage/ext-cmd, WebApi, Dlna, LiveViewClient, LensCommunicator, `WebApiLauncher.sh`, `network.sh`, `qsi.elf noevent0`, … |

* After the threads join: `Backup_read`; `sub_9A84` (warm counter := 0, `Backup_write`);
  `osal_snd_sync_direct(cmd 0x920117)`; `sub_91A4(0x920121, 0x92026E, 0x920269, …)` (register
  with UPM [?]); then the power-state message loop `sub_A06C@0xa06c` (handles UPM
  0x920143.. commands; `sub_9FB4` writes `"warm"`/`"mem"` to `/sys/power/state` — this is the
  suspend/WBI path).

### 1.4 `s_start_android.sh` (forked by init in parallel)

tmpfs `/android`; `gzip -dc /usr/bin/ramdisk.img | busybox cpio -idm`; mount
`/dev/dnflasha16` ext2 RO → `/android/system`; `/dev/nflasha17` vfat → `/android/data`
(fallback ext2); `chk_bkfile.elf`; possibly `mke2fs` on `/dev/ram8`,`/dev/ram9`;
`insmod /usr/kmod/usbg_adb.ko` (already in); cgroup memory limit 113M;
`busybox taskset 0x8 chroot /android /init &` — **Android runs pinned to CPU3**. Not on the
critical path of the camera UI except via `libSyncAndroid.so` (stage i, row 100) [?].

### 1.5 Kernel interfaces each stage needs

| consumer | needs |
|---|---|
| init | `/proc/{snsc_boot_time,version,mounts,cmdline,1/oom_adj}`, `/proc/udm/drivers` (UDIF), `/proc/udm/upm_bootmode` (upm), `init_module`, `pivot_root`, vfat (`posix_attr`, `batch_sync` Sony options), ext2, cramfs |
| bootin | `/dev/pts` devpts, `/dev/uipc` (OSAL message queues, osal_uipc.ko), backup module |
| im.elf core | `/dev/uipc`, osal_utm (`osal_*` timers/semaphores), backup, `/sys/power/state`, syscall 129 (`delete_module`, `sub_B6F2`) |
| libObj (GPU, stage i) | `/dev/dmpgles2` ioctl `0xC0048202` (register PA), `0x80048263`, `0xC0048212`; then **`/dev/mem` mmap of the GPU register PA** (`libObj sub_5EA650` area, decompile line ~1337330) |
| many libs | `/dev/mem` mmap of DMM buffers (sub 0xFA memory map, e.g. `0x8C800000`), `/dev/gpio`, `/dev/stream*`, `/dev/mcmn`, `/dev/dmac` (libmpr), `/dev/hdmi`, `/dev/uinput` (InputServant) |
| liro/av-cam | `subsys_request_irq`, `hwtimer_*`, `kthread_*`, `osal_*` in-kernel API, `Backup_*` in-kernel API, `dmm_direct_call`, `tmonitor_write`, `dmac_mem_transfer*` |

### 1.6 First hardware interactions that would block (userland side)

Userland itself touches hardware only through modules (+ `/dev/mem` for GPU/DMM). In boot
order the first things that can hang rather than fail:

1. `nand.ko`/`lld.ko` mailbox + ldec (mounts) — NAND_PROTOCOL.md.
2. `backup.ko` init: RAM image at 0x82EC0000 must have cookie 0xAAAAAAAA (`Data_init` /
   `Data_GetCookie`), otherwise `backup_init() error (file_load)` [?].
3. `upm.ko` init: reads 0x0051000F/0x00510013/14, 0x00FA0029 (→ phys 0x8002104C), loops on
   **0xF2916730 bit 8** and **0xF2916740 & 0xF** in `upm_ddr_ctrl`-like helper
   (`upm.ko.c` ~line 12470: write 0xF2916720 = mode|(x<<4), 0xF2916700 = 1, poll until
   0xF2916740&0xF == 0 and 0xF2916720 echoes the request). That block is the sysctl page
   0xF2915000+0x1700 (DDR/DFS controller [?]). If unmodelled and read-as-0 it terminates;
   **if it is modelled as RAM the echo check passes; if it returns garbage it spins**.
   Called from DFS/power paths, not necessarily at boot [?].
4. `liro.ko` `insmod` blocks until av-cam `entry_pre` returns (`liro_init@0x83fc` →
   `wait_for_completion`) — §2.5.
5. Userland ↔ av-cam sync messages (ScalarDaemon/InfraAvbbc/HapiLib/appFw) — block if an
   av-cam task is stuck on hardware.

---

## 2. `av-cam.bin` — the LIRO camera firmware

### 2.1 What it is
* "ORIL" = "LIRO" byte-reversed. **LIRO = Sony's µITRON-on-Linux layer** (`liro.ko`, "LIRO (liro)
  rev:4-MUSASHI", source `/vobs/Etools-LIRO_131H/driver/os/os.c`). `av-cam.bin` is a
  statically linked µITRON 4.0 application (C++; source trees `Avbb-Vdf_131H`, `ScalarA-ScalarInfra_131H`,
  "ZIT" face detection, audio "AIP", ZIMA video codec control…) — i.e. **the whole camera
  engine: VDF (video data flow / live view / display planes), ISP control, AF/AE, lens,
  codecs control, SA management**.
* **It does not run on a separate processor.** It runs on the same quad Cortex-A5 as Linux,
  in SVC mode, inside kernel threads `kliro_%d` created by liro.ko (`os_create_task`,
  `os_thread_bind` → `kthread_bind(cpu)`), using the kernel page tables. ISA: ARM entry stubs
  (`0x43541030..0x435410A0`, `push {..}; blx thumb`) + **Thumb-2 body (ARMv7-A)**.
* Header (file offset 0):

  | off | value | meaning (`tsk_entry@0x8558` in liro.ko) |
  |---|---|---|
  | 0x00 | `EA00000A` | `b 0x30` |
  | 0x04 | `"ORIL"` (0x4C49524F) | magic |
  | 0x08 | 4 | API version (must be 4) |
  | 0x0C | 0x43541044 | entry_pre |
  | 0x10 | 0x43541058 | entry_main |
  | 0x14 | 0x435410B4 | info block (printed by liro as ram/bss/tail/limit): words `0, 0x43E791C0, 0x6190B, 0x43C27F39, 0x1F404E, 0x43F43B00, 4, 0x44136FFC, 4` — .data = 0x43E791C0+0x6190B; entry_pre clears .bss = **0x43C84000 + 0x1F404E** (the header bss word does not match, field layout [?]); **+36: liro writes its 209-slot API table here (0x344 bytes) → av-cam slot base 0x435410D8** |
  | 0x18 | 0x43541068 | suspend |
  | 0x1C | 0x43541078 | resume |
  | 0x20 | 0x43541088 | exit |
  | 0x24.. | 0x43541098/9C/A0 | func1..3 (bx lr) |
* Link/load address: **VA 0x43541000 = PA 0x83541000**, file 0x9A103C bytes (→0x83EE203C),
  runtime limit 0x84137000. Matches exactly the snapshot hole PA 0x834C0000–0x84137000 (the
  WBI excludes it; boottime 0x834C0000, klog 0x834E0000 precede it) [S] and lies inside
  `memrsv=0x13A7000@0x82D90000`.

### 2.2 How Linux loads and starts it
1. **Bootloader** copies `av-cam.bin` to PA 0x83541000 (the only path on hardware; im.elf passes
   `fn=` only with the qemu flag 0x1000 — `sub_AD3C`).
2. im.elf stage `l`: `init_module(liro.ko, "adr=0x83541000")`; `adr` comes from Backup
   0x00FA007C (fallback 0x00FA0076, then 0xC2F00000; 0xC0000000 if the first read fails).
3. `liro_init@0x83fc`: `adr -= 0x40000000` (→ VA), `ltron_tsk_init`, `malloc_init`, spawn
   kthread `tsk_entry` (prio 5) and **wait for completion**.
4. `tsk_entry@0x8558`: optional file load (`fn`, `load`), check magic/version, copy the API
   table to `info+36`, call **`entry_pre(0)`**, then `tsk_entry_2nd@0xa10`: `complete()` →
   insmod returns; if `delay` (only with `cipa`) wait for a write to `/proc/liro/start`
   (upm.ko contains that path, `k_upm_liro_start` [?]), else directly call **`entry_main`** in the `liro-liro` thread.
5. `/proc/susres` entry for suspend/resume; `liro_suspend/resume` save/restore
   (`get_cp_start/size`, "ft_backup/cp_backup") around WBI.

### 2.3 Communication with Linux/userland
No mailbox hardware between av-cam and Linux — same CPUs, same memory:
* **OSAL message queues** (the same uipc IPC userland uses): API slots
  `osal_snd_msg`, `osal_snd_sync_msg`, `osal_snd_sync_direct`, `liro_osal_rcv_msg_tmo`,
  `osal_reg_*_queue_cb`, `osal_valloc_msg_wait`… (slot table in `av-cam.boot.c`; veneers at
  0x4355D32C–0x4355DE44, one per API, calling `[0x435410D8+4*i]`).
* **Backup** API (settings), **`dmm_direct_call`** (DMM buffer manager), cache/phys helpers,
  `dmac_mem_transfer*`, `tmonitor_write`.
* Shared memory = DMM pools in the 312 MiB reserved bank 0x8C800000 (sub 0xFA memory map).
* Interrupts: `def_inh` → `os_request_irq` → `subsys_request_irq(irq, isr_wrapper, …, cpu)`;
  `isr_wrapper` runs the handler only on the bound CPU and **writes GICC_EOIR (0xF8000110)
  itself**. `ena_int/dis_int/chk_pic/clr_pic/set_pic` write **GICD_ISENABLER (0xF8001100),
  ICENABLER (0xF8001180), ICPENDR (0xF8001280), SGIR (0xF8001F00)** directly — the QEMU GIC
  must tolerate a second agent poking the distributor.
* av-cam IRQs (GIC IDs, table at 0x43E94D04, 87 × {intno, attr, handler, cpu}; installed by
  `main_tail@0x4355D240`):
  `35-38 40-56 61-70 72-75 77-79 81-84 98 99 101 102 122 123 126 128 132-136 142 144 151
  163-168 173-188 201-205 215 216`, bound to CPU0/1/2 (never CPU3). Known ones: 151 = UART1,
  163–168 = CXD4115 timer ch10–15 (153+n), 201–205 = **sio0–4** (SPI to sensor/lens/panel [?]).

### 2.4 Boot path inside av-cam (Hex-Rays, `av-cam.boot.c`)
`wr32 = sub_4355CD10`, `rd32 = sub_439B186C`, `prof(code,line) = sub_4355CD78` (boot-time
messages via OSAL queue 0x8F013A).

**entry_pre `sub_4355CE18`**
```
wr 0xF3400260 = 0x15 ; wr 0xF3400060 = 0x15        ; clock/reset ON for unit "0x60" [?]
wr 0xF30A1280 = 0x20 ; 0xF30A128C = 0
wr 0xF30A12A0 = PA(0x43C84000) ; 0xF30A12A4 = 0x1F404E ; 0xF30A12AC/B4/BC/C4 = 0
wr 0xF30A1280 = 0x21                                ; HW memory fill: zero .bss (2 MiB)
if (.data != load copy):                            ; DMA copy .data -> 0x43EE2100 (pristine copy for resume)
   wr 0xF0102030 = 1; 0xF0102008 = 64; 0xF010200C = 64; 0xF01021C8 = 0
   loop 16 KiB chunks: 0xF01021C0 = src PA, 0xF01021C4 = dst PA,
                       0xF01021CC = 0x1C491000 | nwords, 0xF01021D0 = 1,
                       while (rd 0xF010201C & 0x40) ;           ; busy
   wr 0xF0102030 = 0
while ((int)rd 0xF30A1284 < 0) ;                    ; fill busy (bit31)
wr 0xF30A1200 = 0x10 ; 0xF3400064 = 0x15 ; 0xF3400264 = 0x15     ; clock/reset OFF
sub_4355D180: create 113 tasks, 35 sems, 30 flags, 67 mbf/mpl, 2 mpls (static ITRON config)
```
**entry_main `sub_4355CFFC`**: run 346 C++ static constructors (table 0x43AE5054), then
`main_tail`: `sta_cyc(1)` + 87 × `def_inh`, then `act_tsk(1)`.
**Task 1** `0x439B195A` (prio 5, 4 KiB stack): ~13 subsystem inits
(`sub_43AB3A24, 43A52738, 43AB8888, 438DA818 (writes 0xF2804000/5000/6000 &= ~0x3F), 439C1818,
439172E8, 43919D40, 439188E4, 43916E50, 43917280, 439B1E0C, 4355E56C`), then
`act_tsk(110, 50, 51, 52, 57)`, `sub_43A52746`, `sub_439BD77C`, `ext_tsk`. Task 8 =
`0x439B1A86` → `sub_436FCF00` (large init fan-out). Deeper tracing not done.

### 2.5 Emulation notes for av-cam boot
* With an unimplemented-device that reads 0, entry_pre completes (no poll can spin) — but the
  .bss fill and the .data DMA copy are **not performed**. Fresh QEMU RAM is zero, so .bss is
  fine on a cold boot; the missing .data copy only breaks a later suspend/resume (WBI).
  Cheap to model properly: fill engine @0xF30A1200 (+0x80 ctrl, +0x84 status bit31,
  +0xA0 PA, +0xA4 length, pattern regs +0xAC..+0xC4) and DMA copier @0xF0102000 (+0x1C0 src,
  +0x1C4 dst, +0x1CC len|0x1C491000, +0x1D0 kick, +0x1C bit6 busy) [?].
* 0xF3400000 is a set/clear clock-reset controller for the accelerator domain (+0x00/+0x04 set/clr
  pairs; kernel cmpr uses +0xB0/0xC0/0x130 clocks and +0x2B0/0x2C0/0x330 resets with values
  5 and 0x11). RAZ/WI is acceptable [?].
* A5 core count: av-cam binds tasks/IRQs to CPU0–2 (`os_thread_bind` fails if offline);
  keep 4 CPUs online.

---

## 3. `sabin/*.bin` — SA accelerator programs

* Common header (`3D 00 xx B0 42 34 …`, size at +0x18, load/segment table +0x40..0x80);
  bodies are **not ARM/Thumb** (32-bit word-oriented, e.g. `0000000N` words). ISA unknown —
  Sony "SA" programmable signal-processing cores [?].
* SA core register blocks (av-cam constants; identical layout +0x000 ctrl (14 refs),
  +0x100 mailbox/irq (16), +0x200 (8)): **0xF34A0000, 0xF34B0000, 0xF34C0000, 0xF35C0000**;
  clock/reset in 0xF3400000. av-cam strings mention SA "mbox", "int_fact", "SA Load", "hardware
  hang" (audio, JPEG, ZIMA AVC, panorama, face "ZIT", "idt_*" tracking).
* Consumers:
  * **`ssboot.bin` / `ssboot_any.bin`**: kernel `cmpr` driver (WBI image compressor):
    `cmpr.path=/system/sabin/ssboot.bin cmpr.path2=…ssboot_any.bin`, loaded by
    `cmpr_load_file@0x40054664` into `cmpr.mem=0x8000@0x9A000000`; 3 cores at table
    `0x4037C484` (stride 24): `{clk 0xF3400130/0xF34000B0/0xF34000C0, rst 0xF3400330/0xF34002B0/0xF34002C0,
    regs 0xF34C0000/0xF34A0000/0xF34B0000, mbox +0x100, work 0x9A000180/300/480}`.
    `cmpr_deflate@0x400542F8`: descriptor in work RAM, `regs+0x30 = codePA|0x80000000`,
    `+0x8 = 0x80000000`, `+0x0 = 1`, mbox `+0x10/+0x14` descriptor, `+0x4/+0x8 = 0xFF`
    (irq), then `regs+4 = 1` (go); completion polled (`cmpr_deflate_poll/is_end`).
    **Only used when writing/rewriting a WBI** (ssboot writer).
  * `idt_cam.bin` (object/eye tracking), `sa_dfdet.bin` ("ZITTOOLS" face detector) and
    `/usr/sabin/sa_*.bin` (codecs) are loaded on demand by userland **libOnDemandLoader.so**
    (ODL, file/memory info from Backup) into DMM memory units, and started by av-cam
    through the SA mailboxes [?]. Not needed for a first UI frame unless a face-detect/AF
    task waits for an SA ack at boot [?].
* The **ARM7** (NAND sub-CPU, SRAM @0x0, arm7io @0x11000, IRQ 170) is the only other real
  processor on the Linux side; its firmware is left running by the bootloader and is HLE'd per
  `NAND_PROTOCOL.md`.

---

## 4. Camera-engine hardware boundary

### 4.1 Who touches the hardware
* **av-cam (kernel threads)**: all ISP/VDF/sensor/display/SA register blocks, via the 1:1 static
  IO map (L1 sections `0xFxx11452`: VA == PA for 0xF0000000–0xF3FFFFFF, 0xF8000000, 0xFA000000,
  0xFC000000 [S]; 0xF4xx/0xF5xx/0xFExx are not mapped → those constants are data, not MMIO).
* **ScalarDaemon/SDCom, HapiLib, InfraAvbbc** (userland, stage i): pure OSAL-message clients of
  av-cam — no direct MMIO found [?]. "VDF" (live-view/OSD planes) is inside av-cam
  (`/vobs/Avbb-Vdf_131H`).
* **libObj (GRM/GPU)**: `/dev/dmpgles2` + `/dev/mem` mapping of GPU registers (see GPU report).
* Kernel modules: UDIF table @0x40379B28 (HARDWARE.md).

### 4.2 IO pages referenced by av-cam (static constants; `av-cam.mmio.tsv`)
Sum of referencing functions per 64 KiB block (≥ 8 shown):

| block | refs | notes |
|---|---|---|
| 0xF0000000 / 0xF0010000 | (79, mostly masks) | dmac ch0–7 (IRQ 189–196) |
| 0xF0100000 | 4 | DMA copier @0xF0102000 (entry_pre) |
| 0xF2000000 | 39 | UART0/1 (UART1 IRQ 151 owned by av-cam) |
| 0xF2400000 | 56 | timers 0xF2403000/0xF2404000, **sio0–4 0xF2405000+0x200n**, 0xF2406000 |
| **0xF2800000** | 142 | 0xF2802000 (16-reg stride-0x10 table), 0xF2804000/5000/6000 (task-1 init clears bits 0..5) — not in UDIF table, unknown [?] |
| 0xF2900000/10000/20000 | 26 | clkrst3, GPIO p0–p17, otg 0xF292A000/B000/D000 |
| 0xF2A00000–0xF2A30000 | 47 | HDMI 0xF2A24000 neighbourhood, clkrst4 0xF2A30000 (+0x1170/80/90) |
| 0xF3000000 / 0xF3010000 | 159 | +0x2200.. reg arrays, 0xF3015800.. — ISP front end [?] |
| **0xF3020000** | 443 | heavy: +0x400/0x700/0x800/0xC00 arrays [?] |
| 0xF3030000 / 0xF3040000 | 142 | |
| 0xF30A0000 | 11 | fill engine 0xF30A1200 |
| 0xF3100000 / 0xF3110000 | 108 | mirror of 0xF3000000/0xF3010000 layout (2nd instance) [?] |
| **0xF3120000** | 202 | 0x2100/0x2200/0x2300 and 0x3100/0x3200/0x3300 (2 × 3 channel sets) [?] |
| 0xF3400000 | 38 | accelerator clock/reset controller |
| 0xF3420000 | 176 | |
| **0xF3440000** | 1122 | biggest: +0x100..+0x700 channel arrays — likely the VDF/IMG DMA hub [?] |
| 0xF34A/B/C0000, 0xF35C0000 | 4 × 38 | SA cores |
| 0xF3800000 / 0xF3900000 | 55 | same layout (+0x1000×16, +0x1100) ×2 |
| **0xF3850000 / 0xF3950000 / 0xF3A50000** | 3 × ~210 | three identical engines (+0x0, +0x100, +0x400, +0xD00, +0x1C00) [?] |
| 0xF3880000 | 11 | |
| 0xF3C00000 / 0xF3C30000 | 159 | |
| 0xF3E00000 | 67 | |
| 0xF8000000 | — | GIC (via liro) |
| 0xFC0A0000 | 76 | +0x3100/+0x4100 (38 each), 0xFC020000 +0xA00C [?] |

These are base constants; most register offsets are applied at runtime, so the list is a map
of *where*, not *what*. Per-function detail is in the TSV (first 8 referencing functions per page).

### 4.3 Recommended modelling strategy (no sensor)
1. Keep `a5k.io` (0xF0000000–0xF7FFFFFF) as unimplemented-RAZ/WI **with access logging**
   (`-d unimp`) and boot to stage `l`; then dump the av-cam PC histogram
   (`info registers` on CPU0–2) to find spin loops. RAZ makes "busy" bits clear, which is what
   most polls want; the dangerous cases are polls for a **set** done/ready bit and waits for
   **interrupts** (frame-start/VD of the sensor/ISP/VDF, SIO transfer complete).
2. Expect to need at least: (a) a periodic **VD/frame interrupt** source on the VDF/display
   IRQs from the list in §2.3 [?]; (b) **SIO (0xF2405000)** completion IRQs 201–205 with
   plausible sensor/lens replies (or "no lens"); (c) SA mailbox open/close acks
   (0xF34A0000.. +0x100) if any task waits for them.
3. Do **not** model the image pipeline math; a black live view is fine. Make DMA/fill engines
   (0xF30A1200, 0xF0102000, dmac) actually move memory so data structures are consistent.
4. Alternative that sidesteps the sensor entirely is not available without modifying firmware:
   the `target=qemu`/`sim` code paths exist (IMDB target bits 2/4, `qsi.elf`) but are only
   selected by "qemu" in `/proc/version` (kernel banner) or bootin argv.

---

## 5. WBI caveat (affects what "cold boot" means)
The real camera **never cold-boots** in the field: the loader resumes the factory WBI snapshot
in `nflasha5` (`warm.mode=2`, NOTES §2). The kernel log inside the snapshot is the factory cold
boot that *created* it ("ssboot: optimizing image done", suspend, "count_sections") [S].
A cold boot in the emulator therefore follows the same path as Sony's factory image build; whether
it will try to write a new WBI at the end (needs `cmpr` + SA cores + ssboot.bin, upm
"EV OFF … Warm boot", `libSyncAndroid` `/setting/first_ssboot`) is not proven [?]. If it does,
failure of the SA compressor should surface as "cmpr: deflate abort" rather than a hang [?].

## 6. Open items
* Meaning of loader words 0x80021048–0x80021058 and the remaining sub-0x051 props.
* Backup IDs behind bootin's `cho/cipa/usbj/dummy_susres/resub` flags (Hex-Rays lost the args;
  read the literal pools in `bootin.elf sub_9844..sub_98EC`).
* Which av-cam tasks wait on which IRQ at boot (needs dynamic run or deeper static walk of
  tasks 1/8/50–52/57/110).
* ISA of the SA cores.

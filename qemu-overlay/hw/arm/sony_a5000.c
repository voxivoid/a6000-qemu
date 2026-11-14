/*
 * Sony ILCE-5000 (a5000) — CXD90014 "BIONZ X" SoC machine model.
 *
 * Goal: run the unmodified official firmware (1.10) exactly like the
 * real camera.  Every address below was recovered from the firmware
 * itself; see emulator/docs/HARDWARE.md for provenance and status.
 *
 *   0x80000000  DDR, 512 MiB (kernel cmdline: mem=200M@0x80000000,
 *               mem=312M@0x8C800000)
 *   0xC0000000  on-chip memory, 16 MiB (SMP mailbox at +0x5000)
 *   0xF2000000  PL011 UART0 (ttyAM0, GIC 150)  +0x1000 UART1 (151)
 *               +0x2000 UART2 (152)
 *   0xF2402000  watchdog (CXD4115 channel, bit31 = reset)
 *   0xF2915000  system controller (chip id 0x500, reset request)
 *   0xF2403000  CXD4115 timer block (GIC 153..)
 *   0xF8000000  Cortex-A5 MPCore private region (SCU, GICC +0x100,
 *               global timer +0x200, private timers +0x600, GICD +0x1000)
 *   0xF8002000  L2C-310
 *
 * Kernel entry: raw Image at PA 0x80038000 (PAGE_OFFSET 0x40000000,
 * TEXT_OFFSET 0x38000), machine id 0xDA8 "ARM-CXD90014", ATAG boot.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "cpu.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/char/pl011.h"
#include "hw/cpu/a9mpcore.h"
#include "hw/misc/unimp.h"
#include "hw/arm/boot.h"
#include "hw/sd/sdhci.h"
#include "hw/misc/cxd90014_adc.h"
#include "ui/input.h"
#include "hw/misc/cxd90014_sio.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "system/blockdev.h"
#include "system/block-backend.h"
#include "system/system.h"
#include "system/address-spaces.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "system/rtc.h"
#include "chardev/char.h"
#include "qom/object.h"

#define A5K_DDR_BASE        0x80000000ULL
#define A5K_DDR_SIZE        (512 * MiB)
#define A5K_KERNEL_ADDR     0x80038000ULL
#define A5K_INITRD_ADDR     0x80428000ULL
/* boot scratch inside memrsv 0x28000@0x80000000, clear of the MUMIN
 * hand-off (0x80000000+0x20000) and the wbimem page at 0x80021000 */
#define A5K_ATAG_ADDR       0x80024000ULL
#define A5K_STUB_ADDR       0x80025000ULL
#define A5K_HANDOFF_RTC_LOST 0x800210E4ULL  /* docs/RE_RTC.md section 2 */
#define A5K_MACH_ID         0xDA8
#define A5K_PERIPH_BASE     0xF8000000ULL
#define A5K_L2C_BASE        0xF8002000ULL
#define A5K_UART_BASE       0xF2000000ULL
#define A5K_TIMER_BASE      0xF2403000ULL
#define A5K_NUM_IRQ         256

#define GIC_ID(n)           ((n) - 32)

/* Exact command line passed by the real bootloader (recovered from the
 * WBI RAM snapshot).  "-M sony-a5000,debug-console=on" swaps
 * console=ttynull for a live serial console on UART0 (ttyAM0); the Sony
 * PL011 driver additionally needs amba2.console=1 to emit anything.
 * Override completely with -append. */
static const char a5k_default_cmdline[] =
    "ramdisk_size=8192 rw noatime noalign lpj=1671168 "
    "heap_profile.records=0 dma_mapping.l2th=0x80000 "
    "snsc_workqueue.kipid=130 manage.irqd=129 softirq.sirqd=128 "
    "exception.reboot=1 softlockup_panic=0 softlockup_thresh=30 cfg.uart=5 "
    "wdt.timeout=500 warm.mode=2 warm.tmo=50000 warm.resume_drop=0 "
    "warm.boot_drop=0 warm.ignore_drop=1 basic.prof_path=/log/ssbi_prof.bin "
    "cfg.poff=0,10 error.led=0,0 page_alloc.minkb=256 console=ttynull "
    "xrstreq=1 wdt.mode=1 stack_usage=n cxd90014.bam=N ip=off "
    "cmpr.path=/system/sabin/ssboot.bin "
    "cmpr.path2=/system/sabin/ssboot_any.bin tmonitor.addr=0x82000000 "
    "tmonitor.size=0x500000 tmonitor.mask=0 rootfstype=ext2 root=/dev/ram0 "
    "init=/sbin/init initrd=0x80428000,8M pm.resume=0x80021054 "
    "pm.sus_error=0x80021060 wbi_memclr=0x800210e8 "
    "wbimem=0x1000@0x80021000 wbimem=0xE5000@0x82D90000 "
    "wbimem=0x8000@0x82EAE000 wbimem=0x500000@0x82FC0000 "
    "wbimem=0x30000@0x9EC00000 wbimem=0x80000@0x8C800000 "
    "wbimem=0x204E000@0x8D7A0000 wbimem=0x1E000@0x8FE3F000 "
    "wbimem=0x2000@0x8FE73000 wbimem=0xEC000@0x91883000 "
    "wbimem=0x200000@0x91E00000 cmpr.mem=0x8000@0x9A000000 "
    "warm.mem_dst=0xC00000@0x9B000000 warm.mem_work=0x600000@0x9C000000 "
    "wbi_cmpr.waddr=0x5D000000 wbi_cmpr.wsize=0x100000 wbi_cmpr.nbuf=12 "
    "boottime=0x20000@0x834C0000 klog.addr=0x834E0000 klog.size=0x20000 "
    "alog.addr=0x8345ffc0 alog.size=0x60040 alog.clock=cpu "
    "mem=200M@0x80000000@0 mem=312M@0x8C800000@2 "
    "memrsv=0x28000@0x80000000 memrsv=0x13A7000@0x82D90000 "
    "memrsv=0x13800000@0x8C800000";

#define TYPE_A5K_MACHINE MACHINE_TYPE_NAME("sony-a5000")
OBJECT_DECLARE_SIMPLE_TYPE(A5KMachineState, A5K_MACHINE)

struct A5KMachineState {
    MachineState parent;
    ARMCPU *cpu[4];
    hwaddr entry;
    bool debug_console;
    char *provision;
    char *nand_file;
    char *preload;
    char *loader;
    char *fb;
    char *usb_socket;               /* A5KU server address HOST:PORT */
    DeviceState *gpio;
    QEMUTimer *submicro_t;
    int submicro_step;
    bool submicro;
    uint8_t *submicro_frames;
    int adc_key[4];                 /* qcode holding each ADC key ladder */
    bool key_shift;                 /* host Shift held (zoom speed) */
    bool key_s2;                    /* shutter fully pressed */
    DeviceState *adc[2];
    char *submicro_file;
    /* sub-micro RTC (docs/RE_RTC.md section 7) */
    time_t rtc_offset;              /* seconds added to qemu_get_timedate */
    uint8_t rtc_set[6];             /* last applied TX clock write */
    bool rtc_set_valid;
    int rtc_ack;                    /* replies that still echo rtc_set */
    bool rtc_lost;
    /* E-mount lens (docs/RE_LENS_EMOUNT.md section 5) */
    char *lens;
    bool lens_ekit;
    bool lens_pwr_req;              /* slot-B TX TLV 0x0A data[1] bit0 */
    int lens_hs[2];                 /* driven P5.4 / P2.11 */
    QEMUTimer *lens_hs_t;
    /* sub-micro power-off (docs/RE_REBOOT.md section A.5, 4.4) */
    uint8_t poff_req;               /* slot-B TX TLV 0x0A data[1] & 0x42 */
    bool poff_ack_logged;
    bool sm_rd_pending;             /* previous slot-A TX was a 0x41 */
    uint8_t sm_rd_hdr[5];           /* its bytes 1..5 (address, length) */
    uint8_t sm_mem[4][0x180];       /* micro memory regions (A.4 table) */
    uint8_t sa_tx_last[128];        /* last normal slot-A TX packet */
    uint8_t sa_tx_boot[128];        /* the first one after reset */
    bool sa_tx_valid, sa_tx_boot_valid;
    int poff_level;                 /* p0.10 latch as last reported */
    bool poff_restart;              /* poff=restart (default) or off */
    char *poff;
    QEMUTimer *poff_t;
    int reset_kind;                 /* A5K_RESET_* consumed on reset */
    char *cmdline;
    uint8_t *kernel, *initrd;
    size_t kernel_len, initrd_len;
    Notifier init_done;
    MemoryRegion prov_rom;
    MemoryRegion iram;
    MemoryRegion arm7_sram;
};

/* ---- boot: ATAGs + a 4-instruction stub, like the Sony bootloader ---- */

#define ATAG_NONE    0x00000000
#define ATAG_CORE    0x54410001
#define ATAG_MEM     0x54410002
#define ATAG_INITRD2 0x54420005
#define ATAG_CMDLINE 0x54410009

static void a5k_write_atags(const char *cmdline, uint32_t initrd_size)
{
    uint32_t buf[1024];
    int n = 0;

    buf[n++] = 5; buf[n++] = ATAG_CORE;
    buf[n++] = 1; buf[n++] = 4096; buf[n++] = 0;

    buf[n++] = 4; buf[n++] = ATAG_MEM;
    buf[n++] = 200 * MiB; buf[n++] = 0x80000000;
    buf[n++] = 4; buf[n++] = ATAG_MEM;
    buf[n++] = 312 * MiB; buf[n++] = 0x8C800000;

    if (initrd_size) {
        buf[n++] = 4; buf[n++] = ATAG_INITRD2;
        buf[n++] = A5K_INITRD_ADDR; buf[n++] = initrd_size;
    }

    size_t len = strlen(cmdline) + 1;
    if (len > 3000) {
        error_report("sony-a5000: command line too long");
        exit(1);
    }
    uint32_t words = (len + 3) / 4;
    buf[n++] = 2 + words; buf[n++] = ATAG_CMDLINE;
    memset(&buf[n], 0, words * 4);
    memcpy(&buf[n], cmdline, len);
    n += words;

    buf[n++] = 0; buf[n++] = ATAG_NONE;

    for (int i = 0; i < n; i++) {
        buf[i] = cpu_to_le32(buf[i]);
    }
    rom_add_blob_fixed("a5k.atags", buf, n * 4, A5K_ATAG_ADDR);
}

/*
 * The Sony head.S never validates r2/ATAGs (its early init clobbers r2).
 * The bootloader instead hands over the command line through a magic
 * block at PA 0x80033000: "kemco" followed by the cmdline (setup_arch:
 * strncmp(p, "kemco", 5), then copies from p+5, '\n'/'\r' -> ' ').
 * Memory layout then comes from the mem=/memrsv= parameters.
 */
#define A5K_KEMCO_ADDR      0x80033000ULL
static void a5k_write_kemco(const char *cmdline)
{
    size_t len = strlen(cmdline);
    if (len + 6 > 0x1000) {
        error_report("sony-a5000: command line too long for kemco block");
        exit(1);
    }
    char *buf = g_malloc0(len + 6);
    memcpy(buf, "kemco", 5);
    memcpy(buf + 5, cmdline, len);
    address_space_write(&address_space_memory, A5K_KEMCO_ADDR,
                        MEMTXATTRS_UNSPECIFIED, buf, len + 6);
    g_free(buf);
}

/*
 * GICD_ITARGETSR8..63 (SPIs 32..255) as the Sony bootloader leaves them:
 * read from the real camera (re/device/hwdump/GICD.BIN, offset 0x820, fw
 * 1.10, running system).  Sony's gic_dist_init never programs targets and
 * irq_select_affinity (0x4004f0d8) derives each IRQ's affinity from this
 * byte, so it decides which CPU gets which interrupt -- e.g. the per-CPU
 * tick timers 153/155/156/157 -> CPU0/1/2/3 (without that, CPUs 1-3 get no
 * tick, RCU callbacks never run there and rcu_barrier() hangs umount).
 * Bytes that are 0 (IRQ unused on the real camera) become CPU0 here so a
 * source that is enabled anyway is not lost [deviation, harmless].
 */
static const uint32_t a5k_gicd_itargetsr[56] = {
    0x04010102, 0x04040404, 0x02020202, 0x02020202,
    0x02020202, 0x02020202, 0x02020202, 0x02020202,
    0x04020202, 0x04040404, 0x04040404, 0x02020201,
    0x04020200, 0x01040404, 0x02020000, 0x00000400,
    0x02020000, 0x02020200, 0x02020202, 0x04000000,
    0x00000400, 0x00000000, 0x02040100, 0x01040101,
    0x01010101, 0x04040104, 0x01040004, 0x01040101,
    0x01010004, 0x04020101, 0x02010101, 0x02010804,
    0x04010804, 0x04040404, 0x01010104, 0x02040402,
    0x02040402, 0x02020204, 0x02020202, 0x01010102,
    0x00010101, 0x01010101, 0x01020100, 0x01010101,
    0x02020201, 0x01020202, 0x01010002, 0x00010100,
    0x01000101, 0x01010101, 0x01000101, 0x01010101,
    0x01010101, 0x01010101, 0x00000000, 0x01010000,
};

static void a5k_write_stub(void)
{
    /*
     * What the Sony bootloader does before entering the kernel:
     *  - program the SPI targets (a5k_gicd_itargetsr, real-camera values);
     *  - make the ack-less frame sources edge-triggered: SYSV 98/99 and the
     *    VPH vsyncs 101/102 (their av-cam handlers never touch the device,
     *    as level interrupts they would re-enter endlessly) [hypothesis];
     *  - r0 = 0, r1 = machine id, r2 = tags, jump to the kernel.
     */
    uint32_t stub[21 + 56] = {
        0xe59f3030,             /* ldr r3, =0xF8001820        */
        0xe59f4030,             /* ldr r4, =0xF8001900        */
        0xe28f6044,             /* add r6, pc, #0x44 (table)  */
        0xe4965004,             /* 1: ldr r5, [r6], #4        */
        0xe4835004,             /* str r5, [r3], #4           */
        0xe1530004,             /* cmp r3, r4                 */
        0x3afffffb,             /* blo 1b                     */
        0xe59f301c,             /* ldr r3, =0xF8001C18 (ICFGR6) */
        0xe59f501c,             /* ldr r5, =0x28A0            */
        0xe5835000,             /* str r5, [r3]               */
        0xe3a00000,             /* mov r0, #0                 */
        0xe59f1014,             /* ldr r1, =A5K_MACH_ID       */
        0xe59f2014,             /* ldr r2, =A5K_ATAG_ADDR     */
        0xe59ff014,             /* ldr pc, =A5K_KERNEL_ADDR   */
        0xf8001820,
        0xf8001900,
        0xf8001c18,
        0x000028a0,             /* IRQ 98, 99, 101, 102 edge-triggered */
        A5K_MACH_ID,
        (uint32_t)A5K_ATAG_ADDR,
        (uint32_t)A5K_KERNEL_ADDR,
    };
    for (int i = 0; i < 56; i++) {
        uint32_t w = a5k_gicd_itargetsr[i];
        for (int b = 0; b < 4; b++) {
            if (!((w >> (8 * b)) & 0xff)) {
                w |= 1u << (8 * b);
            }
        }
        stub[21 + i] = w;
    }
    for (size_t i = 0; i < ARRAY_SIZE(stub); i++) {
        stub[i] = cpu_to_le32(stub[i]);
    }
    /* word 20 (the kernel entry literal) is left out of the ROM blobs:
     * a5k_boot_reset() writes it on every reset (normally
     * A5K_KERNEL_ADDR; the updater core enters elsewhere) */
    rom_add_blob_fixed("a5k.stub", stub, 20 * 4, A5K_STUB_ADDR);
    rom_add_blob_fixed("a5k.stub-gicd", &stub[21], 56 * 4,
                       A5K_STUB_ADDR + 21 * 4);
}

/*
 * Secondary-core boot ROM behaviour: wait for an event, then jump to the
 * address the boot CPU stored at 0xC0005010 + 4*(cpu-1)
 * (cxd90014_smp_init writes cxd90014_secondary_entry there and issues SEV).
 */
#define A5K_SMP_STUB_ADDR   0x80025200ULL
static void a5k_write_smp_stub(void)
{
    uint32_t stub[] = {
        0xee103fb0,             /* mrc p15, 0, r3, c0, c0, 5  (MPIDR) */
        0xe2033003,             /* and r3, r3, #3                     */
        0xe2433001,             /* sub r3, r3, #1                     */
        0xe59f2014,             /* ldr r2, =0xC0005010                */
        0xe0822103,             /* add r2, r2, r3, lsl #2             */
        0xe320f002,             /* 1: wfe                             */
        0xe5920000,             /* ldr r0, [r2]                       */
        0xe3500000,             /* cmp r0, #0                         */
        0x0afffffb,             /* beq 1b                             */
        0xe12fff10,             /* bx r0                              */
        0xc0005010,
    };
    for (size_t i = 0; i < ARRAY_SIZE(stub); i++) {
        stub[i] = cpu_to_le32(stub[i]);
    }
    rom_add_blob_fixed("a5k.smpstub", stub, sizeof(stub), A5K_SMP_STUB_ADDR);
}

static void a5k_cpu_reset(void *opaque)
{
    ARMCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);

    cpu_reset(cs);
    if (cs->cpu_index == 0) {
        cpu_set_pc(cs, A5K_STUB_ADDR);
    } else {
        cpu_set_pc(cs, A5K_SMP_STUB_ADDR);
    }
}

static void a5k_regfile_ex(const char *name, hwaddr base, uint64_t size,
                           const char *init, const char *autoclear, int prio)
{
    DeviceState *d = qdev_new("cxd90014-regfile");
    qdev_prop_set_string(d, "name", name);
    qdev_prop_set_uint64(d, "size", size);
    qdev_prop_set_uint64(d, "base", base);
    if (init) {
        qdev_prop_set_string(d, "init", init);
    }
    if (autoclear) {
        qdev_prop_set_string(d, "autoclear", autoclear);
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(d), &error_fatal);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(d), 0, base, prio);
}

static void a5k_regfile(const char *name, hwaddr base, uint64_t size)
{
    a5k_regfile_ex(name, base, size, NULL, NULL, -1000);
}

/*
 * Sub-micro (power/front micro) HLE, transport level only
 * (docs/RE_SUBMICRO_LINK.md).  The micro strobes p0.12 (IRQ 133, link event
 * 0) while its request line p3.17 stays low; av-cam then runs one 128-byte
 * full-duplex exchange: CPU status frame (type 0x31) out, micro status frame
 * (0x30) in.  On its periodic tick the CPU also clocks a 128-byte slot-A
 * packet; the micro's slot-A (0x20) comes back.  Both reply frames are the
 * real camera's own frames from the WBI snapshot (data/submicro_frames.bin;
 * the power-off flag in TLV 3 cleared, CRC-16/CCITT-FALSE recomputed).
 * Contents are replayed as-is, never synthesised.
 */
/* CRC-16/CCITT-FALSE over bytes 0..125, stored little-endian at 126 */
static void a5k_submicro_crc(uint8_t *f)
{
    uint16_t c = 0xffff;
    for (int i = 0; i < 126; i++) {
        c ^= f[i] << 8;
        for (int b = 0; b < 8; b++) {
            c = (c & 0x8000) ? (c << 1) ^ 0x1021 : c << 1;
        }
    }
    f[126] = c;
    f[127] = c >> 8;
}

/*
 * The media-slot switches are read by the sub-micro and reported in slot A:
 * byte 2 bit4 = slot 0 (SD/MS combo) card present, bit0 = slot 0 second
 * flag [? lock], bit5/bit1 = slot 1 (av-cam 439161D4/439162A8, used when the
 * slot config mode is <= 1; task 48 turns them into the 0x28017C front-media
 * notify that mcmn_drv forwards to libInfraMediaDetect).  The snapshot frame
 * says "no card"; with an SD drive attached, model the closed switch.
 */
static void a5k_submicro_card_inserted(uint8_t *frames)
{
    frames[2] |= 0x10;
    a5k_submicro_crc(frames);
}

/*
 * Front keys are read by the sub-micro too: slot A bytes 28/29 are the
 * "CAkey1" word (av-cam field parser 43AC73AE: b28 | b29 << 8, copied to the
 * task-48 state +24 = 0x43ED43C8 and served to input.ko on sync queue
 * 0x280132).  input.ko's key table (Backup 0x1120000): bit1 = PLAY (0x1F7),
 * bit2 = MENU (0x1AB), both active high.  The snapshot word is 0x0201.
 */
static void a5k_submicro_front_key(A5KMachineState *s, int bit, bool down)
{
    if (!s->submicro_frames) {
        return;
    }
    if (down) {
        s->submicro_frames[28] |= 1 << bit;
    } else {
        s->submicro_frames[28] &= ~(1 << bit);
    }
    a5k_submicro_crc(s->submicro_frames);
}

/*
 * Battery-backed clock of the sub-micro (docs/RE_RTC.md sections 1.1, 7).
 * RX slot A bytes 6..12 = {status 2 = valid, yy, mon, day, hour, min, sec},
 * binary, UTC, year 2000 + yy (av-cam 43AC65FA).  Time source: the QEMU RTC
 * (-rtc base=...) plus a session offset set by the CPU's clock writes.
 */
static void a5k_rtc_now(A5KMachineState *s, uint8_t *b)
{
    struct tm tm;

    qemu_get_timedate(&tm, s->rtc_offset);
    /*
     * udate_get_localtime clamps to [Backup default year (2014), 2037] and
     * rewrites the RTC when outside (RE_RTC.md section 2 step 5): keep the
     * reported year inside that range.
     */
    int year = MIN(MAX(tm.tm_year + 1900, 2014), 2037);
    b[0] = year - 2000;
    b[1] = tm.tm_mon + 1;
    b[2] = tm.tm_mday;
    b[3] = tm.tm_hour;
    b[4] = tm.tm_min;
    b[5] = tm.tm_sec;
}

/*
 * CPU -> micro "set clock": TX slot A (type 0x21) bytes 4..10 =
 * {0x02, yy, mon, day, hour, min, sec} UTC (av-cam 43AC663E).  The TX
 * builder 43914E28 keeps the request pending until rx[6] has bit 1 set and
 * rx[7..12] echo the written yy..ss, so the written bytes are reported
 * verbatim until the request is dropped, then the clock ticks on from them.
 * Observed: after the ack the CPU sends TX byte 4 = 0 again, so one request
 * is applied once (on its first packet) and the next write re-arms.
 */
#define A5K_RTC_ACK_REPLIES 20      /* hypothesis: cap of ~1 s of ticks */

static void a5k_rtc_tx(A5KMachineState *s, const uint8_t *tx)
{
    const uint8_t *w = tx + 5;
    struct tm want = { 0 }, now;

    if (tx[0] != 0x21) {
        return;
    }
    if (tx[4] != 2) {
        s->rtc_ack = 0;             /* request acknowledged and dropped */
        s->rtc_set_valid = false;
        return;
    }
    if (s->rtc_set_valid && !memcmp(s->rtc_set, w, 6)) {
        return;
    }
    if (w[1] < 1 || w[1] > 12 || w[2] < 1 || w[2] > 31 || w[3] > 23 ||
        w[4] > 59 || w[5] > 59) {
        qemu_log_mask(LOG_GUEST_ERROR, "sony-a5000: bad RTC write "
                      "%02x %02x %02x %02x %02x %02x\n",
                      w[0], w[1], w[2], w[3], w[4], w[5]);
        return;
    }
    want.tm_year = 100 + w[0];
    want.tm_mon = w[1] - 1;
    want.tm_mday = w[2];
    want.tm_hour = w[3];
    want.tm_min = w[4];
    want.tm_sec = w[5];
    qemu_get_timedate(&now, 0);
    s->rtc_offset = mktimegm(&want) - mktimegm(&now);
    memcpy(s->rtc_set, w, 6);
    s->rtc_set_valid = true;
    s->rtc_ack = A5K_RTC_ACK_REPLIES;
    info_report("sony-a5000: sub-micro RTC set to 20%02u-%02u-%02u "
                "%02u:%02u:%02u UTC", w[0], w[1], w[2], w[3], w[4], w[5]);
}

/*
 * Micro memory read/written through slot-A packets 0x41 (docs/RE_REBOOT.md
 * section A.4): the four regions of av-cam's table 0x43EA797C.  On a cold
 * power-off av-cam reads 0x20000500 and 0x00030100 (256 bytes each) and
 * only commits the micro's power mode 0x0D20 once both reads completed.
 * hypothesis: the real content is unknown; nothing on the off path checks
 * it, so zero-filled regions stand in.
 */
static const struct {
    uint32_t addr, len;
} a5k_sm_regions[4] = {
    { 0x00010100, 0x080 },
    { 0x00020280, 0x180 },
    { 0x20000500, 0x100 },
    { 0x00030100, 0x100 },
};

static uint8_t *a5k_sm_mem(A5KMachineState *s, uint32_t addr, int n)
{
    for (int i = 0; i < ARRAY_SIZE(a5k_sm_regions); i++) {
        uint32_t off = addr - a5k_sm_regions[i].addr;
        if (addr >= a5k_sm_regions[i].addr &&
            off + n <= a5k_sm_regions[i].len) {
            return s->sm_mem[i] + off;
        }
    }
    qemu_log_mask(LOG_UNIMP, "sony-a5000: sub-micro memory 0x%08x+%d "
                  "outside the known regions (zeros)\n", addr, n);
    return NULL;
}

/*
 * Slot-A 0x41 = CPU memory request (av-cam 43915604/4391383C): bytes 1..4
 * micro address (LE), byte 5 = length (<= 0x78), bit 7 = write with the data
 * at 6...  The answer is due in the RX of the NEXT exchange (the micro loads
 * its TX before the transfer): type 0x40, bytes 1..4 address echo [? not
 * checked by av-cam], byte 5 length, data at 6.. (parser 439149F8/43913AC0).
 * A write is answered with length 0 [? write ack not decoded; not used on
 * the power-off path].
 */
static void a5k_submicro_mem_reply(A5KMachineState *s, uint8_t *out)
{
    uint32_t addr = ldl_le_p(s->sm_rd_hdr);
    bool wr = s->sm_rd_hdr[4] & 0x80;
    int n = wr ? 0 : MIN(s->sm_rd_hdr[4] & 0x7f, 0x78);
    uint8_t *m = n ? a5k_sm_mem(s, addr, n) : NULL;

    memset(out, 0, 128);
    out[0] = 0x40;
    memcpy(out + 1, s->sm_rd_hdr, 4);
    out[5] = n;
    if (m) {
        memcpy(out + 6, m, n);
    }
    a5k_submicro_crc(out);
}

static void a5k_submicro_mem_tx(A5KMachineState *s, const uint8_t *tx)
{
    s->sm_rd_pending = tx[0] == 0x41;
    if (!s->sm_rd_pending) {
        return;
    }
    memcpy(s->sm_rd_hdr, tx + 1, 5);
    if (tx[5] & 0x80) {
        int n = MIN(tx[5] & 0x7f, 0x78);
        uint8_t *m = a5k_sm_mem(s, ldl_le_p(tx + 1), n);
        if (m) {
            memcpy(m, tx + 6, n);
        }
    }
}

static void a5k_submicro_slot_a(A5KMachineState *s, const uint8_t *tx,
                                uint8_t *out)
{
    if (s->sm_rd_pending) {
        a5k_submicro_mem_reply(s, out);
    } else {
        memcpy(out, s->submicro_frames, 128);
        out[6] = 2;                 /* clock valid (bit 1 = write ack) */
        if (s->rtc_ack > 0) {
            s->rtc_ack--;
            memcpy(out + 7, s->rtc_set, 6);
        } else {
            a5k_rtc_now(s, out + 7);
        }
        a5k_submicro_crc(out);
    }
    a5k_submicro_mem_tx(s, tx);
    if (tx[0] != 0x41) {
        memcpy(s->sa_tx_last, tx, 128);
        s->sa_tx_valid = true;
        if (!s->sa_tx_boot_valid) {
            memcpy(s->sa_tx_boot, tx, 128);
            s->sa_tx_boot_valid = true;
        }
    }
}

/* find TLV @id in a slot-B frame (RE_SUBMICRO_LINK.md section 3.1) */
static uint8_t *a5k_submicro_tlv(uint8_t *f, int id, int minlen)
{
    int end = MIN(4 + f[2], 126);

    for (int o = 4; o + 4 <= end; o += 4 + f[o + 1]) {
        if (f[o] == id) {
            return f[o + 1] >= minlen && o + 4 + minlen <= end ?
                   f + o + 4 : NULL;
        }
    }
    return NULL;
}

/*
 * Lens power is switched by the micro (Backup 0x01250007 = 1): the CPU asks
 * in its slot-B TLV 0x0A data[1] bit0 (av-cam 436B760C), the micro answers
 * in TLV 0x03 data[3] bit0 (consumer 436B75CC; RE_LENS_EMOUNT.md 2.3).  Only
 * acknowledged with a lens model attached (lens=ekit).
 *
 * Power mode of the link (docs/RE_REBOOT.md sections A.2, A.3): the CPU asks
 * for mode 0x0D20 (cold power-off) with TX TLV 0x0A data[1] bit 6 and for
 * 0x0D09 with bit 1 (av-cam 4369C580); the slot-B RX parser 4369B88C only
 * commits the mode once the micro reports RX TLV 0x03 data[1] bit 6 (resp.
 * bit 0 for 0x0D09 [? static only, not exercised]).  The snapshot frame
 * comes from a camera that was powering off (bit 6 set; mkdata.py clears
 * it), so the ack follows the request: set while requested.  Answered in
 * the same exchange [? real micro latency unknown; 4369B88C re-checks every
 * frame].
 */
static void a5k_submicro_slot_b(A5KMachineState *s, uint8_t *tx, uint8_t *out)
{
    uint8_t *t;
    bool dirty = false;

    if (tx[0] == 0x31 && (t = a5k_submicro_tlv(tx, 0x0a, 2))) {
        s->lens_pwr_req = t[1] & 1;
        s->poff_req = t[1] & 0x42;
    }
    memcpy(out, s->submicro_frames + 128, 128);
    if (s->lens_ekit && s->lens_pwr_req &&
        (t = a5k_submicro_tlv(out, 0x03, 4))) {
        t[3] |= 1;
        dirty = true;
    }
    if (s->poff_req && (t = a5k_submicro_tlv(out, 0x03, 2))) {
        if (s->poff_req & 0x40) {
            t[1] |= 0x40;
        }
        if (s->poff_req & 0x02) {
            t[1] |= 0x01;
        }
        dirty = true;
        if (!s->poff_ack_logged) {
            s->poff_ack_logged = true;
            info_report("sony-a5000: sub-micro power-off request "
                        "acknowledged (TLV 0x0a data[1] = 0x%02x)",
                        s->poff_req);
        }
    }
    if (dirty) {
        a5k_submicro_crc(out);
    }
}

static void a5k_submicro_xfer(void *opaque, int ch, uint8_t *buf, int len)
{
    A5KMachineState *s = opaque;
    uint8_t out[256];

    if (len < 128 || !s->submicro_frames) {
        memset(buf, 0, len);            /* 8-byte header phase: nothing */
        return;
    }
    /* buf holds the CPU's TX packet(s) until it is overwritten below */
    if (buf[0] == 0x31) {
        a5k_submicro_slot_b(s, buf, out);
    } else {
        a5k_rtc_tx(s, buf);
        a5k_submicro_slot_a(s, buf, out);
    }
    if (len > 128) {
        /* 256-byte exchange: slot A followed by slot B */
        a5k_submicro_slot_b(s, buf + 128, out + 128);
    }
    memcpy(buf, out, MIN(len, 256));
}

/*
 * Lens handshake (RE_LENS_EMOUNT.md 2.4 / 5.2): the lens echoes the body's
 * P5.4 on P0.5 and P2.11 on P2.10.  Echo delay 1 ms [hypothesis; the body
 * polls every 5 ms].  Once 43993910 muxes P2.10/P2.11 to UART1 the latch
 * no longer moves, so the echo simply stops changing [?].
 */
static void a5k_lens_hs_cb(void *opaque)
{
    A5KMachineState *s = opaque;
    qemu_set_irq(qdev_get_gpio_in(s->gpio, 0 * 32 + 5), s->lens_hs[0]);
    qemu_set_irq(qdev_get_gpio_in(s->gpio, 2 * 32 + 10), s->lens_hs[1]);
}

static void a5k_lens_hs_out(void *opaque, int n, int level)
{
    A5KMachineState *s = opaque;
    s->lens_hs[n] = level;
    timer_mod(s->lens_hs_t, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SCALE_MS);
}

static void a5k_submicro_tick(void *opaque)
{
    A5KMachineState *s = opaque;
    if (!s->gpio) {
        return;
    }
    /* p0.12 must be LOW when the engine tick samples it, otherwise the CPU
     * skips its slot-A exchange ("FrtComSkip"): give a 1 ms strobe every
     * 50 ms (each edge is a link event 0; the trailing one is ignored in
     * the states it lands in). */
    qemu_set_irq(qdev_get_gpio_in(s->gpio, 3 * 32 + 17), 0); /* request low */
    s->submicro_step ^= 1;
    qemu_set_irq(qdev_get_gpio_in(s->gpio, 0 * 32 + 12), s->submicro_step);
    timer_mod(s->submicro_t, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              (s->submicro_step ? SCALE_MS : 49 * SCALE_MS));
}

/*
 * Camera buttons from the host keyboard (QEMU window or HMP "sendkey").
 * Wiring from input.ko's key table (Backup 0x1120000, docs/RE_INPUT_I2C_POWER.md
 * section 7); Sony key codes named by libObj's key-name table (file 0xEE1078:
 * {code, "KEY_TACT_..."}) and mapped to event0 codes by libInputServant
 * (0x3364: 0x03->103, 0x30->108, 0x1AF->106, 0x15->105, 0x1AC->232, ...):
 *   ADC ch 0x0D (= unit 1 ch 1) control-wheel resistor ladder, pressed bands
 *     0..91 ENTER (0x1AC), 92..273 UP (0x03), 274..439 LEFT (0x15),
 *     440..599 DOWN (0x30), 600..772 RIGHT (0x1AF); released = 1023
 *   ADC ch 0x0C (= unit 1 ch 0) 0..91 DELETE (0x1F8)
 *   ADC ch 0x0F (= unit 1 ch 3) zoom lever, one key 0x1BF whose value is
 *     the band: 80..244 = 2, 245..358 = 0x8002, 359..442 = 0x8001,
 *     443..514 = 1, 515..1023 = 0 (centre).  Reading 1/2 = tele slow/fast,
 *     0x8001/0x8002 = wide slow/fast [? sign bit = wide, not verified]
 *   GPIO P10.10 active low = STARTSTOP (0x1F0, the MOVIE button)
 *   sub-micro slot A byte 28 bit 2 MENU (0x1AB), bit 1 PLAY (0x1F7)
 *   GPIO p0.1/p0.11 S1 (0x1F1), p1.5/p0.13 S2 (0xA7), all active low
 * Host keys: arrows, Enter = centre, M = MENU, P = PLAY,
 * Delete/Backspace = delete, R = MOVIE, T/W = zoom tele/wide (slow; with
 * Shift held = fast), S = shutter half press, Shift+S or S+Space = full.
 * Band mid-points are used.
 */
static void a5k_adc_key(A5KMachineState *s, int ch, int qcode, bool down,
                        int level)
{
    if (!s->adc[1]) {
        return;
    }
    if (down) {
        s->adc_key[ch] = qcode;
        cxd90014_adc_set(s->adc[1], ch, level);
    } else if (s->adc_key[ch] == qcode) {
        s->adc_key[ch] = 0;
        cxd90014_adc_set(s->adc[1], ch, 1023);
    }
}

/*
 * Shutter: S1 (half) = p0.1 + p0.11, S2 (full) = p1.5 + p0.13, active low
 * (see hw/gpio/cxd90014_gpio.c reset).  Each switch drives both of its
 * interrupt lines [? two lines per switch inferred from av-cam's IRQ set].
 */
static void a5k_shutter(A5KMachineState *s, bool s1, bool s2)
{
    if (!s->gpio) {
        return;
    }
    qemu_set_irq(qdev_get_gpio_in(s->gpio, 0 * 32 + 1), !s1);
    qemu_set_irq(qdev_get_gpio_in(s->gpio, 0 * 32 + 11), !s1);
    qemu_set_irq(qdev_get_gpio_in(s->gpio, 1 * 32 + 5), !s2);
    qemu_set_irq(qdev_get_gpio_in(s->gpio, 0 * 32 + 13), !s2);
}

static void a5k_key_event(DeviceState *dev, QemuConsole *src,
                          InputEvent *evt)
{
    A5KMachineState *s = A5K_MACHINE(qdev_get_machine());
    InputKeyEvent *key = evt->u.key.data;
    int qcode = qemu_input_key_value_to_qcode(key->key);
    bool down = key->down;

    switch (qcode) {
    case Q_KEY_CODE_SHIFT:
    case Q_KEY_CODE_SHIFT_R:
        s->key_shift = down;
        break;
    case Q_KEY_CODE_S:
        /* S = half press, Shift+S = full press (passes through S1) */
        if (down) {
            s->key_s2 = s->key_shift;
            a5k_shutter(s, true, s->key_s2);
        } else {
            s->key_s2 = false;
            a5k_shutter(s, false, false);
        }
        break;
    case Q_KEY_CODE_SPC:
        /* Space while S is held = full press */
        s->key_s2 = down;
        a5k_shutter(s, true, down);
        if (!down) {
            a5k_shutter(s, false, false);
        }
        break;
    case Q_KEY_CODE_T:
        a5k_adc_key(s, 3, qcode, down, s->key_shift ? 162 : 478);
        break;
    case Q_KEY_CODE_W:
        a5k_adc_key(s, 3, qcode, down, s->key_shift ? 301 : 400);
        break;
    case Q_KEY_CODE_RET:
    case Q_KEY_CODE_KP_ENTER:
        a5k_adc_key(s, 1, qcode, down, 45);
        break;
    case Q_KEY_CODE_UP:
        a5k_adc_key(s, 1, qcode, down, 182);
        break;
    case Q_KEY_CODE_LEFT:
        a5k_adc_key(s, 1, qcode, down, 356);
        break;
    case Q_KEY_CODE_DOWN:
        a5k_adc_key(s, 1, qcode, down, 520);
        break;
    case Q_KEY_CODE_RIGHT:
        a5k_adc_key(s, 1, qcode, down, 686);
        break;
    case Q_KEY_CODE_DELETE:
    case Q_KEY_CODE_BACKSPACE:
        a5k_adc_key(s, 0, qcode, down, 45);
        break;
    case Q_KEY_CODE_M:
        a5k_submicro_front_key(s, 2, down);
        break;
    case Q_KEY_CODE_P:
        a5k_submicro_front_key(s, 1, down);
        break;
    case Q_KEY_CODE_R:
        if (s->gpio) {
            qemu_set_irq(qdev_get_gpio_in(s->gpio, 10 * 32 + 10), !down);
        }
        break;
    default:
        break;
    }
}

static const QemuInputHandler a5k_key_handler = {
    .name  = "sony-a5000 buttons",
    .mask  = INPUT_EVENT_MASK_KEY,
    .event = a5k_key_event,
};

/* ---- per-reset boot (a legacy reset handler; it is registered before
 * rom_reset, so it must not write into ROM blobs) ---- */

#define A5K_LOADER_DESC     0x80027000ULL
#define A5K_DESC_MAGIC      0x424b3541      /* "A5KB" */
/*
 * Optional descriptor extension (words 5..7): {"A5KX", kdst, idst}.
 * Stage 1 writes it when it boots an image whose load addresses are not
 * the normal ones -- the updater core on nflasha1 (/boot/loadaddr.txt
 * "0x81528000,0x80628000": a zImage entered at its load address, cramfs
 * initrd at 0x80628000).  See docs/RE_UPDATER_BOOT.md.
 */
#define A5K_DESC_XMAGIC     0x584b3541      /* "A5KX" */
#define A5K_STUB_ENTRY_LIT  (A5K_STUB_ADDR + 20 * 4)  /* ldr pc literal */
#define A5K_SMP_MBOX        0xC0005010ULL

/*
 * Reset kinds (docs/RE_REBOOT.md section 4.1).  A SoC reset (SYSCTL +0x44,
 * WDT, HMP system_reset) keeps the DDR contents.  A power cycle by the
 * sub-micro loses them: the IRAM sleep code puts DDR in deep power-down
 * ("ddr_ctrl=dpd") and the SoC loses power, so a5k_boot_reset clears all
 * of DDR and IRAM before rom_reset restores the ROM blobs (zero stands in
 * for the undefined content after power loss).
 */
enum {
    A5K_RESET_SOC = 0,
    A5K_RESET_POWER,
};

#define A5K_IRAM_BASE       0xC0000000ULL
#define A5K_IRAM_SIZE       (16 * MiB)
#define A5K_GPIO_BASE       0xF2910000ULL
#define A5K_POFF_PIN        10      /* p0.10, cfg.poff=0,10 */
#define A5K_POFF_DELAY_MS   100     /* hypothesis: lets the wfi loop settle */

static void a5k_dump_sa_tx(const char *what, const uint8_t *f)
{
    for (int o = 0; o < 64; o += 16) {
        info_report("sony-a5000: slot-A TX %s +0x%02x: %02x %02x %02x %02x "
                    "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x "
                    "%02x", what, o, f[o], f[o + 1], f[o + 2], f[o + 3],
                    f[o + 4], f[o + 5], f[o + 6], f[o + 7], f[o + 8],
                    f[o + 9], f[o + 10], f[o + 11], f[o + 12], f[o + 13],
                    f[o + 14], f[o + 15]);
    }
}

/*
 * Restart/factor bytes of the slot-A TX packet (RE_REBOOT.md 4.4).  Seen at
 * the p0.10 drop of an OFF_BY_UPDATE (pmca updatershell, factor 13, cold):
 *   +0x14 0d  +0x15 20  +0x1B 80  +0x1D 80  +0x1E 0d  +0x20 01  +0x21 81
 *   +0x31 80
 * i.e. the RE_REBOOT.md 4.4 table with the entries 0x1F..0x25 four bytes
 * lower (0x14, 0x15 and 0x31 match).  hypothesis: 0x1E = off factor,
 * 0x20 bit 0 = restart, 0x21 = cold flag | 0x80; a plain off (restart 0)
 * has not been observed yet.  Logged on every off; the decision itself is
 * the poff= option for now.
 */
#define A5K_SA_TX_FACTOR    0x1E
#define A5K_SA_TX_RESTART   0x20

static void a5k_poff_cycle(void *opaque)
{
    A5KMachineState *s = opaque;
    int factor = s->sa_tx_valid ? s->sa_tx_last[A5K_SA_TX_FACTOR] : -1;

    info_report("sony-a5000: sub-micro power cycle (factor %d)", factor);
    s->reset_kind = A5K_RESET_POWER;
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
}

/*
 * Power-off line p0.10 (RE_REBOOT.md 1.4 step 3, 4.4): the IRAM sleep code
 * at 0xC00200C8 writes 0xF2910048 = 0x400 (p0 output clear, from the
 * kernel's cfg.poff=0,10) and then loops on wfi; the sub-micro sees the line
 * drop and removes power.  With restart requested it powers the SoC up
 * again (power cycle), otherwise it stays off.
 */
static void a5k_poff_line(void *opaque, int n, int level)
{
    A5KMachineState *s = opaque;
    bool fall = s->poff_level && !level;

    s->poff_level = level;
    if (!fall) {
        return;
    }
    /* r15 is only exact to the translation block: enough to tell IRAM */
    uint32_t pc = current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0;
    info_report("sony-a5000: power-off line p0.10 low (pc ~0x%08x, "
                "off request 0x%02x)", pc, s->poff_req);
    if (s->sa_tx_boot_valid) {
        a5k_dump_sa_tx("at boot", s->sa_tx_boot);
    }
    if (s->sa_tx_valid) {
        a5k_dump_sa_tx("at off", s->sa_tx_last);
        info_report("sony-a5000: slot-A TX factor byte 0x%02x = 0x%02x, "
                    "restart byte 0x%02x = 0x%02x [?]", A5K_SA_TX_FACTOR,
                    s->sa_tx_last[A5K_SA_TX_FACTOR], A5K_SA_TX_RESTART,
                    s->sa_tx_last[A5K_SA_TX_RESTART]);
    }
    if (s->poff_restart) {
        timer_mod(s->poff_t, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  A5K_POFF_DELAY_MS * SCALE_MS);
    } else {
        info_report("sony-a5000: sub-micro power off");
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
    }
}

/*
 * Bootloader stand-in: p0.10 is left driven high ("power hold") at power-on
 * [hypothesis: Linux never configures p0.10 (p0 dir = 0x40, out = 0x01 at
 * the shooting screen) yet the sleep code drives it low to power off, so it
 * must have been driven high before the kernel].  Runs after the device
 * reset cleared the GPIO latch.
 */
static void a5k_poff_hold(A5KMachineState *s)
{
    uint32_t bit = cpu_to_le32(1u << A5K_POFF_PIN);

    s->poff_level = 0;
    if (!s->gpio) {
        return;
    }
    address_space_write(&address_space_memory, A5K_GPIO_BASE + 0x44,
                        MEMTXATTRS_UNSPECIFIED, &bit, 4);   /* out set */
    address_space_write(&address_space_memory, A5K_GPIO_BASE + 0x04,
                        MEMTXATTRS_UNSPECIFIED, &bit, 4);   /* dir set */
}

/* sub-micro power-off / memory-read state (micro side, RE_REBOOT.md A.5) */
static void a5k_submicro_poff_reset(A5KMachineState *s)
{
    s->poff_req = 0;
    s->poff_ack_logged = false;
    s->sm_rd_pending = false;
    memset(s->sm_mem, 0, sizeof(s->sm_mem));
    s->sa_tx_valid = s->sa_tx_boot_valid = false;
    if (s->poff_t) {
        timer_del(s->poff_t);
    }
}

static bool a5k_ddr_range(uint32_t pa, uint32_t len)
{
    return pa >= A5K_DDR_BASE && len <= A5K_DDR_SIZE &&
           pa - A5K_DDR_BASE <= A5K_DDR_SIZE - len;
}

static void a5k_boot_reset(void *opaque)
{
    A5KMachineState *s = opaque;
    AddressSpace *as = &address_space_memory;
    uint32_t d[8];
    bool ext;

    if (s->reset_kind == A5K_RESET_POWER) {
        /* power loss: DDR and IRAM content gone, ROM blobs come back next */
        address_space_set(as, A5K_DDR_BASE, 0, A5K_DDR_SIZE,
                          MEMTXATTRS_UNSPECIFIED);
        address_space_set(as, A5K_IRAM_BASE, 0, A5K_IRAM_SIZE,
                          MEMTXATTRS_UNSPECIFIED);
        info_report("sony-a5000: power-on (DDR and IRAM cleared)");
    }
    s->reset_kind = A5K_RESET_SOC;
    a5k_poff_hold(s);
    a5k_submicro_poff_reset(s);

    address_space_read(as, A5K_LOADER_DESC, MEMTXATTRS_UNSPECIFIED, d,
                       sizeof(d));
    for (int i = 0; i < 8; i++) {
        d[i] = le32_to_cpu(d[i]);
    }
    /*
     * A reset sends the secondaries back to the boot-ROM wait loop
     * (a5k_write_smp_stub); the release mailbox written by the previous
     * kernel's cxd90014_smp_init must not survive it, or they jump into
     * that kernel's (now stale) secondary entry before the new kernel
     * releases them.  On power-on the IRAM is zero, which is the state
     * reproduced here [hypothesis: the real boot ROM/bootloader clears
     * it; the normal boot only worked because stage 2 reloads the same
     * kernel at the same address].
     */
    uint32_t mbox[3] = { 0 };
    address_space_write(as, A5K_SMP_MBOX, MEMTXATTRS_UNSPECIFIED, mbox,
                        sizeof(mbox));

    uint32_t entry = cpu_to_le32(A5K_KERNEL_ADDR);
    address_space_write(as, A5K_STUB_ENTRY_LIT, MEMTXATTRS_UNSPECIFIED,
                        &entry, 4);

    ext = d[0] == A5K_DESC_MAGIC && d[5] == A5K_DESC_XMAGIC &&
          a5k_ddr_range(d[6], d[2]) && a5k_ddr_range(d[7], d[4]) &&
          (d[6] >= d[7] + d[4] || d[7] >= d[6] + d[2]);
    if (d[0] == A5K_DESC_MAGIC && d[2] &&
        (ext || (d[2] <= A5K_INITRD_ADDR - A5K_KERNEL_ADDR &&
                 d[4] <= 8 * MiB))) {
        /* stage 2: kernel + initrd read from NAND by stage 1 */
        uint32_t kdst = ext ? d[6] : A5K_KERNEL_ADDR;
        uint32_t idst = ext ? d[7] : A5K_INITRD_ADDR;
        g_autofree uint8_t *k = g_malloc(d[2]);
        g_autofree uint8_t *r = g_malloc(MAX(d[4], 1));
        address_space_read(as, d[1], MEMTXATTRS_UNSPECIFIED, k, d[2]);
        address_space_read(as, d[3], MEMTXATTRS_UNSPECIFIED, r, d[4]);
        address_space_write(as, kdst, MEMTXATTRS_UNSPECIFIED, k, d[2]);
        address_space_write(as, idst, MEMTXATTRS_UNSPECIFIED, r, d[4]);
        uint32_t zero[6] = { 0 };
        address_space_write(as, A5K_LOADER_DESC, MEMTXATTRS_UNSPECIFIED,
                            zero, sizeof(zero));
        if (ext) {
            /* enter the image at its load address (boot stub literal) */
            entry = cpu_to_le32(kdst);
            address_space_write(as, A5K_STUB_ENTRY_LIT,
                                MEMTXATTRS_UNSPECIFIED, &entry, 4);
            info_report("sony-a5000: stage 2 — kernel %u B @0x%08x, "
                        "initrd %u B @0x%08x from NAND", d[2], kdst, d[4],
                        idst);
            /* this kernel carries its own built-in command line and has
             * no kemco parser: no kemco block */
            goto out;
        }
        info_report("sony-a5000: stage 2 — kernel %u B, initrd %u B from NAND",
                    d[2], d[4]);
    } else {
        address_space_write(as, A5K_KERNEL_ADDR, MEMTXATTRS_UNSPECIFIED,
                            s->kernel, s->kernel_len);
        if (s->initrd_len) {
            address_space_write(as, A5K_INITRD_ADDR, MEMTXATTRS_UNSPECIFIED,
                                s->initrd, s->initrd_len);
            /*
             * The cmdline declares an 8 MiB initrd window and the kernel
             * unpacks all of it; after a warm reset the rest still held
             * the previous kernel's data ("junk in compressed archive",
             * then a panic in mount_block_root).  The real bootloader's
             * ext2 initrd ignores trailing bytes; ours is a cpio, so clear
             * the remainder of the window (docs/RE_REBOOT.md).
             */
            if (s->initrd_len < 8 * MiB) {
                address_space_set(as, A5K_INITRD_ADDR + s->initrd_len, 0,
                                  8 * MiB - s->initrd_len,
                                  MEMTXATTRS_UNSPECIFIED);
            }
        }
        if (s->loader) {
            info_report("sony-a5000: stage 1 (loader)");
        }
    }
    a5k_write_kemco(s->cmdline);
out:
    s->lens_pwr_req = false;
    s->lens_hs[0] = s->lens_hs[1] = 0;
    s->rtc_ack = 0;
    if (s->lens_hs_t) {
        timer_del(s->lens_hs_t);
    }
    if (s->submicro_t && s->submicro) {
        s->submicro_step = 0;
        timer_mod(s->submicro_t, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  8 * NANOSECONDS_PER_SECOND);
    }
}

static void a5k_init_done(Notifier *n, void *data)
{
    A5KMachineState *s = container_of(n, A5KMachineState, init_done);
    qemu_register_reset(a5k_boot_reset, s);   /* before rom_reset */
}

/* ---- machine ---- */

static void a5k_init(MachineState *ms)
{
    A5KMachineState *s = A5K_MACHINE(ms);
    MemoryRegion *sysmem = get_system_memory();
    unsigned int ncpu = ms->smp.cpus;
    DeviceState *mp, *dev;
    qemu_irq pic[A5K_NUM_IRQ - 32];

    if (ms->ram_size != A5K_DDR_SIZE) {
        error_report("sony-a5000: RAM is fixed at 512M");
        exit(1);
    }
    memory_region_add_subregion(sysmem, A5K_DDR_BASE, ms->ram);

    /* 16 MiB on-chip memory at PA 0xC0000000 (kernel map_io maps it at
     * VA 0xC0000000 cached and 0xC1000000 uncached).  0xC0005000 is the
     * secondary-CPU release mailbox written by cxd90014_smp_init. */
    memory_region_init_ram(&s->iram, NULL, "a5k.iram", 16 * MiB, &error_fatal);
    memory_region_add_subregion(sysmem, 0xC0000000, &s->iram);

    for (unsigned i = 0; i < ncpu; i++) {
        Object *o = object_new(ms->cpu_type);
        object_property_set_int(o, "reset-cbar", A5K_PERIPH_BASE,
                                &error_abort);
        /* The kernel enters in SVC mode from a non-secure-agnostic
         * bootloader; keep EL3 so secure CP15 accesses behave. */
        qdev_realize(DEVICE(o), NULL, &error_fatal);
        s->cpu[i] = ARM_CPU(o);
        qemu_register_reset(a5k_cpu_reset, s->cpu[i]);
    }

    /* ARM7 sub-CPU SRAM (UDIF "arm7_sram", 16 KiB at PA 0).  The Sony
     * bootloader leaves the NAND geometry at +0x3F00 for lld.ko
     * (boss_lld_direct_get_dev_info_save_sram).  The 13 words below are the
     * real camera's copy, recovered from lld's .bss (VA 0x3f018b78) in the
     * WBI RAM snapshot via its page tables. */
    memory_region_init_ram(&s->arm7_sram, NULL, "a5k.arm7-sram", 16 * KiB,
                           &error_fatal);
    memory_region_add_subregion(sysmem, 0x00000000, &s->arm7_sram);
    {
        static const uint32_t nand_devinfo[13] = {
            0x00000000, 0xc0000c00, 0x00000006, 0x00000001, 0x00000800,
            0x0000002e, 0x00000005, 0x0000c350, 0, 0, 0, 0, 0x00000004,
        };
        uint32_t le[13];
        for (int i = 0; i < 13; i++) {
            le[i] = cpu_to_le32(nand_devinfo[i]);
        }
        rom_add_blob_fixed("a5k.nand-devinfo", le, sizeof(le), 0x3F00);

        /* ARM7 NAND firmware already running (SRAM+0x3F40 firm_status = 2):
         * on a cold boot lld.ko never (re)starts the ARM7. */
        uint32_t two = cpu_to_le32(2);
        rom_add_blob_fixed("a5k.arm7-status", &two, 4, 0x3F40);

        /* On-chip RAM left by the bootloader: initial bad-block list and
         * init_param + init_reg_param for nand.ko boot_mode=1/resume.
         * Both read from the real camera (/dev/mem, PA 0xC0000C00 /
         * 0xC0001500, re/device/hwdump/IRAM.BIN): u16 list {0x10 (factory
         * bad block of this unit), 0..5 (boot area)}, init_param[2] = 7. */
        static const uint16_t ibt[16] = {
            0x0010, 0, 1, 2, 3, 4, 5, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
            0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
        };
        rom_add_blob_fixed("a5k.nand-ibt", ibt, sizeof(ibt), 0xC0000C00);
        static const uint32_t ip[5 + 50] = {
            0x00000000, 0xC0000C00, 0x00000007, 0x00000004, 0xC0001514,
            0x00000000, 0x000001f4, 0x000001f4, 0x000001f4, 0x000001f4,
            0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
            0x00000000, 0x00000001, 0x00000001, 0x00000001, 0x00001f1f,
            0x00000b4d, 0x0000001a, 0x0000000c, 0x00000000, 0x00000040,
            0x00000001, 0x00001000, 0x00000100, 0x00000000, 0x00000000,
            0x00420010, 0x00000000, 0x00000000, 0x00000000, 0x00000008,
            0x00000003, 0x00000001, 0x0001a000, 0x00000000, 0x0000ffff,
            0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x0000001a,
            0x001f013b, 0x0000ffff, 0x000000ec, 0x00000000, 0x00000010,
            0x00000001, 0x00000800, 0x0000002e, 0x00000005, 0x0000c350,
        };
        rom_add_blob_fixed("a5k.nand-initparam", ip, sizeof(ip), 0xC0001500);
    }

    /* Cortex-A5 MPCore private peripherals: same map as A9 MPCore. */
    mp = qdev_new(TYPE_A9MPCORE_PRIV);
    qdev_prop_set_uint32(mp, "num-cpu", ncpu);
    qdev_prop_set_uint32(mp, "num-irq", A5K_NUM_IRQ);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(mp), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(mp), 0, A5K_PERIPH_BASE);
    for (unsigned i = 0; i < ncpu; i++) {
        DeviceState *cd = DEVICE(s->cpu[i]);
        sysbus_connect_irq(SYS_BUS_DEVICE(mp), i,
                           qdev_get_gpio_in(cd, ARM_CPU_IRQ));
        sysbus_connect_irq(SYS_BUS_DEVICE(mp), i + ncpu,
                           qdev_get_gpio_in(cd, ARM_CPU_FIQ));
    }
    for (int i = 0; i < A5K_NUM_IRQ - 32; i++) {
        pic[i] = qdev_get_gpio_in(mp, i);
    }

    sysbus_create_varargs("l2x0", A5K_L2C_BASE, NULL);

    for (int i = 0; i < 3; i++) {
        pl011_create(A5K_UART_BASE + i * 0x1000, pic[GIC_ID(150 + i)],
                     serial_hd(i));
    }

    dev = qdev_new("cxd4115-timer");
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, A5K_TIMER_BASE);
    for (int i = 0; i < 16; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), i, pic[GIC_ID(153 + i)]);
    }

    /* Watchdog: a CXD4115 timer channel whose CTRL bit31 resets the SoC. */
    dev = qdev_new("cxd4115-timer");
    qdev_prop_set_uint32(dev, "num-channels", 1);
    qdev_prop_set_bit(dev, "watchdog", true);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0xF2402000);

    /* SD slot: Fujitsu F_SDH30 = SDHCI v3 (vendor regs 0x100+ fall into the
     * register file), mmc0 @0xF0300000 GIC 218.  -drive if=sd,file=... */
    DriveInfo *sd = drive_get(IF_SD, 0, 0);
    {
        DeviceState *h = qdev_new("generic-sdhci");
        qdev_prop_set_uint8(h, "sd-spec-version", 3);
        qdev_prop_set_uint8(h, "uhs", 0);
        qdev_prop_set_uint64(h, "capareg", 0x017834b4);  /* 3.3V, HS, ADMA2, SDMA */
        sysbus_realize_and_unref(SYS_BUS_DEVICE(h), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(h), 0, 0xF0300000);
        sysbus_connect_irq(SYS_BUS_DEVICE(h), 0, pic[GIC_ID(218)]);
        if (sd) {
            DeviceState *card = qdev_new("sd-card");
            qdev_prop_set_drive_err(card, "drive", blk_by_legacy_dinfo(sd),
                                    &error_fatal);
            qdev_realize_and_unref(card, qdev_get_child_bus(h, "sd-bus"),
                                   &error_fatal);
        }
    }

    dev = qdev_new("cxd90014-sysctl");
    qdev_prop_set_bit(dev, "sd-present", sd != NULL);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0xF2915000);

    /* GPIO ports p0..p17; pin IRQs p0.0-15 -> 121+, p0.16 -> 144,
     * p1.0-6 -> 137+ */
    dev = qdev_new("cxd90014-gpio");
    if (!s->lens || !strcmp(s->lens, "none")) {
        s->lens_ekit = false;
    } else if (!strcmp(s->lens, "ekit")) {
        s->lens_ekit = true;
    } else {
        error_report("sony-a5000: lens=%s: expected none or ekit", s->lens);
        exit(1);
    }
    qdev_prop_set_bit(dev, "lens-attached", s->lens_ekit);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, A5K_GPIO_BASE);
    for (int pn = 0; pn <= 16; pn++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), pn,
                           pic[GIC_ID(pn < 16 ? 121 + pn : 144)]);
    }
    for (int pn = 0; pn <= 6; pn++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), 17 + pn, pic[GIC_ID(137 + pn)]);
    }
    s->gpio = dev;
    s->submicro_t = timer_new_ns(QEMU_CLOCK_VIRTUAL, a5k_submicro_tick, s);
    /* power-off line p0.10 -> sub-micro (power cycle / power off) */
    if (!s->poff || !strcmp(s->poff, "restart")) {
        s->poff_restart = true;
    } else if (!strcmp(s->poff, "off")) {
        s->poff_restart = false;
    } else {
        error_report("sony-a5000: poff=%s: expected restart or off", s->poff);
        exit(1);
    }
    qemu_log_mask(LOG_UNIMP, "sony-a5000: power-off line: restart decision "
                  "from poff=%s, not from the slot-A TX restart bit (offsets "
                  "unconfirmed, RE_REBOOT.md 4.4)\n",
                  s->poff_restart ? "restart" : "off");
    s->poff_t = timer_new_ns(QEMU_CLOCK_VIRTUAL, a5k_poff_cycle, s);
    qdev_connect_gpio_out_named(dev, "out", 0 * 32 + A5K_POFF_PIN,
                                qemu_allocate_irq(a5k_poff_line, s, 0));
    if (s->lens_ekit) {
        /* lens handshake echo: P5.4 -> P0.5, P2.11 -> P2.10 */
        qemu_irq *hs = qemu_allocate_irqs(a5k_lens_hs_out, s, 2);
        qdev_connect_gpio_out_named(dev, "out", 5 * 32 + 4, hs[0]);
        qdev_connect_gpio_out_named(dev, "out", 2 * 32 + 11, hs[1]);
        s->lens_hs_t = timer_new_ns(QEMU_CLOCK_VIRTUAL, a5k_lens_hs_cb, s);
    }

    /* ADC units 0/1 (resistor-ladder keys, zoom lever), GIC 215/216 */
    for (int u = 0; u < 2; u++) {
        s->adc[u] = sysbus_create_simple("cxd90014-adc",
                                         0xF2406000 + u * 0x1000,
                                         pic[GIC_ID(215 + u)]);
    }

    /* camera buttons from the host keyboard (after GPIO and ADC exist) */
    qemu_input_handler_activate(
        qemu_input_handler_register(NULL, &a5k_key_handler));

    /* SIO ch0..4, GIC 201..205; sub-micro link on ch0 (frames) / ch3 */
    dev = qdev_new("cxd90014-sio");
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    if (s->submicro) {
        gsize flen;
        const char *fn = s->submicro_file ? s->submicro_file :
                         "emulator/data/submicro_frames.bin";
        if (!g_file_get_contents(fn, (gchar **)&s->submicro_frames, &flen,
                                 NULL) || flen < 256) {
            error_report("sony-a5000: submicro frames %s missing", fn);
            exit(1);
        }
        if (sd) {
            a5k_submicro_card_inserted(s->submicro_frames);
        }
        cxd90014_sio_attach(dev, 0, a5k_submicro_xfer, s);
        cxd90014_sio_attach(dev, 3, a5k_submicro_xfer, s);
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0xF2405000);
    for (int c = 0; c < 5; c++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), c, pic[GIC_ID(201 + c)]);
    }

    /* VPH display controller + SYSV frame timer: panel = console 0 */
    dev = qdev_new("cxd90014-vph");
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0xF3440000);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 1, 0xF3402000);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, pic[GIC_ID(98)]);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 1, pic[GIC_ID(101)]);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 2, pic[GIC_ID(102)]);

    /* timing-event selector 0xF3C30000, IRQ 83 (8 ch) / 84 (10 src) */
    DeviceState *vph = dev;
    dev = qdev_new("cxd90014-evsel");
    /* engine frame event (source 29) is driven by the SYSV V clock */
    qdev_prop_set_bit(dev, "ext-vd", true);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    qdev_connect_gpio_out(vph, 0, qdev_get_gpio_in(dev, 0));
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0xF3C30000);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, pic[GIC_ID(83)]);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 1, pic[GIC_ID(84)]);

    /* ---- camera front end (docs/RE_SENSOR_ISP.md §4.a) ---- */
    {
        /* P2S sensor register writer, IRQ 38 */
        dev = qdev_new("cxd90014-p2s");
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0xF3C00000);
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, pic[GIC_ID(38)]);

        /* SIF sync-code counters, clocked by the input unit's frames */
        DeviceState *sif = qdev_new("cxd90014-sif");
        sysbus_realize_and_unref(SYS_BUS_DEVICE(sif), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(sif), 0, 0xF3122000);

        /* AIF channel groups of the camera pipeline: table 0x43B278D4
         * {base, channels}; the CamCore unit groups follow their unit's
         * frame end, the others complete after a delay.  irq1/irq3 = GIC
         * line of channel 1 / 3 (0 = none): ISRs 438CB804.. watch these
         * channels' status nibbles (RC objects, docs/RE_SENSOR_ISP.md §7.1) */
        static const struct { hwaddr base; int nch, irq1, irq3; } aifg[] = {
            { 0xF3002000, 6 }, { 0xF3102000, 6 }, { 0xF3131000, 1 },
            { 0xF3020000, 4 }, { 0xF3041000, 2 },
            { 0xF3881000, 4, 40, 41 }, { 0xF3891000, 4, 42, 43 },
            { 0xF3981000, 4, 44, 45 }, { 0xF30A1000, 2, 46 },
            { 0xF30B1000, 2, 47 }, { 0xF31A1000, 2, 48 },
            { 0xF31B1000, 2, 49 }, { 0xF31C1000, 2, 50 },
            { 0xF3811000, 3 }, { 0xF3911000, 3 },
            { 0xF3800000, 5 }, { 0xF3900000, 5 }, { 0xF31D0000, 3 },
        };
        DeviceState *aif[ARRAY_SIZE(aifg)];
        for (int i = 0; i < ARRAY_SIZE(aifg); i++) {
            aif[i] = qdev_new("cxd90014-aif");
            qdev_prop_set_uint64(aif[i], "base", aifg[i].base);
            qdev_prop_set_uint32(aif[i], "nch", aifg[i].nch);
            qdev_prop_set_bit(aif[i], "frame-sync", i < 2);
            if (aifg[i].base == 0xF3881000) {
                /* RC output ch26 = nibble 3 writes the ch51 live-view
                 * buffer; debug lv-source frames go there only
                 * (docs/RE_LIVEVIEW_IMAGE.md §2.1, §4) */
                qdev_prop_set_int32(aif[i], "lv-ch", 3);
            }
            sysbus_realize_and_unref(SYS_BUS_DEVICE(aif[i]), &error_fatal);
            sysbus_mmio_map(SYS_BUS_DEVICE(aif[i]), 0, aifg[i].base);
            if (aifg[i].irq1) {
                sysbus_connect_irq(SYS_BUS_DEVICE(aif[i]), 1,
                                   pic[GIC_ID(aifg[i].irq1)]);
            }
            if (aifg[i].irq3) {
                sysbus_connect_irq(SYS_BUS_DEVICE(aif[i]), 3,
                                   pic[GIC_ID(aifg[i].irq3)]);
            }
        }

        /* CamCore "Pig" frame/IRQ blocks: unit 0 (IRQ 36), unit 1 (37),
         * input unit 0xF3120000 (35) */
        static const struct { hwaddr base; int irq; } pig[] = {
            { 0xF3000000, 36 }, { 0xF3100000, 37 }, { 0xF3120000, 35 },
        };
        for (int i = 0; i < ARRAY_SIZE(pig); i++) {
            dev = qdev_new("cxd90014-campig");
            qdev_prop_set_uint64(dev, "base", pig[i].base);
            sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
            sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, pig[i].base);
            sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                               pic[GIC_ID(pig[i].irq)]);
            qdev_connect_gpio_out(dev, 0, i < 2 ?
                                  qdev_get_gpio_in(aif[i], 0) :
                                  qdev_get_gpio_in(sif, 0));
        }
    }

    /* ---- SA DSP cores, "null SA" (docs/RE_SA_DSP.md §5) ----
     * Four cores (av-cam accessors 43923E74..43924660), each with source
     * A/B/C interrupts.  A GO completes on source A with factor 0x01; B and
     * C are wired but never raised.  ID strings from the task-44 compare
     * table 4391B334.  These blocks were on the io-f0 register file before;
     * the clock/reset block 0xF3400000 stays there. */
    {
        static const struct {
            hwaddr base; int irq[3]; const char *id;
        } sa[] = {
            { 0xF34A0000, { 51, 52, 53 }, "ZMA0V1.0SA5_V1.0" },
            { 0xF34B0000, { 54, 55, 56 }, "ZMA1V1.0SA5_V1.0" },
            { 0xF34C0000, { 61, 62, 63 }, "SA0_V1.0SA5_V1.0" },
            { 0xF35C0000, { 64, 65, 66 }, "SA1_V1.0SA5_V1.0" },
        };
        for (int i = 0; i < ARRAY_SIZE(sa); i++) {
            dev = qdev_new("cxd90014-sa");
            qdev_prop_set_uint32(dev, "index", i);
            qdev_prop_set_uint64(dev, "base", sa[i].base);
            qdev_prop_set_string(dev, "core-id", sa[i].id);
            sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
            sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, sa[i].base);
            for (int k = 0; k < 3; k++) {
                sysbus_connect_irq(SYS_BUS_DEVICE(dev), k,
                                   pic[GIC_ID(sa[i].irq[k])]);
            }
        }
    }

    /* Debug view of guest frame buffers in a QEMU window (no MMIO; the
     * firmware cannot see it).  -M ...,fb=addr,w,h,stride,fmt[;...] or at
     * run time: qom-set /machine/fbview fb "..." */
    dev = qdev_new("a5k-fbview");
    object_property_add_child(OBJECT(ms), "fbview", OBJECT(dev));
    object_property_set_str(OBJECT(dev), "fb", s->fb ? s->fb :
                            "0x8F758000,640,480,1280,rgb565", &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

    /* av-cam memory DMA, 2 x 8 channels, GIC 173..188 */
    for (int u = 0; u < 2; u++) {
        dev = qdev_new("cxd90014-mdma");
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0xF0102000 + u * 0x1000);
        for (int c = 0; c < 8; c++) {
            sysbus_connect_irq(SYS_BUS_DEVICE(dev), c,
                               pic[GIC_ID(173 + u * 8 + c)]);
        }
    }

    /* DDR controller (IRAM sleep code, docs/RE_REBOOT.md 4.3) */
    sysbus_create_simple("cxd90014-ddrc", 0xF0104000, NULL);

    /* DMP SMAPH-S30 GPU (grm_gles.ko + libObj rm_*), GIC 76 */
    sysbus_create_simple("dmp-smaph-s30", 0xF1000000, pic[GIC_ID(76)]);

    /* BOSS LDEC LZ77 decoder (memory mode, /dev/ldec), GIC 171 */
    dev = qdev_new("cxd90014-ldec");
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0xF2903000);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 1, 0xF0020000);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, pic[GIC_ID(171)]);

    /* BOSS mem-to-mem DMA: two units of four channels, GIC 189..196 */
    for (int u = 0; u < 2; u++) {
        dev = qdev_new("cxd90014-dmac");
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0xF0000000 + u * 0x10000);
        for (int c = 0; c < 4; c++) {
            sysbus_connect_irq(SYS_BUS_DEVICE(dev), c,
                               pic[GIC_ID(189 + u * 4 + c)]);
        }
    }

    /* Everything not yet modelled: a logging register file (reads back
     * the last value written, -d unimp shows every access), underneath the
     * real models at priority -1000. */
    a5k_regfile("io-f0", 0xF0000000, 0x08000000);
    a5k_regfile("io-f8", 0xF8000000, 0x00100000);
    a5k_regfile("io-fa", 0xFA000000, 0x04000000);
    a5k_regfile("io-38", 0x38000000, 0x00100000);
    create_unimplemented_device("a5k.nandc", 0x10000000, 0x10000);

    /* USB device side (docs/RE_USB_DEVICE_CONTROLLER.md): F_USB20HDC UDC
     * 0xF0210000 (GIC 227), its HDMAC 0xF0204000 (GIC 228/229, error only)
     * and the VBUS/ID glue 0xF2920000 (+0x10, GIC 232/233).  The A5KU
     * virtual cable is the device's "chardev" (usb-socket=HOST:PORT, or
     * -global cxd90014-usbhdc.chardev=ID); no client = no cable. */
    dev = qdev_new("cxd90014-usbhdc");
    object_property_add_child(OBJECT(ms), "usbhdc", OBJECT(dev));
    if (s->usb_socket) {
        g_autofree char *spec = g_strdup_printf(
            "tcp:%s,server=on,wait=off", s->usb_socket);
        Chardev *chr = qemu_chr_new("a5k-usb", spec, NULL);
        if (!chr) {
            error_report("usb-socket=%s: cannot create the chardev",
                         s->usb_socket);
            exit(1);
        }
        qdev_prop_set_chr(dev, "chardev", chr);
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0xF0210000);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 1, 0xF0204000);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 2, 0xF2920000);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, pic[GIC_ID(227)]);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 1, pic[GIC_ID(228)]);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 2, pic[GIC_ID(229)]);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 3, pic[GIC_ID(232)]);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 4, pic[GIC_ID(233)]);

    /* CEC (cec.ko, UDIF "cec", PA 0xF3C10000): +0x180 is the interrupt
     * status, write-1-to-clear.  Nothing on the bus raises it, so the bits
     * written there must not read back; otherwise cec.ko sees a phantom
     * RX-ready, reads an RX count of 0, fails the receive and libObj's CEC
     * Msg Thread power-cycles CEC forever (docs/RE_HDMI.md section 4a). */
    a5k_regfile_ex("cec", 0xF3C10000, 0x1000, NULL, "0x180=0xf", -500);

    /* clock generators (PLL lock / self-clearing update request) */
    static const hwaddr clkgen[] = {
        0xF2A01000, 0xF2A02000, 0xF2A11000, 0xF2A12000, 0xF2A21000,
        0xF2A22000, 0xF2901000, 0xF2A31000, 0xF2A32000,
    };
    for (int i = 0; i < ARRAY_SIZE(clkgen); i++) {
        DeviceState *d = qdev_new("cxd90014-regfile");
        qdev_prop_set_string(d, "name", "clkgen");
        qdev_prop_set_uint64(d, "base", clkgen[i]);
        qdev_prop_set_bit(d, "clkgen", true);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(d), &error_fatal);
        sysbus_mmio_map_overlap(SYS_BUS_DEVICE(d), 0, clkgen[i], -500);
    }

    /* NAND subsystem: controller regs (GIC 172) + ARM7 sub-CPU I/O (170) */
    dev = qdev_new("cxd90014-nand");
    if (s->nand_file) {
        qdev_prop_set_string(dev, "file", s->nand_file);
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0x00020000);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 1, 0x00011000);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 2, 0xF2900000);    /* clkrst3 */
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, pic[GIC_ID(172)]);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 1, pic[GIC_ID(170)]);

    /* Provisioning window: a host image (e.g. backup/NAND.BIN, the logical
     * /dev/nflasha) exposed read-only at PA 0x40000000 for the factory-style
     * programming initramfs (emulator/provision).  Absent in normal runs. */
    int64_t prov_len = 0;
    if (s->provision) {
        prov_len = get_image_size(s->provision);
        if (prov_len <= 0 || prov_len > 0x3F000000) {
            error_report("sony-a5000: bad provision image %s", s->provision);
            exit(1);
        }
        /* layout: +0 "A5KP" magic, +4 image length, +0x1000 image */
        memory_region_init_rom(&s->prov_rom, NULL, "a5k.provision",
                               ROUND_UP(prov_len, 4096) + 0x1000,
                               &error_fatal);
        memory_region_add_subregion(sysmem, 0x40000000, &s->prov_rom);
        uint8_t *rp = memory_region_get_ram_ptr(&s->prov_rom);
        memcpy(rp, "A5KP", 4);
        stl_le_p(rp + 4, prov_len);
        if (load_image_size(s->provision, rp + 0x1000,
                            prov_len) != prov_len) {
            error_report("sony-a5000: cannot read %s", s->provision);
            exit(1);
        }
    }

    /* preload=FILE@ADDR[;FILE@ADDR...]: raw images placed in RAM before
     * the kernel starts (bootloader duties, see docs/BOOT.md) */
    if (s->preload) {
        g_auto(GStrv) items = g_strsplit(s->preload, ";", -1);
        for (int i = 0; items[i]; i++) {
            char *at = strrchr(items[i], '@');
            uint64_t addr;
            if (!at || qemu_strtou64(at + 1, NULL, 0, &addr) < 0) {
                error_report("sony-a5000: bad preload item '%s'", items[i]);
                exit(1);
            }
            *at = 0;
            if (load_image_targphys(items[i], addr, 64 * MiB) < 0) {
                error_report("sony-a5000: cannot preload %s", items[i]);
                exit(1);
            }
        }
    }

    /*
     * Hand-off word 0x800210E4 (read by udate_init_clock through Backup
     * 0x00FA00A5; docs/RE_RTC.md sections 2, 5, 7.3).  The factory page in
     * data/handoff_80021000.bin has 1, which makes udate mark the clock
     * UNSET and libObj start the first-boot area/date/time setup on every
     * boot.  A normal power-on with a retained clock presents 0
     * [hypothesis: the bootloader derives it from the micro's "RTC lost"
     * status]; rtc-lost=on keeps 1 (backup battery lost).  The preloaded
     * ROM blob is patched, so every reset presents the same value.
     */
    {
        uint32_t *w = rom_ptr(A5K_HANDOFF_RTC_LOST, 4);
        if (w) {
            *w = cpu_to_le32(s->rtc_lost ? 1 : 0);
        } else if (s->rtc_lost) {
            error_report("sony-a5000: rtc-lost=on needs the hand-off page "
                         "(preload=...@0x80021000)");
            exit(1);
        }
    }

    /* ---- load firmware images ----
     * Kernel, initrd and the kemco command line are (re)written on every
     * reset by a5k_boot_reset() (none of them lies in a ROM blob):
     *  - stage 2: the loader descriptor "A5KB" at PA 0x80027000 is valid ->
     *    the kernel/initrd that stage 1 read from the emulated NAND are
     *    moved into place (what the Sony bootloader does);
     *  - otherwise (power-on, reboot): boot -kernel with the stage-1 loader
     *    initramfs (-M ...,loader=) or the -initrd given. */
    if (!ms->kernel_filename) {
        error_report("sony-a5000: -kernel vmlinux.bin is required");
        exit(1);
    }
    gsize len;
    if (!g_file_get_contents(ms->kernel_filename, (gchar **)&s->kernel,
                             &len, NULL)) {
        error_report("sony-a5000: cannot load kernel %s", ms->kernel_filename);
        exit(1);
    }
    s->kernel_len = len;
    const char *ird = s->loader ? s->loader : ms->initrd_filename;
    if (ird) {
        if (!g_file_get_contents(ird, (gchar **)&s->initrd, &len, NULL) ||
            len > 8 * MiB) {
            error_report("sony-a5000: cannot load initrd %s", ird);
            exit(1);
        }
        s->initrd_len = len;
    }
    g_autofree char *dbg = NULL;
    const char *cmd = a5k_default_cmdline;
    if (ms->kernel_cmdline && *ms->kernel_cmdline) {
        cmd = ms->kernel_cmdline;
    } else if (s->debug_console) {
        g_autoptr(GString) g = g_string_new(a5k_default_cmdline);
        g_string_replace(g, "console=ttynull", "console=ttyAM0 amba2.console=1", 1);
        dbg = g_string_free(g_steal_pointer(&g), false);
        cmd = dbg;
    }
    if (prov_len) {
        s->cmdline = g_strdup_printf("%s a5k.provlen=0x%" PRIx64, cmd, prov_len);
    } else {
        s->cmdline = g_strdup(cmd);
    }
    a5k_write_atags(s->cmdline, s->initrd_len);
    s->init_done.notify = a5k_init_done;
    qemu_add_machine_init_done_notifier(&s->init_done);
    a5k_write_stub();
    a5k_write_smp_stub();
}

static bool a5k_get_rtclost(Object *o, Error **errp)
{
    return A5K_MACHINE(o)->rtc_lost;
}

static void a5k_set_rtclost(Object *o, bool v, Error **errp)
{
    A5K_MACHINE(o)->rtc_lost = v;
}

static char *a5k_get_usbsock(Object *o, Error **errp)
{
    return g_strdup(A5K_MACHINE(o)->usb_socket);
}

static void a5k_set_usbsock(Object *o, const char *v, Error **errp)
{
    g_free(A5K_MACHINE(o)->usb_socket);
    A5K_MACHINE(o)->usb_socket = g_strdup(v);
}

static char *a5k_get_lens(Object *o, Error **errp)
{
    A5KMachineState *s = A5K_MACHINE(o);
    return g_strdup(s->lens ? s->lens : "none");
}

static void a5k_set_lens(Object *o, const char *v, Error **errp)
{
    g_free(A5K_MACHINE(o)->lens);
    A5K_MACHINE(o)->lens = g_strdup(v);
}

static bool a5k_get_dbgcon(Object *o, Error **errp)
{
    return A5K_MACHINE(o)->debug_console;
}

static void a5k_set_dbgcon(Object *o, bool v, Error **errp)
{
    A5K_MACHINE(o)->debug_console = v;
}

static char *a5k_get_smcf(Object *o, Error **errp)
{
    return g_strdup(A5K_MACHINE(o)->submicro_file);
}

static void a5k_set_smcf(Object *o, const char *v, Error **errp)
{
    g_free(A5K_MACHINE(o)->submicro_file);
    A5K_MACHINE(o)->submicro_file = g_strdup(v);
}

/*
 * Debug only: "submicro-poke" = "IDX=VAL" patches one byte of the replayed
 * slot-A frame at run time (qom-set /machine submicro-poke 30=0x08), CRC
 * recomputed.  Used to locate which slot-A fields carry which inputs; never
 * set by default.
 */
static char *a5k_get_smpoke(Object *o, Error **errp)
{
    return g_strdup("");
}

static void a5k_set_smpoke(Object *o, const char *v, Error **errp)
{
    A5KMachineState *s = A5K_MACHINE(o);
    char *e;
    unsigned long idx = strtoul(v, &e, 0);
    if (*e != '=' || idx >= 126 || !s->submicro_frames) {
        error_setg(errp, "submicro-poke: expected IDX=VAL, IDX < 126");
        return;
    }
    s->submicro_frames[idx] = strtoul(e + 1, NULL, 0);
    a5k_submicro_crc(s->submicro_frames);
}

/* Debug only: "gpio-poke" = "PORT.PIN=LEVEL" drives an external GPIO input
 * (qom-set /machine gpio-poke 0.1=1); used to identify unmodelled inputs. */
static char *a5k_get_gpoke(Object *o, Error **errp)
{
    return g_strdup("");
}

static void a5k_set_gpoke(Object *o, const char *v, Error **errp)
{
    A5KMachineState *s = A5K_MACHINE(o);
    unsigned port, pin, level;
    if (sscanf(v, "%u.%u=%u", &port, &pin, &level) != 3 || port >= 18 ||
        pin >= 32 || !s->gpio) {
        error_setg(errp, "gpio-poke: expected PORT.PIN=LEVEL");
        return;
    }
    qemu_set_irq(qdev_get_gpio_in(s->gpio, port * 32 + pin), level);
}

static bool a5k_get_smc(Object *o, Error **errp)
{
    return A5K_MACHINE(o)->submicro;
}

static void a5k_set_smc(Object *o, bool v, Error **errp)
{
    A5K_MACHINE(o)->submicro = v;
}

static char *a5k_get_fb(Object *o, Error **errp)
{
    return g_strdup(A5K_MACHINE(o)->fb);
}

static void a5k_set_fb(Object *o, const char *v, Error **errp)
{
    g_free(A5K_MACHINE(o)->fb);
    A5K_MACHINE(o)->fb = g_strdup(v);
}

static char *a5k_get_loader(Object *o, Error **errp)
{
    return g_strdup(A5K_MACHINE(o)->loader);
}

static void a5k_set_loader(Object *o, const char *v, Error **errp)
{
    g_free(A5K_MACHINE(o)->loader);
    A5K_MACHINE(o)->loader = g_strdup(v);
}

static char *a5k_get_poff(Object *o, Error **errp)
{
    return g_strdup(A5K_MACHINE(o)->poff);
}

static void a5k_set_poff(Object *o, const char *v, Error **errp)
{
    g_free(A5K_MACHINE(o)->poff);
    A5K_MACHINE(o)->poff = g_strdup(v);
}

static char *a5k_get_preload(Object *o, Error **errp)
{
    return g_strdup(A5K_MACHINE(o)->preload);
}

static void a5k_set_preload(Object *o, const char *v, Error **errp)
{
    g_free(A5K_MACHINE(o)->preload);
    A5K_MACHINE(o)->preload = g_strdup(v);
}

static char *a5k_get_nand(Object *o, Error **errp)
{
    return g_strdup(A5K_MACHINE(o)->nand_file);
}

static void a5k_set_nand(Object *o, const char *v, Error **errp)
{
    g_free(A5K_MACHINE(o)->nand_file);
    A5K_MACHINE(o)->nand_file = g_strdup(v);
}

static char *a5k_get_prov(Object *o, Error **errp)
{
    return g_strdup(A5K_MACHINE(o)->provision);
}

static void a5k_set_prov(Object *o, const char *v, Error **errp)
{
    g_free(A5K_MACHINE(o)->provision);
    A5K_MACHINE(o)->provision = g_strdup(v);
}

static void a5k_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    object_class_property_add_bool(oc, "submicro", a5k_get_smc, a5k_set_smc);
    object_class_property_set_description(oc, "submicro",
        "Sub-micro (power micro) HLE replaying the real camera's frames");
    object_class_property_add_str(oc, "submicro-frames", a5k_get_smcf,
                                  a5k_set_smcf);
    object_class_property_add_str(oc, "gpio-poke", a5k_get_gpoke,
                                  a5k_set_gpoke);
    object_class_property_set_description(oc, "gpio-poke",
        "Debug: PORT.PIN=LEVEL drives an external GPIO input");
    object_class_property_add_str(oc, "submicro-poke", a5k_get_smpoke,
                                  a5k_set_smpoke);
    object_class_property_set_description(oc, "submicro-poke",
        "Debug: IDX=VAL patches a byte of the replayed slot-A frame");
    object_class_property_add_str(oc, "fb", a5k_get_fb, a5k_set_fb);
    object_class_property_set_description(oc, "fb",
        "Debug frame-buffer view: addr,w,h,stride,fmt[;...]");
    object_class_property_add_str(oc, "loader", a5k_get_loader,
                                  a5k_set_loader);
    object_class_property_set_description(oc, "loader",
        "Stage-1 loader initramfs (bootloader NAND duties), see docs/BOOT.md");
    object_class_property_add_str(oc, "poff", a5k_get_poff, a5k_set_poff);
    object_class_property_set_description(oc, "poff",
        "restart|off: what the sub-micro does when the power-off line "
        "p0.10 drops (default restart = power cycle)");
    object_class_property_add_str(oc, "preload", a5k_get_preload,
                                  a5k_set_preload);
    object_class_property_set_description(oc, "preload",
        "FILE@ADDR[;FILE@ADDR...] raw images placed in RAM at boot");
    object_class_property_add_str(oc, "nand", a5k_get_nand, a5k_set_nand);
    object_class_property_set_description(oc, "nand",
        "Raw NAND backing file (created erased if missing)");
    object_class_property_add_str(oc, "provision", a5k_get_prov,
                                  a5k_set_prov);
    object_class_property_set_description(oc, "provision",
        "Logical NAND image exposed at PA 0x40000000 for programming");
    object_class_property_add_bool(oc, "debug-console", a5k_get_dbgcon,
                                   a5k_set_dbgcon);
    object_class_property_set_description(oc, "debug-console",
        "Serial console on UART0 instead of the stock console=ttynull");

    object_class_property_add_bool(oc, "rtc-lost", a5k_get_rtclost,
                                   a5k_set_rtclost);
    object_class_property_set_description(oc, "rtc-lost",
        "Clock backup lost: hand-off word 0x800210E4 = 1 (first-boot "
        "area/date/time setup)");
    object_class_property_add_str(oc, "lens", a5k_get_lens, a5k_set_lens);
    object_class_property_set_description(oc, "lens",
        "E-mount lens: none (empty mount, default) or ekit (detect low, "
        "power ack + handshake echo; no lens serial protocol yet)");

    object_class_property_add_str(oc, "usb-socket", a5k_get_usbsock,
                                  a5k_set_usbsock);
    object_class_property_set_description(oc, "usb-socket",
        "HOST:PORT of the A5KU USB cable server (docs/RE_USB_HOST_PATH.md); "
        "a connected client = cable plugged");

    mc->desc = "Sony ILCE-5000 (CXD90014, Cortex-A5 MPCore)";
    mc->init = a5k_init;
    mc->max_cpus = 4;
    mc->default_cpus = 4;   /* quad Cortex-A5: snapshot log "CPU1..3: shutdown" */
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a5");
    mc->default_ram_size = A5K_DDR_SIZE;
    mc->default_ram_id = "a5k.ddr";
    mc->ignore_memory_transaction_failures = false;
}

static const TypeInfo a5k_info = {
    .name = TYPE_A5K_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(A5KMachineState),
    .class_init = a5k_class_init,
};

static void a5k_register(void)
{
    type_register_static(&a5k_info);
}
type_init(a5k_register)

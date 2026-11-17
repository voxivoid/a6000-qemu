/*
 * CXD90014 DDR controller, PA 0xF0104000 (4 KiB), no IRQ used.
 *
 * Only the part that the kernel's IRAM sleep code needs is modelled
 * (vmlinux 0x4037BFA0..0x4037C174, copied to IRAM 0xC0020000 by the sleep
 * entry 0x4037BEA0; literal 0xF0104000 at 0x4037C160; docs/reference/RE_REBOOT.md
 * sections 1.4 and 4.3).  Register names are guesses [?]:
 *
 *   +0x78 (RW)  bits[31:24] = requested low-power state; the IRAM code
 *               writes 0x0A000000 (IRAM +0x78..+0x90)
 *   +0x7C (R)   low byte = state reached; polled for 0x25 (IRAM +0x94)
 *   +0xAC (W)   per-channel command; 0x11008000 then 0x11008002 (ch1:
 *               0x11018000 / 0x11018002).  A write with bit1 set completes
 *               at once and sets +0x128 bit12
 *   +0x128 (R)  status, bit12 = command done (polled, IRAM +0x3C / +0x74)
 *   +0x12C (W)  0x1000 written before each command: clears +0x128 bit12
 *               (write-1-to-clear) [?: consistent with the write order]
 *   +0xC4/+0xD8 per-channel "ddr_ctrl" mode bytes (storage only)
 *
 * DDR contents are not touched here: the loss of DDR on the power cycle
 * that follows is modelled by the board (hw/arm/sony_a6000.c reset kinds).
 * Every other offset reads back the last value written and is logged with
 * LOG_UNIMP, like the io-f0 register file this block used to fall into.
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "trace.h"

#define TYPE_CXD_DDRC "cxd90014-ddrc"
OBJECT_DECLARE_SIMPLE_TYPE(DdrcState, CXD_DDRC)

#define DDRC_SIZE       0x1000
#define DDRC_LPSTATE    0x78
#define DDRC_LPSTAT     0x7C
#define DDRC_CMD        0xAC
#define DDRC_CH0_MODE   0xC4
#define DDRC_CH1_MODE   0xD8
#define DDRC_STATUS     0x128
#define DDRC_STATUS_CLR 0x12C
#define DDRC_ST_DONE    (1u << 12)
#define DDRC_LP_REQ     0x0A        /* +0x78[31:24] written by the IRAM code */
#define DDRC_LP_DONE    0x25        /* +0x7C low byte the IRAM code waits on */

struct DdrcState {
    SysBusDevice parent_obj;
    MemoryRegion mr;
    uint32_t r[DDRC_SIZE / 4];
};

#define R(o) (s->r[(o) / 4])

static bool ddrc_known(hwaddr o)
{
    switch (o) {
    case DDRC_LPSTATE: case DDRC_LPSTAT: case DDRC_CMD:
    case DDRC_CH0_MODE: case DDRC_CH1_MODE:
    case DDRC_STATUS: case DDRC_STATUS_CLR:
        return true;
    }
    return false;
}

static uint64_t ddrc_read(void *opaque, hwaddr o, unsigned size)
{
    DdrcState *s = opaque;
    uint32_t v = R(o);

    if (o == DDRC_LPSTAT) {
        /*
         * Low-power state reached = the one requested in +0x78[31:24];
         * the requested state 0x0A (self-refresh / power-down entry [?])
         * reads back as 0x25.  Other states are reported unchanged
         * [hypothesis: the code only proves the 0x0A -> 0x25 pair].
         */
        uint32_t req = R(DDRC_LPSTATE) >> 24;
        v = (v & ~0xffu) | (req == DDRC_LP_REQ ? DDRC_LP_DONE : req);
    }
    trace_cxd_ddrc_rd(o, v);
    if (!ddrc_known(o)) {
        qemu_log_mask(LOG_UNIMP, "cxd90014-ddrc: rd +0x%03" HWADDR_PRIx
                      " -> 0x%x\n", o, v);
    }
    return v;
}

static void ddrc_write(void *opaque, hwaddr o, uint64_t v, unsigned size)
{
    DdrcState *s = opaque;

    trace_cxd_ddrc_wr(o, v);
    switch (o) {
    case DDRC_STATUS_CLR:
        R(DDRC_STATUS) &= ~(v & DDRC_ST_DONE);
        R(o) = v;
        return;
    case DDRC_CMD:
        R(o) = v;
        if (v & 2) {
            /* command issued: completes immediately */
            R(DDRC_STATUS) |= DDRC_ST_DONE;
        }
        return;
    case DDRC_STATUS:
        return;                     /* read-only status */
    }
    if (!ddrc_known(o)) {
        qemu_log_mask(LOG_UNIMP, "cxd90014-ddrc: wr +0x%03" HWADDR_PRIx
                      " = 0x%" PRIx64 "\n", o, v);
    }
    R(o) = v;
}

static const MemoryRegionOps ddrc_ops = {
    .read = ddrc_read, .write = ddrc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void ddrc_reset(DeviceState *dev)
{
    DdrcState *s = CXD_DDRC(dev);
    memset(s->r, 0, sizeof(s->r));
}

static void ddrc_init(Object *obj)
{
    DdrcState *s = CXD_DDRC(obj);
    memory_region_init_io(&s->mr, obj, &ddrc_ops, s, "cxd-ddrc", DDRC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mr);
}

static void ddrc_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), ddrc_reset);
}

static const TypeInfo ddrc_info = {
    .name = TYPE_CXD_DDRC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DdrcState),
    .instance_init = ddrc_init,
    .class_init = ddrc_class_init,
};

static void ddrc_register(void)
{
    type_register_static(&ddrc_info);
}
type_init(ddrc_register)

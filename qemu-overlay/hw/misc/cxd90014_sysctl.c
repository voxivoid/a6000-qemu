/*
 * Sony CXD90014 system controller block at 0xF2915000 (a5000).
 *
 * Recovered from the 1.10 kernel:
 *   +0x00  chip id (cxd90014_read_cpuid, printed as "CPUID:").  Real value
 *          0x00000500, recovered from the cached copy in the WBI RAM
 *          snapshot (kernel VA 0x403798c0).
 *   +0x44  reset/request SET register   (cxd90014_reset: 0x01 soft, 0x11
 *          hard; cxd90014_xrstreq_init: 0x10)
 *   +0x48  same bits, CLEAR register
 *   +0x50  reset-cause / wdt status (read by wdt_intr)
 * Other offsets behave as plain read/write storage and are logged under
 * -d unimp so new accesses get noticed.
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "system/runstate.h"

#define TYPE_CXD90014_SYSCTL "cxd90014-sysctl"
OBJECT_DECLARE_SIMPLE_TYPE(CXDSysctlState, CXD90014_SYSCTL)

#define NREGS (0x1000 / 4)

struct CXDSysctlState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[NREGS];
    uint32_t chip_id;
    uint32_t rstreq;
    bool sd_present;
};

static uint64_t sc_read(void *opaque, hwaddr off, unsigned size)
{
    CXDSysctlState *s = opaque;
    switch (off) {
    case 0x00:
        return s->chip_id;
    case 0x44:
    case 0x48:
        return s->rstreq;
    default:
        qemu_log_mask(LOG_UNIMP, "cxd90014-sysctl: read 0x%03" HWADDR_PRIx
                      " -> 0x%08x\n", off, s->regs[off / 4]);
        return s->regs[off / 4];
    }
}

static void sc_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    CXDSysctlState *s = opaque;
    switch (off) {
    case 0x44:
        s->rstreq |= v;
        /* cxd90014_reset(1) writes 0x11 then spins forever: that is the
         * real SoC reset.  cxd90014_reset(0) writes 0x01 alone and returns,
         * so bit0 by itself is not a reset. */
        if ((v & 0x11) == 0x11) {
            qemu_log_mask(LOG_GUEST_ERROR, "cxd90014-sysctl: reset request "
                          "0x%02" PRIx64 "\n", v);
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        }
        break;
    case 0x48:
        s->rstreq &= ~v;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "cxd90014-sysctl: write 0x%03" HWADDR_PRIx
                      " = 0x%08" PRIx64 "\n", off, v);
        s->regs[off / 4] = v;
    }
}

static const MemoryRegionOps sc_ops = {
    .read = sc_read,
    .write = sc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void sc_reset(DeviceState *dev)
{
    CXDSysctlState *s = CXD90014_SYSCTL(dev);
    memset(s->regs, 0, sizeof(s->regs));
    s->rstreq = 0;
    /* +0x10 media detect (active low): bit16 MS in, bit24 SD in, bit25 SD
     * WP (mcmn_drv getmmccddata/getmmcwpdata/getmsindata; values per
     * docs/RE_STORAGE_USB.md: 0x0C010000 = SD in + writable, 0x0D010000 =
     * empty slot) */
    s->regs[0x10 / 4] = s->sd_present ? 0x0C010000 : 0x0D010000;
}

static void sc_init(Object *obj)
{
    CXDSysctlState *s = CXD90014_SYSCTL(obj);
    memory_region_init_io(&s->iomem, obj, &sc_ops, s, "cxd90014-sysctl",
                          0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const Property sc_props[] = {
    DEFINE_PROP_UINT32("chip-id", CXDSysctlState, chip_id, 0x00000500),
    DEFINE_PROP_BOOL("sd-present", CXDSysctlState, sd_present, false),
};

static void sc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_legacy_reset(dc, sc_reset);
    device_class_set_props(dc, sc_props);
}

static const TypeInfo sc_info = {
    .name = TYPE_CXD90014_SYSCTL,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CXDSysctlState),
    .instance_init = sc_init,
    .class_init = sc_class_init,
};

static void sc_register(void)
{
    type_register_static(&sc_info);
}
type_init(sc_register)

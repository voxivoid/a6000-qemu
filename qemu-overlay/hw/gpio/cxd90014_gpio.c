/*
 * CXD90014 GPIO, 18 ports (gpio_p0..p17) at PA 0xF2910000 + 0x100*n.
 * Register triples (value / set / clear), from the kernel hw_gpio_* code
 * (docs/RE_INPUT_I2C_POWER.md):
 *   +0x00/04/08 direction (1 = output [?])   +0x10 pin input data
 *   +0x20/24/28 input enable                  +0x30/34/38 mode/pull
 *   +0x40/44/48 output latch
 * Input data = output latch on output pins, external level on inputs.
 * External levels are set through qdev GPIO inputs (32 per port, port-major)
 * by board-level models (keys, sub-micro handshake).  Reset levels: P5.2,
 * P10.10 and the shutter lines P0.1/P0.11/P0.13/P1.5 high (active-low keys
 * released).
 * Pin interrupts (gpiopin_to_irq): p0.0..15 -> GIC 121+pin, p0.16 -> 144,
 * p1.0..6 -> 137+pin; pulsed on every edge of the pin.
 */
#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "trace.h"

#define TYPE_CXD_GPIO "cxd90014-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(GpioState, CXD_GPIO)

#define NPORT 18
#define NIRQ (17 + 7)

struct GpioState {
    SysBusDevice parent_obj;
    MemoryRegion mr;
    uint32_t dir[NPORT], ien[NPORT], mode[NPORT], out[NPORT], ext[NPORT];
    uint32_t misc[NPORT][0x100 / 4];
    qemu_irq irq[NIRQ];
    QEMUTimer *drop[NIRQ];
};

static void drop_cb(void *opaque);
typedef struct { struct GpioState *s; int n; } DropArg;
static DropArg drop_args[NIRQ];

static uint32_t pin_in(GpioState *s, int p)
{
    return (s->out[p] & s->dir[p]) | (s->ext[p] & ~s->dir[p]);
}

static uint32_t *triple(GpioState *s, int p, hwaddr o)
{
    switch (o & 0xf0) {
    case 0x00: return &s->dir[p];
    case 0x20: return &s->ien[p];
    case 0x30: return &s->mode[p];
    case 0x40: return &s->out[p];
    }
    return NULL;
}

static void pulse(GpioState *s, int port, int pin)
{
    int n = -1;
    if (port == 0 && pin <= 16) {
        n = pin;                    /* irq[0..16] */
    } else if (port == 1 && pin <= 6) {
        n = 17 + pin;               /* irq[17..23] */
    }
    if (n >= 0) {
        /* hold the line 1 ms so a level-configured GIC input sees it */
        qemu_set_irq(s->irq[n], 1);
        timer_mod(s->drop[n], qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SCALE_MS);
    }
}

static void edges(GpioState *s, int p, uint32_t before)
{
    uint32_t ch = before ^ pin_in(s, p);
    for (int b = 0; ch; b++, ch >>= 1) {
        if (ch & 1) {
            pulse(s, p, b);
        }
    }
}

static uint64_t gpio_read(void *opaque, hwaddr off, unsigned size)
{
    GpioState *s = opaque;
    int p = off / 0x100;
    hwaddr o = off % 0x100;
    if (o == 0x10) {
        trace_cxd90014_gpio_in(p, pin_in(s, p));
        return pin_in(s, p);
    }
    uint32_t *t = triple(s, p, o);
    if (t && (o & 0xf) == 0) {
        return *t;
    }
    return s->misc[p][o / 4];
}

static void gpio_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    GpioState *s = opaque;
    int p = off / 0x100;
    hwaddr o = off % 0x100;
    uint32_t *t = triple(s, p, o);
    uint32_t before = pin_in(s, p);
    if (t) {
        switch (o & 0xf) {
        case 0x0: *t = v; break;
        case 0x4: *t |= v; break;
        case 0x8: *t &= ~v; break;
        default: s->misc[p][o / 4] = v; return;
        }
        edges(s, p, before);
        return;
    }
    s->misc[p][o / 4] = v;
}

static void drop_cb(void *opaque)
{
    DropArg *a = opaque;
    qemu_set_irq(a->s->irq[a->n], 0);
}

static void gpio_set_ext(void *opaque, int line, int level)
{
    GpioState *s = opaque;
    int p = line / 32, b = line % 32;
    uint32_t before = pin_in(s, p);
    s->ext[p] = (s->ext[p] & ~(1u << b)) | ((uint32_t)!!level << b);
    edges(s, p, before);
}

static const MemoryRegionOps gpio_ops = {
    .read = gpio_read, .write = gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void gpio_reset(DeviceState *dev)
{
    GpioState *s = CXD_GPIO(dev);
    memset(s->dir, 0, sizeof(s->dir)); memset(s->ien, 0, sizeof(s->ien));
    memset(s->mode, 0, sizeof(s->mode)); memset(s->out, 0, sizeof(s->out));
    memset(s->misc, 0, sizeof(s->misc));
    memset(s->ext, 0, sizeof(s->ext));
    s->ext[5] |= 1u << 2;           /* key (active low) released */
    s->ext[10] |= 1u << 10;         /* key (active low) released */
    /*
     * Shutter release, active low (av-cam 436FC740 reads the level and
     * inverts it): S1 (half press, key 0x1F1) on p0.1 with second interrupt
     * line p0.11, S2 (full press, key 0xA7) on p1.5 with p0.13 (IRQ set
     * chosen in 436FCA80: 122/142/132/134).  Idle = high; with these low the
     * camera booted with the shutter held fully down.
     */
    s->ext[0] |= (1u << 1) | (1u << 11) | (1u << 13);
    s->ext[1] |= 1u << 5;
}

static void gpio_init(Object *obj)
{
    GpioState *s = CXD_GPIO(obj);
    memory_region_init_io(&s->mr, obj, &gpio_ops, s, "cxd-gpio", 0x100 * NPORT);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mr);
    for (int i = 0; i < NIRQ; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[i]);
        drop_args[i].s = s;
        drop_args[i].n = i;
        s->drop[i] = timer_new_ns(QEMU_CLOCK_VIRTUAL, drop_cb, &drop_args[i]);
    }
    qdev_init_gpio_in(DEVICE(obj), gpio_set_ext, NPORT * 32);
}

static void gpio_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), gpio_reset);
}

static const TypeInfo gpio_info = {
    .name = TYPE_CXD_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GpioState),
    .instance_init = gpio_init,
    .class_init = gpio_class_init,
};

static void gpio_register(void)
{
    type_register_static(&gpio_info);
}
type_init(gpio_register)

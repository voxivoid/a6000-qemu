/*
 * CXD90014 16-channel memory DMA used by av-cam.bin.
 * Units at PA 0xF0102000 (ch0-7) and 0xF0103000 (ch8-15), GIC 173 + ch.
 * Recovered from av-cam: entry_pre copies its initialised data section
 * with it (ch6 regs +0x1C0..+0x1D0, 25 x 16 KiB), ISR 0x439172b0,
 * status 0x43917994, stop 0x43917bb0 (docs/reference/RE_AVCAM_IRQ_BOOT.md):
 *   unit +0x00  per-channel done/IRQ status (bit ch)     +0x08 W clear
 *        +0x0C  per-channel error status                 +0x10 W clear
 *        +0x1C  busy bits (always 0: transfers finish at once)
 *   ch k +0x100 + 0x20*k: +0x00 src PA, +0x04 dst PA, +0x0C cfg (bits
 *        [11:0] = 32-bit word count, 0 = 4096), +0x10 ctl bit0 = start
 * Without this model the engine ran with an all-zero .data section.
 *
 * Peripheral-paced channels (docs/reference/RE_LENS_EMOUNT.md 9.4, lens on UART1):
 * when src or dst equals the FIFO address registered by the board
 * (cxd90014_mdma_set_periph, 0xF2001000 = UART1 DR) the channel does not
 * copy memory.  The driver packs (av-cam 439174D2) cfg bits[11:0] = count
 * in source units, [14:12] = dst width, [17:15] = src width, bit31 = done
 * IRQ enable (43917A4C); ctl +0x10 bit0 = run (start 43917BB0(ch, 1), stop
 * 43917BB0(ch, 0); 43917BF4 reports busy while bit0 = 1).
 *   TX (dst = FIFO): the count * unit bytes go to the peer; done (status
 *       bit, GIC 173 + ch) after the serial time of those bytes.
 *   RX (src = FIFO): stays armed (bit0 = 1).  Bytes the peer produces are
 *       written at +0x04, which advances (43993840 measures progress as
 *       +0x04 - ring start); done when the count is used up.  A read of
 *       +0x04 first lets the peer deliver bytes that are due, so the
 *       guest never loses bytes between that read and its stop.
 * Width code -> bytes: 1 -> 1, 2 -> 2, 3 -> 4 [hypothesis: lens ring
 * 0x200 units of width 1 = 512 bytes, TX 12 units of width 3 >= 41-byte
 * frame].
 */
#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "system/address-spaces.h"
#include "qemu/timer.h"
#include "hw/dma/cxd90014_mdma.h"
#include "trace.h"

#define TYPE_CXD_MDMA "cxd90014-mdma"
OBJECT_DECLARE_SIMPLE_TYPE(MdmaState, CXD_MDMA)

struct MdmaState {
    SysBusDevice parent_obj;
    MemoryRegion mr;
    uint32_t reg[0x1000 / 4];
    uint32_t status, err;
    qemu_irq irq[8];
    /* peripheral-paced mode */
    uint32_t pfifo;
    uint32_t ns_per_byte;
    CxdMdmaPeriphTx ptx;
    CxdMdmaPeriphPull ppull;
    void *popaque;
    uint32_t rx_left[8];
    QEMUTimer *tx_t[8];
};

static void mdma_update(MdmaState *s)
{
    for (int k = 0; k < 8; k++) {
        qemu_set_irq(s->irq[k], (s->status >> k) & 1);
    }
}

static uint32_t *mdma_ch(MdmaState *s, int k)
{
    return &s->reg[(0x100 + 0x20 * k) / 4];
}

static void mdma_done(MdmaState *s, int k)
{
    mdma_ch(s, k)[4] &= ~1u;
    s->status |= 1u << k;
    mdma_update(s);
}

static void mdma_tx_done(void *opaque)
{
    MdmaState *s = opaque;
    for (int k = 0; k < 8; k++) {
        if (s->tx_t[k] && !timer_pending(s->tx_t[k]) &&
            (mdma_ch(s, k)[4] & 1) && mdma_ch(s, k)[1] == s->pfifo) {
            trace_cxd_mdma_periph_done(k,
                                       qemu_clock_get_us(QEMU_CLOCK_VIRTUAL));
            mdma_done(s, k);
        }
    }
}

/* count in source units, width code from cfg [17:15] (hypothesis above) */
static size_t mdma_periph_len(uint32_t cfg)
{
    static const uint8_t unit[8] = { 1, 1, 2, 4, 4, 4, 4, 4 };
    uint32_t n = cfg & 0xfff;
    return (size_t)(n ? n : 4096) * unit[(cfg >> 15) & 7];
}

static void mdma_run_periph(MdmaState *s, int k)
{
    uint32_t *c = mdma_ch(s, k);
    size_t len = mdma_periph_len(c[3]);

    if (c[1] == s->pfifo) {
        g_autofree uint8_t *buf = g_malloc(len);
        address_space_read(&address_space_memory, c[0],
                           MEMTXATTRS_UNSPECIFIED, buf, len);
        trace_cxd_mdma_periph_tx(k, c[0], len);
        if (s->ptx) {
            s->ptx(s->popaque, buf, len);
        }
        timer_mod(s->tx_t[k], qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (int64_t)len * s->ns_per_byte);
    } else {
        s->rx_left[k] = len;
        trace_cxd_mdma_periph_rx_arm(k, c[1], len);
    }
}

size_t cxd90014_mdma_periph_rx(DeviceState *dev, const uint8_t *buf,
                               size_t len)
{
    MdmaState *s = CXD_MDMA(dev);

    for (int k = 0; k < 8; k++) {
        uint32_t *c = mdma_ch(s, k);
        size_t n;

        if (!(c[4] & 1) || c[0] != s->pfifo || c[1] == s->pfifo) {
            continue;
        }
        n = MIN(len, s->rx_left[k]);
        address_space_write(&address_space_memory, c[1],
                            MEMTXATTRS_UNSPECIFIED, buf, n);
        trace_cxd_mdma_periph_rx(k, c[1], n);
        c[1] += n;
        s->rx_left[k] -= n;
        if (!s->rx_left[k]) {
            mdma_done(s, k);
        }
        return n;
    }
    return 0;
}

void cxd90014_mdma_set_periph(DeviceState *dev, uint32_t fifo,
                              uint32_t ns_per_byte,
                              CxdMdmaPeriphTx tx, CxdMdmaPeriphPull pull,
                              void *opaque)
{
    MdmaState *s = CXD_MDMA(dev);
    s->pfifo = fifo;
    s->ns_per_byte = ns_per_byte;
    s->ptx = tx;
    s->ppull = pull;
    s->popaque = opaque;
    for (int k = 0; k < 8; k++) {
        if (!s->tx_t[k]) {
            s->tx_t[k] = timer_new_ns(QEMU_CLOCK_VIRTUAL, mdma_tx_done, s);
        }
    }
}

static void mdma_run(MdmaState *s, int k)
{
    uint32_t *c = mdma_ch(s, k);
    uint32_t words = c[3] & 0xfff;
    size_t len = (size_t)(words ? words : 4096) * 4;
    g_autofree uint8_t *buf = g_malloc(len);
    address_space_read(&address_space_memory, c[0], MEMTXATTRS_UNSPECIFIED,
                       buf, len);
    address_space_write(&address_space_memory, c[1], MEMTXATTRS_UNSPECIFIED,
                        buf, len);
    trace_cxd_mdma_copy(k, c[0], c[1], len);
    c[4] &= ~1u;
    s->status |= 1u << k;
    mdma_update(s);
}

static uint64_t mdma_read(void *opaque, hwaddr off, unsigned size)
{
    MdmaState *s = opaque;
    switch (off) {
    case 0x00: return s->status;
    case 0x0C: return s->err;
    case 0x1C: return 0;
    }
    if (s->ppull && off >= 0x100 && off < 0x200 && (off & 0x1f) == 0x04) {
        uint32_t *c = mdma_ch(s, (off - 0x100) / 0x20);
        if ((c[4] & 1) && c[0] == s->pfifo && c[1] != s->pfifo) {
            s->ppull(s->popaque);       /* RX progress read: take bytes */
        }
    }
    return s->reg[off / 4];
}

static void mdma_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    MdmaState *s = opaque;
    switch (off) {
    case 0x08:
        s->status &= ~v;
        mdma_update(s);
        return;
    case 0x10:
        s->err &= ~v;
        return;
    }
    uint32_t old = s->reg[off / 4];
    s->reg[off / 4] = v;
    if (off >= 0x100 && off < 0x200 && (off & 0x1f) == 0x10) {
        int k = (off - 0x100) / 0x20;
        uint32_t *c = mdma_ch(s, k);

        if (s->pfifo && (c[0] == s->pfifo || c[1] == s->pfifo)) {
            /* the driver read-modify-writes +0x10: act on bit0 edges */
            if (!((old ^ v) & 1)) {
                return;
            }
            if (!(v & 1)) {
                if (s->tx_t[k]) {
                    timer_del(s->tx_t[k]);      /* stop (43917BB0(ch, 0)) */
                }
            } else {
                mdma_run_periph(s, k);
            }
        } else if (v & 1) {
            mdma_run(s, k);
        }
    }
}

static const MemoryRegionOps mdma_ops = {
    .read = mdma_read, .write = mdma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void mdma_reset(DeviceState *dev)
{
    MdmaState *s = CXD_MDMA(dev);
    memset(s->reg, 0, sizeof(s->reg));
    s->status = s->err = 0;
    memset(s->rx_left, 0, sizeof(s->rx_left));
    for (int k = 0; k < 8; k++) {
        if (s->tx_t[k]) {
            timer_del(s->tx_t[k]);
        }
    }
}

static void mdma_init(Object *obj)
{
    MdmaState *s = CXD_MDMA(obj);
    memory_region_init_io(&s->mr, obj, &mdma_ops, s, "cxd-mdma", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mr);
    for (int i = 0; i < 8; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[i]);
    }
}

static void mdma_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), mdma_reset);
}

static const TypeInfo mdma_info = {
    .name = TYPE_CXD_MDMA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MdmaState),
    .instance_init = mdma_init,
    .class_init = mdma_class_init,
};

static void mdma_register(void)
{
    type_register_static(&mdma_info);
}
type_init(mdma_register)

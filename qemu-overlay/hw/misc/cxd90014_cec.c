/*
 * CXD90014 HDMI-CEC controller, PA 0xF3C10000 (UDIF "cec", 0x190
 * bytes used, 4 KiB window), no IRQ: cec.ko polls it through hdmi.ko's
 * executor.  docs/reference/RE_HDMI.md §1.6 (cec.ko cell tables, cec_core_*).
 *
 *   +0x000..0x03C  TX frame, one byte per word (byte 0 = initiator<<4 | dest)
 *                  (cec_core_write_tx_frame @0x7cdc)
 *   +0x040  [4:0]  TX length
 *   +0x044  bit0   TX start (cleared here when the frame completes)
 *   +0x048/0x04C/0x0A4  bit timing / prescaler [?]           RAM
 *   +0x054..0x090  RX frame (cec_core_read_rx_frame @0x7d7c)
 *   +0x094  [4:0]  RX byte count (1..16; 0 is rejected by the driver)
 *   +0x098  bit0   RX buffer release / enable [?]
 *   +0x09C  bit0   ignore while LA = 15 [?]                   RAM
 *   +0x0A0  [3:0]  own logical address (cec_core_set_logical_addr @0x7e48)
 *   +0x0AC/0x0B0   enable / soft reset [?]                    RAM
 *   +0x180  status, write 1 to clear: bit0 RX ready, bit1 TX done - no ACK,
 *           bit2 TX retry / arbitration error, bit3 TX done - ACK
 *           (cec_core_chk_rx_ready @0x7c1c, chk_tx_ready @0x7828,
 *           chk_tx_int_polling @0x7b14)
 *   +0x184  interrupt enable (0xF on / 0 off)
 *
 * Virtual bus: the camera is alone unless "connected" (HDMI cable) is set;
 * then an optional virtual TV ("tv", logical address 0, physical address
 * 0.0.0.0) acknowledges frames sent to it and answers a few requests.
 * Every directed frame to another address (including the polling frame to
 * the camera's own candidate address) completes with "no ACK", so the
 * camera adopts the address.  Broadcast frames complete with ACK when the
 * cable is connected [? no-cable broadcast reported as no ACK, doc §4.2].
 * Bit timing: 4.5 ms start bit + 24 ms per byte (CEC nominal).
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "trace.h"

#define TYPE_CXD_CEC "cxd90014-cec"
OBJECT_DECLARE_SIMPLE_TYPE(CxdCecState, CXD_CEC)

#define CEC_SIZE    0x1000
#define CEC_USED    0x190
#define TX_BUF      0x000
#define TX_LEN      0x040
#define TX_START    0x044
#define RX_BUF      0x054
#define RX_CNT      0x094
#define RX_REL      0x098
#define OWN_LA      0x0A0
#define STATUS      0x180
#define INT_EN      0x184

#define ST_RX       (1u << 0)
#define ST_NOACK    (1u << 1)
#define ST_ACK      (1u << 3)

#define TV_LA       0
#define RXQ         4

typedef struct CecFrame {
    uint8_t len;
    uint8_t b[16];
} CecFrame;

struct CxdCecState {
    SysBusDevice parent_obj;
    MemoryRegion mr;
    uint32_t regs[CEC_SIZE / 4];
    QEMUTimer *tx_timer, *rx_timer;
    uint32_t tx_result;
    bool connected;
    bool tv;
    CecFrame rxq[RXQ];
    unsigned rxq_n;
};

#define R(o) (s->regs[(o) / 4])

static void cec_rx_deliver(CxdCecState *s)
{
    if (!s->rxq_n || (R(STATUS) & ST_RX) || !(R(INT_EN) & ST_RX)) {
        return;
    }
    CecFrame *f = &s->rxq[0];
    for (unsigned i = 0; i < 16; i++) {
        R(RX_BUF + 4 * i) = i < f->len ? f->b[i] : 0;
    }
    R(RX_CNT) = f->len;
    R(STATUS) |= ST_RX;
    trace_cxd90014_cec_rx(f->len, f->b[0], f->len > 1 ? f->b[1] : -1);
    memmove(&s->rxq[0], &s->rxq[1], (RXQ - 1) * sizeof(CecFrame));
    s->rxq_n--;
}

static void cec_rx_timer(void *opaque)
{
    CxdCecState *s = opaque;
    cec_rx_deliver(s);
    if (s->rxq_n) {
        timer_mod(s->rx_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  20 * SCALE_MS);
    }
}

static void cec_tv_send(CxdCecState *s, uint8_t dest, const uint8_t *op,
                        unsigned n)
{
    if (s->rxq_n == RXQ || n > 15) {
        return;
    }
    CecFrame *f = &s->rxq[s->rxq_n++];
    f->len = n + 1;
    f->b[0] = TV_LA << 4 | (dest & 15);
    memcpy(f->b + 1, op, n);
    /* the reply follows the request on the bus (~ start + bytes) */
    timer_mod(s->rx_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              (5 + 24 * f->len) * SCALE_MS);
}

/* The virtual TV's answers to the requests a playback device makes. */
static void cec_tv_receive(CxdCecState *s, const uint8_t *b, unsigned len)
{
    uint8_t from = b[0] >> 4;
    if (len < 2 || from == 15) {
        return;
    }
    switch (b[1]) {
    case 0x83: {                    /* Give Physical Address */
        static const uint8_t r[] = { 0x84, 0x00, 0x00, 0x00 };  /* TV */
        cec_tv_send(s, 15, r, sizeof(r));
        break;
    }
    case 0x8F: {                    /* Give Device Power Status */
        static const uint8_t r[] = { 0x90, 0x00 };              /* on */
        cec_tv_send(s, from, r, sizeof(r));
        break;
    }
    case 0x9F: {                    /* Get CEC Version */
        static const uint8_t r[] = { 0x9E, 0x04 };              /* 1.3a */
        cec_tv_send(s, from, r, sizeof(r));
        break;
    }
    case 0x46: {                    /* Give OSD Name */
        static const uint8_t r[] = { 0x47, 'Q', 'E', 'M', 'U', ' ', 'T',
                                     'V' };
        cec_tv_send(s, from, r, sizeof(r));
        break;
    }
    case 0x8C: {                    /* Give Device Vendor ID */
        static const uint8_t r[] = { 0x87, 0x00, 0x00, 0x00 };
        cec_tv_send(s, 15, r, sizeof(r));
        break;
    }
    case 0x91: {                    /* Get Menu Language */
        static const uint8_t r[] = { 0x32, 'e', 'n', 'g' };
        cec_tv_send(s, 15, r, sizeof(r));
        break;
    }
    default:
        break;                      /* everything else: acknowledged only */
    }
}

static void cec_tx_timer(void *opaque)
{
    CxdCecState *s = opaque;
    R(TX_START) &= ~1u;
    R(STATUS) |= s->tx_result;
    trace_cxd90014_cec_tx_done(s->tx_result);
}

static void cec_tx_start(CxdCecState *s)
{
    unsigned len = R(TX_LEN) & 0x1f;
    uint8_t b[16];

    if (len > 16) {
        len = 16;
    }
    for (unsigned i = 0; i < 16; i++) {
        b[i] = R(TX_BUF + 4 * i);
    }
    uint8_t dest = b[0] & 15;
    bool tv = s->connected && s->tv;

    if (!len) {
        qemu_log_mask(LOG_GUEST_ERROR, "cxd90014-cec: TX with length 0\n");
        len = 1;
    }
    if (!s->connected) {
        s->tx_result = ST_NOACK;
    } else if (dest == 15) {
        s->tx_result = ST_ACK;
    } else {
        s->tx_result = (tv && dest == TV_LA) ? ST_ACK : ST_NOACK;
    }
    trace_cxd90014_cec_tx(len, b[0], len > 1 ? b[1] : -1, s->tx_result);
    if (tv && (dest == TV_LA || dest == 15)) {
        cec_tv_receive(s, b, len);
    }
    timer_mod(s->tx_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              (4500 + 24000 * len) * SCALE_US);
}

static uint64_t cec_read(void *opaque, hwaddr off, unsigned size)
{
    CxdCecState *s = opaque;
    uint32_t w = R(off & ~3);
    if (off >= CEC_USED) {
        qemu_log_mask(LOG_UNIMP, "cxd90014-cec: rd 0x%03" HWADDR_PRIx "\n",
                      off);
    }
    return (w >> ((off & 3) * 8)) & (size == 4 ? 0xffffffffu :
                                     (1u << (size * 8)) - 1);
}

static void cec_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    CxdCecState *s = opaque;
    hwaddr w = off & ~3;
    unsigned sh = (off & 3) * 8;
    uint32_t mask = size == 4 ? 0xffffffffu : ((1u << (size * 8)) - 1) << sh;
    uint32_t v = ((uint32_t)val << sh) & mask;

    if (off >= CEC_USED) {
        qemu_log_mask(LOG_UNIMP, "cxd90014-cec: wr 0x%03" HWADDR_PRIx
                      " = 0x%" PRIx64 "\n", off, val);
    }
    switch (w) {
    case STATUS:
        R(w) &= ~(v & 0xf);         /* write 1 to clear */
        if (v & ST_RX) {
            cec_rx_deliver(s);
        }
        return;
    case RX_CNT:
        return;                     /* read-only */
    case TX_START: {
        bool was = R(w) & 1;
        R(w) = (R(w) & ~mask) | v;
        if (!(R(w) & 1)) {
            timer_del(s->tx_timer);     /* power on / off: cancel */
        } else if (!was) {
            cec_tx_start(s);
        }
        return;
    }
    case INT_EN:
        R(w) = (R(w) & ~mask) | v;
        if (!(R(w) & ST_RX)) {
            s->rxq_n = 0;           /* CEC off: nothing is received */
        }
        cec_rx_deliver(s);
        return;
    default:
        R(w) = (R(w) & ~mask) | v;
        return;
    }
}

static const MemoryRegionOps cec_ops = {
    .read = cec_read, .write = cec_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static bool cec_get_connected(Object *obj, Error **errp)
{
    return CXD_CEC(obj)->connected;
}

static void cec_set_connected(Object *obj, bool v, Error **errp)
{
    CxdCecState *s = CXD_CEC(obj);
    s->connected = v;
    if (!v) {
        s->rxq_n = 0;
    }
}

static void cec_reset(DeviceState *dev)
{
    CxdCecState *s = CXD_CEC(dev);
    memset(s->regs, 0, sizeof(s->regs));
    timer_del(s->tx_timer);
    timer_del(s->rx_timer);
    s->rxq_n = 0;
}

static void cec_init(Object *obj)
{
    CxdCecState *s = CXD_CEC(obj);
    memory_region_init_io(&s->mr, obj, &cec_ops, s, "cec", CEC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mr);
    s->tx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cec_tx_timer, s);
    s->rx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cec_rx_timer, s);
    object_property_add_bool(obj, "connected", cec_get_connected,
                             cec_set_connected);
    object_property_set_description(obj, "connected",
        "HDMI cable plugged: the CEC line reaches the sink");
}

static const Property cec_props[] = {
    DEFINE_PROP_BOOL("tv", CxdCecState, tv, true),
};

static void cec_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_legacy_reset(dc, cec_reset);
    device_class_set_props(dc, cec_props);
}

static const TypeInfo cec_info = {
    .name = TYPE_CXD_CEC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CxdCecState),
    .instance_init = cec_init,
    .class_init = cec_class_init,
};

static void cec_register(void)
{
    type_register_static(&cec_info);
}
type_init(cec_register)

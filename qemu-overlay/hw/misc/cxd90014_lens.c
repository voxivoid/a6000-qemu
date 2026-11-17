/*
 * a6k-lens: HLE of an E-mount lens MCU on the E-mount (lens=ekit).
 *
 * Reason for HLE: no lens firmware is available.  The boundary is the
 * mount: the handshake GPIOs and the UART1 byte stream (750 kbaud) that
 * av-cam moves with the 0xF0102000 memory DMA (docs/reference/RE_LENS_EMOUNT.md
 * sections 2.4, 9.2-9.7).  No MMIO of its own.
 *
 * Lines (wired by hw/arm/sony_a6000.c):
 *   in  0  P5.4  output latch, body "H1" (handshake, then high while the
 *              body transmits; av-cam 43993AD4)
 *   in  1  P2.11 output latch, body "H2" (handshake; later UART1 TXD)
 *   in  2  lens supply (sub-micro slot-B power ack, TLV 0x03 data[3] bit0)
 *   out 0  P0.5  "H1 echo" = P5.4 after 1 ms [hypothesis: delay]
 *   out 1  P2.10 "H2 echo" = P2.11 after 1 ms, only in the handshake
 *              (9.2); in UART mode the pin is UART1 RXD and idles high
 * Serial side: a cxd90014-mdma peer (cxd90014_mdma_set_periph): body frames
 * arrive from the TX channel (dst = UART1 DR, ch1), replies go into the
 * armed RX ring channel (src = UART1 DR, ch0) with
 * cxd90014_mdma_periph_rx.
 *
 * States: OFF (no supply) -> HS (echo both lines) -> UART (after P5.4
 * falls with P2.10 echoed high = handshake step 2 -> 3, 2.4 / 9.2).
 *
 * Frame (9.5, parser av-cam 436B61D8):
 *   F0 | u16 len LE | type | b4 | blocks | u16 sum LE | 55
 *   sum = sum of bytes[1 .. len-4]; blocks = {id, data}, total length per
 *   id from the block table (9.5: A = body->lens, B = lens->body, id byte
 *   included).  Each body request (type 2) is answered with one type-2
 *   frame that carries, in order, one block per requested id with that
 *   id's B length (the body checks byte 5 == pending command id), b4
 *   echoed [hypothesis: b4 unchecked for type 2].
 *
 * Reply contents (9.3 / 9.5):
 *   01 capability bitmap: bit (id-1) = supported (436CC6B0).  Supported:
 *      1, 5, 6, 7, 8, 9, 0x0A, 0x0B, 0x0D, 0x10 (9.6 minimal set; no 0x0C =
 *      stay at 750 kbaud, no 0x32 / 0x3D = skip sub-states 30/31).
 *   07 lens info (35 B -> lensInfo+33): [1] = power class 1, [2] bit1 = 0
 *      (no 1.5 Mbaud), [5] = 0 (no firmware error).  Rest 0 [hypothesis:
 *      lens id, focal length and F-number layout undecoded].
 *   09 startup info (12 B): [8] = [9] = 0 (436B7EF8).
 *   10 (2 B): [1] bit0 = 0 (436B7C60).
 *   all others (08, 0A, 0B, 0D, any later id): zero-filled [hypothesis:
 *      only presence is checked, 9.5].
 * Timing: the reply is due ~0.6 ms after the body's P5.4 falls (the body
 * holds it high for its TX + 450 us, 9.3) [hypothesis, 9.7: 0.5-1 ms is
 * a safe target]; never while P5.4 is high (P5.4 acts as the body's
 * "request to send" [hypothesis]).  A due reply is copied into the ring
 * at the body's next read of the RX channel's +0x04 (lens_rx_pull).
 * A new body frame replaces a reply that was never taken.
 *
 * Open: periodic type-1 traffic (blocks 5/6, focus/zoom/aperture state),
 * the content of blocks 7/8/0A, the 1.5 Mbaud switch.
 *
 * Aperture / F-number not yet reported (RE_LENS_EMOUNT.md sections 11.7 /
 * 11.8, measured 2026-11-16): the live F-number shown on the OSD comes from
 * the periodic type-1 Info frames (blocks 5/6), which av-cam requests only
 * under live-view VD.  In the emulator av-cam asks for the eight startup
 * blocks (01 07 08 09 0a 0b 0d 10) once per power-up and never a periodic
 * frame (confirmed idle / MENU / shutter half+full), so a pushed 5/6 frame
 * would never be read.  Block 8 (requested at startup) does feed av-cam's
 * capability table at 0x43CC5B30 (mapped: block8[150] -> table[64], the
 * class-1 word read by 436B7E5C as 100*value), but populating it does not
 * light the OSD F-number either (stays "F--").
 *
 * Decoded trigger (section 11.8, IDA + a6ktrace, 2026-11-16): the 5/6
 * request is built by sub_436CFB70 -> sub_43A4C60E -> sub_436B5188, reached
 * only from sub_4380C5A8 (the AE-result -> lens-aperture driver), which runs
 * only via the AE "control/apply" job sub_43853DA8.  That job is dispatched
 * by the per-VD AE job-slot handlers sub_43A89734 / 43A89824 / 43A898A6
 * ONLY when the AE job-context state byte [ctx+35] (sub_43A6EAFC) == 1.
 * Trace of a lens=ekit boot: AE metering runs hard (4372A38C x360,
 * 437737D4 x195), the lens is fully recognised (BL state 0x07, all 8
 * startup blocks answered), and sub_4380C5A8's own gate byte is already set
 * ([0x43E44162] == 0x01) -- yet sub_43A89734/824/8A6, sub_43853DA8 and
 * sub_4380C5A8 are NEVER executed, so block 5/6 is never enqueued.  So the
 * block is NOT a lens-reply byte (candidate "a" refuted) and NOT S1/S2
 * (candidate "b" as half-press refuted): it is av-cam's AE control/apply
 * job never being scheduled in the emulator's live-view state (the AE
 * sequencer, a separate layer).  No faithful lens-model byte can unblock
 * it; the aperture/focal-length props above stay the inert knob for that
 * later work.
 */
#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "hw/dma/cxd90014_mdma.h"
#include "trace.h"

#define TYPE_A6K_LENS "a6k-lens"
OBJECT_DECLARE_SIMPLE_TYPE(LensState, A6K_LENS)

#define LENS_UART_DR        0xF2001000u     /* UART1 DR (1.2) */
#define LENS_NS_PER_BYTE    13333           /* 10 bits at 750 kbaud */
#define LENS_ECHO_NS        (1 * SCALE_MS)  /* hypothesis */
#define LENS_REPLY_NS       (600 * SCALE_US) /* after P5.4 falls, hyp. */
#define LENS_MAXF           1024

enum { LENS_OFF, LENS_HS, LENS_UART };

struct LensState {
    SysBusDevice parent_obj;
    DeviceState *mdma;
    qemu_irq out[2];
    int in[3];                  /* P5.4, P2.11, supply */
    int echo[2];                /* level being driven on P0.5 / P2.10 */
    int state;
    bool h2_seen;               /* P2.10 echoed high in this handshake */
    QEMUTimer *echo_t;
    QEMUTimer *reply_t;
    uint8_t reply[LENS_MAXF];
    size_t reply_len;
    size_t reply_pos;
    bool reply_due;
    uint32_t frames;
    uint8_t power_class;        /* property: block 7 [1] bits1:0 */
    uint8_t drive_mode;         /* property: block 9 [1] bits1:0 */
    uint16_t aperture;          /* property: desired F-number x100 (0 = off) */
    uint16_t focal_length;      /* property: desired focal length mm (0 = off) */
};

/* block table at 0x43ED86E0 (3.2): {A body->lens, B lens->body}, id incl. */
static const uint16_t lens_blk[0x3e][2] = {
    [0x01] = { 0x021, 0x021 }, [0x02] = { 0x000, 0x008 },
    [0x03] = { 0x015, 0x000 }, [0x04] = { 0x00e, 0x000 },
    [0x05] = { 0x000, 0x0ff }, [0x06] = { 0x000, 0x0ff },
    [0x07] = { 0x002, 0x023 }, [0x08] = { 0x009, 0x0ca },
    [0x09] = { 0x005, 0x00c }, [0x0a] = { 0x011, 0x011 },
    [0x0b] = { 0x003, 0x003 }, [0x0c] = { 0x002, 0x002 },
    [0x0d] = { 0x002, 0x002 }, [0x0e] = { 0x002, 0x002 },
    [0x0f] = { 0x002, 0x002 }, [0x10] = { 0x002, 0x002 },
    [0x11] = { 0x005, 0x005 }, [0x12] = { 0x103, 0x103 },
    [0x13] = { 0x002, 0x021 }, [0x14] = { 0x010, 0x002 },
    [0x15] = { 0x408, 0x002 }, [0x16] = { 0x002, 0x002 },
    [0x17] = { 0x000, 0x003 }, [0x18] = { 0x00b, 0x009 },
    [0x19] = { 0x002, 0x002 }, [0x1a] = { 0x003, 0x000 },
    [0x1b] = { 0x006, 0x00b }, [0x1c] = { 0x001, 0x002 },
    [0x1d] = { 0x005, 0x002 }, [0x1e] = { 0x005, 0x002 },
    [0x1f] = { 0x00e, 0x002 }, [0x20] = { 0x002, 0x000 },
    [0x21] = { 0x003, 0x002 }, [0x22] = { 0x003, 0x003 },
    [0x23] = { 0x00f, 0x002 }, [0x24] = { 0x011, 0x008 },
    [0x25] = { 0x002, 0x011 }, [0x26] = { 0x002, 0x005 },
    [0x27] = { 0x006, 0x006 }, [0x28] = { 0x002, 0x024 },
    [0x29] = { 0x001, 0x000 }, [0x2a] = { 0x001, 0x000 },
    [0x2b] = { 0x002, 0x002 }, [0x2c] = { 0x001, 0x041 },
    [0x2d] = { 0x001, 0x002 }, [0x2e] = { 0x003, 0x003 },
    [0x2f] = { 0x003, 0x000 }, [0x30] = { 0x00f, 0x007 },
    [0x31] = { 0x1c2, 0x19a }, [0x32] = { 0x008, 0x008 },
    [0x33] = { 0x015, 0x000 }, [0x34] = { 0x018, 0x018 },
    [0x35] = { 0x002, 0x028 }, [0x36] = { 0x008, 0x008 },
    [0x37] = { 0x008, 0x008 }, [0x38] = { 0x00c, 0x00c },
    [0x39] = { 0x008, 0x010 }, [0x3a] = { 0x018, 0x018 },
    [0x3b] = { 0x008, 0x1ec }, [0x3c] = { 0x008, 0x004 },
    [0x3d] = { 0x018, 0x040 },
};

static const uint8_t lens_caps[] = {
    0x01, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0d, 0x10,
};

static void lens_echo_cb(void *opaque)
{
    LensState *s = opaque;
    int e0 = s->state != LENS_OFF && s->in[0];
    int e1;

    if (s->state == LENS_HS) {
        e1 = s->in[1];
        if (e1) {
            s->h2_seen = true;
        }
    } else {
        e1 = s->state == LENS_UART;     /* UART line idles high */
    }
    if (e0 != s->echo[0]) {
        s->echo[0] = e0;
        qemu_set_irq(s->out[0], e0);
    }
    if (e1 != s->echo[1]) {
        s->echo[1] = e1;
        qemu_set_irq(s->out[1], e1);
    }
}

static void lens_set_state(LensState *s, int st)
{
    if (s->state != st) {
        trace_a6k_lens_state(s->state, st);
        s->state = st;
    }
}

/*
 * The reply is due (its last byte has arrived at the body's UART).  The
 * bytes are handed to the RX ring when the body next reads ch0 +0x04
 * (lens_rx_pull), i.e. in the vCPU's own MMIO order: the RX poll 43993840
 * reads +0x04, then stops and re-arms ch0, so bytes written between that
 * read and the stop from the timer thread would be lost (observed as
 * "[BL]SYNC TIMEOUT" at random init steps).  On the real chip that window
 * is a few bus cycles.
 */
static void lens_reply_cb(void *opaque)
{
    LensState *s = opaque;

    if (s->reply_len && s->state == LENS_UART) {
        s->reply_due = true;
    }
}

static void lens_rx_pull(void *opaque)
{
    LensState *s = opaque;
    size_t n;

    if (!s->reply_due || !s->reply_len || s->state != LENS_UART) {
        return;
    }
    n = cxd90014_mdma_periph_rx(s->mdma, s->reply + s->reply_pos,
                                s->reply_len - s->reply_pos);
    trace_a6k_lens_reply(s->reply_len, s->reply_pos, n, s->reply[5],
                         qemu_clock_get_us(QEMU_CLOCK_VIRTUAL));
    s->reply_pos += n;
    if (s->reply_pos >= s->reply_len) {
        s->reply_len = 0;
        s->reply_due = false;
    }
}

static void lens_in(void *opaque, int n, int level)
{
    LensState *s = opaque;
    int old = s->in[n];
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (level == old) {
        return;
    }
    trace_a6k_lens_in(n, level, now / 1000);
    s->in[n] = level;
    if (n == 2) {
        /* the supply: the lens MCU starts in the handshake state */
        if (level && s->state == LENS_OFF) {
            s->h2_seen = false;
            lens_set_state(s, LENS_HS);
        } else if (!level) {
            lens_set_state(s, LENS_OFF);
            s->reply_len = 0;
            s->reply_due = false;
            timer_del(s->reply_t);
        }
    } else if (n == 0 && old && !level) {
        /* P5.4 falls: end of handshake step 2, or end of a body frame */
        if (s->state == LENS_HS && s->h2_seen) {
            lens_set_state(s, LENS_UART);
        } else if (s->state == LENS_UART && s->reply_len) {
            timer_mod(s->reply_t, now + LENS_REPLY_NS);
        }
    }
    timer_mod(s->echo_t, now + LENS_ECHO_NS);
}

static size_t lens_put_block(LensState *s, uint8_t *o, uint8_t id)
{
    size_t len = lens_blk[id][1];

    memset(o, 0, len);
    o[0] = id;
    switch (id) {
    case 0x01:
        for (int i = 0; i < ARRAY_SIZE(lens_caps); i++) {
            int b = lens_caps[i] - 1;
            o[1 + b / 8] |= 1 << (b % 8);
        }
        break;
    case 0x07:
        o[1] = s->power_class;  /* lensInfo[34] bits1:0 (436B7E5C) */
        break;
    case 0x09:
        o[1] = s->drive_mode;   /* class 2 only: 1 -> mode 5, 2 -> 2 */
        break;
    }
    return len;
}

/* body frame from the TX DMA channel (count rounded up to whole words) */
static void lens_rx_frame(void *opaque, const uint8_t *b, size_t len)
{
    LensState *s = opaque;
    size_t n, pos, o = 5;
    uint16_t sum = 0;
    uint8_t *r = s->reply;

    if (s->state != LENS_UART) {
        qemu_log_mask(LOG_GUEST_ERROR, "a6k-lens: %zu bytes while not in "
                      "UART mode (state %d)\n", len, s->state);
        return;
    }
    if (len < 8 || b[0] != 0xF0) {
        qemu_log_mask(LOG_GUEST_ERROR, "a6k-lens: bad header\n");
        return;
    }
    n = b[1] | b[2] << 8;
    if (n < 8 || n > len || b[n - 1] != 0x55) {
        qemu_log_mask(LOG_GUEST_ERROR, "a6k-lens: bad length/footer "
                      "(len %zu, got %zu)\n", n, len);
        return;
    }
    for (size_t i = 1; i <= n - 4; i++) {
        sum += b[i];
    }
    if (sum != (b[n - 3] | b[n - 2] << 8)) {
        qemu_log_mask(LOG_GUEST_ERROR, "a6k-lens: checksum NG\n");
        return;
    }
    s->frames++;
    trace_a6k_lens_frame(n, b[3], b[4], b[5],
                         qemu_clock_get_us(QEMU_CLOCK_VIRTUAL));
    if ((b[3] & 7) != 2) {
        qemu_log_mask(LOG_UNIMP, "a6k-lens: body frame type %d, id 0x%02x "
                      "not answered\n", b[3] & 7, b[5]);
        return;
    }

    /* answer every requested block in order */
    for (pos = 5; pos < n - 3;) {
        uint8_t id = b[pos];
        size_t a = id < ARRAY_SIZE(lens_blk) ? lens_blk[id][0] : 0;
        size_t bl = id < ARRAY_SIZE(lens_blk) ? lens_blk[id][1] : 0;

        if (!a || pos + a > n - 3) {
            qemu_log_mask(LOG_UNIMP, "a6k-lens: request block 0x%02x "
                          "(A %zu) at %zu: cannot parse\n", id, a, pos);
            break;
        }
        trace_a6k_lens_block(id, a, bl);
        if (bl && o + bl + 3 <= LENS_MAXF) {
            o += lens_put_block(s, r + o, id);
        } else if (!bl) {
            qemu_log_mask(LOG_UNIMP, "a6k-lens: block 0x%02x has no lens "
                          "reply length\n", id);
        }
        pos += a;
    }
    if (o == 5) {
        return;
    }
    r[0] = 0xF0;
    o += 3;
    r[1] = o;
    r[2] = o >> 8;
    r[3] = 2;
    r[4] = b[4];
    sum = 0;
    for (size_t i = 1; i <= o - 4; i++) {
        sum += r[i];
    }
    r[o - 3] = sum;
    r[o - 2] = sum >> 8;
    r[o - 1] = 0x55;
    s->reply_len = o;
    s->reply_pos = 0;
    s->reply_due = false;
    /*
     * Sent LENS_REPLY_NS after P5.4 falls (lens_in).  No fixed-delay
     * fallback: the body holds P5.4 for its TX + 450 us + timer latency
     * (observed 2.6 ms after TX done under TCG), and a reply that arrives
     * while P5.4 is still high is not taken as the answer (the body then
     * stalls into "[BL]SYNC TIMEOUT").
     */
}

static void lens_realize(DeviceState *dev, Error **errp)
{
    LensState *s = A6K_LENS(dev);

    if (s->mdma) {
        cxd90014_mdma_set_periph(s->mdma, LENS_UART_DR, LENS_NS_PER_BYTE,
                                 lens_rx_frame, lens_rx_pull, s);
    }
}

static void lens_reset(DeviceState *dev)
{
    LensState *s = A6K_LENS(dev);

    memset(s->in, 0, sizeof(s->in));
    memset(s->echo, 0, sizeof(s->echo));
    s->state = LENS_OFF;
    s->h2_seen = false;
    s->reply_len = 0;
    s->reply_due = false;
    timer_del(s->echo_t);
    timer_del(s->reply_t);
}

static void lens_init(Object *obj)
{
    LensState *s = A6K_LENS(obj);

    qdev_init_gpio_in(DEVICE(obj), lens_in, 3);
    qdev_init_gpio_out(DEVICE(obj), s->out, 2);
    s->echo_t = timer_new_ns(QEMU_CLOCK_VIRTUAL, lens_echo_cb, s);
    s->reply_t = timer_new_ns(QEMU_CLOCK_VIRTUAL, lens_reply_cb, s);
}

static const Property lens_props[] = {
    DEFINE_PROP_LINK("mdma", LensState, mdma, TYPE_DEVICE, DeviceState *),
    DEFINE_PROP_UINT8("power-class", LensState, power_class, 1),
    DEFINE_PROP_UINT8("drive-mode", LensState, drive_mode, 0),
    /*
     * Aperture / focal length the lens should report (0 = not reported,
     * current behaviour).  Declared as the stable knob for a future
     * live-view integration; NOT emitted on the wire yet, see the
     * "Aperture not yet reported" note in the file header and
     * docs/reference/RE_LENS_EMOUNT.md section 11.7.  Setting them today has no
     * effect because av-cam never polls the virtual lens for the live
     * aperture in the emulator (no live-view VD; measured 2026-11-16).
     */
    DEFINE_PROP_UINT16("aperture", LensState, aperture, 0),
    DEFINE_PROP_UINT16("focal-length", LensState, focal_length, 0),
};

static void lens_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = lens_realize;
    device_class_set_legacy_reset(dc, lens_reset);
    device_class_set_props(dc, lens_props);
}

static const TypeInfo lens_info = {
    .name = TYPE_A6K_LENS,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(LensState),
    .instance_init = lens_init,
    .class_init = lens_class_init,
};

static void lens_register(void)
{
    type_register_static(&lens_info);
}
type_init(lens_register)

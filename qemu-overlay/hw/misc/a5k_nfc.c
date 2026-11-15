/*
 * a5000 board NFC "One-touch" chip: a Sony FeliCa Plug class NFC Dynamic
 * Tag (NFC Forum Type 3) [? part number not in the firmware].  Not an MMIO
 * device; spec and evidence in docs/RE_NFC.md.
 *
 * Connections (Backup subsystem 0xF9 board config, nfc.ko):
 *   SIO channel 2 (0xF2405400, GIC 203)  3-wire half-duplex host link,
 *       driven by av-cam's SIO server for nfc.ko (sub_0 builds the request:
 *       +44 = 6 TX / 5 RX, 1 MHz).  The +0x04 word tells the direction:
 *       bit29 = TX, bit28 = RX [H, RE_NFC.md 1.2].
 *   SW    P7.10 (out)  chip power, 1 = on (nfc_rsrc_dev_init_data 0x688,
 *       nfc_rsrc_dev_power_off).  Off: pending command dropped, TX ignored.
 *   SEL   P7.14 (out)  link direction/handshake, traced only [H].
 *   RFDET P0.8  (in)   0 = reader field present (nfc_rsrc_get_approach,
 *       gpio_get_data_bit(0, 8)); idles 1 (no phone).  Works while SW = 0
 *       (the chip's RF side is field-powered).
 *   IRQ   GIC 125      data ready (= ext-int P0.4, gpiopin_to_irq(0, 4)).
 *       Driven directly with a one-shot pulse (raise + lower): one reader
 *       command = one rising edge = one nfc_rsrc_irq ISR run.  This needs
 *       GIC 125 edge-triggered, which the board's bootloader stub sets
 *       (ICFGR7 bit 27); as a level input a pulse is lost, and through the
 *       P0.4 pin (held 1 ms by the GPIO model) the ISR, which touches no
 *       device, re-enters ~49 times (RE_NFC.md section 3, run 4).
 *       hypothesis: the real bootloader configures 125 as edge; its ICFGR is
 *       not recoverable (the snapshot keeps only ISENABLER).  The P0.4 pin
 *       level itself is left alone: nfc.ko uses P0.4 only as an interrupt.
 *
 * Link protocol (as nfc.ko / libInfraNfc see it, RE_NFC.md section 2.3):
 *   host -> chip 9 B at every power-on: set parameter (sub_21C 0x21C),
 *       e.g. 1B FFFF 0000 00000000.  The first-ever one is sent with SW = 0
 *       (g_nfc_first_comm_flag) and is ignored here.
 *   chip -> host, IRQ then 230 B read as 128 + 102 (nfc_rsrc_dev_recv_data
 *       0x5A0): reader command [cmd][n][block list...], zero-filled.
 *   host -> chip reply (nfc_rsrc_dev_send_data 0x4D4, segments <= 128 B):
 *       Read (06) -> [00 00][16 B x n]; Write (08) -> [01 A5]; other
 *       commands get no reply (RecvPacket 0x3E3C / InfraNfc).
 * The tag image (AIB + NDEF) is built by the firmware (InfraNfc
 * create_ndef_data 0x413C); this model only carries the frames.
 *
 * Host side ("virtual phone"): chardev property, line protocol
 * (RE_NFC.md section 4.4):
 *   field on|off      RFDET 0/1                          -> ok
 *   cmd <hex>         latch a raw command, raise the IRQ -> resp <hex> | timeout
 *   read              phone emulation: AIB, NDEF blocks  -> aib/ndef/records, done
 *   tap [ms]          field on, wait set-parameter, read, hold ms, field off
 *   status            power, SEL, field, last set-parameter, command state
 *   async events      ev power on|off, ev setparam <hex>
 *
 * Open: chip identity, set-parameter field meanings, FeliCa framing the
 * chip adds, IRQ polarity and trigger type [?].
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/module.h"
#include "qemu/cutils.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/resettable.h"
#include "chardev/char-fe.h"
#include "hw/misc/cxd90014_sio.h"
#include "hw/misc/a5k_nfc.h"
#include "qom/object.h"
#include "trace.h"

OBJECT_DECLARE_SIMPLE_TYPE(A5KNfcState, A5K_NFC)

#define NFC_SIO_CH      2
#define NFC_PKT_LEN     230         /* nfc_rsrc_dev_recv_data: 128 + 102 */
#define NFC_SETPARAM    9           /* sub_21C frame length */
#define NFC_MAX_BLOCKS  12          /* RecvPacket: n_blocks 1..12 */
#define NFC_REPLY_NS    (2 * NANOSECONDS_PER_SECOND)
#define NFC_SETTLE_NS   (200 * SCALE_MS)   /* tap: wait for set-parameter */
#define NFC_SIO_TX      (1u << 29)
#define NFC_SIO_RX      (1u << 28)

enum { JOB_NONE, JOB_CMD, JOB_READ, JOB_TAP };
enum { TAP_SETTLE, TAP_READ, TAP_HOLD };

struct A5KNfcState {
    SysBusDevice parent_obj;
    qemu_irq irq;                   /* GIC 125 */
    qemu_irq rfdet;                 /* -> GPIO P0.8 input */
    CharBackend chr;

    bool powered, sel, field;
    uint8_t setparam[NFC_SETPARAM];
    bool have_setparam;
    uint32_t irq_count;

    /* outstanding reader command */
    bool outstanding;
    uint8_t cmd[NFC_PKT_LEN];
    int rx_off;
    int expect;
    uint8_t resp[256];
    int resp_len;
    QEMUTimer *deadline;

    /* host jobs */
    int job;
    int tap_step;
    int64_t tap_hold_ns;
    int64_t setparam_seen;          /* setparam count, for tap */
    QEMUTimer *job_t;
    /* read job */
    int rd_nbr, rd_nmaxb, rd_next, rd_ln;
    uint8_t ndef[NFC_MAX_BLOCKS * 16 * 22];
    int ndef_len;

    char line[1200];
    int line_len;
};

static void nfc_hex(char *out, const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++) {
        sprintf(out + 2 * i, "%02X", b[i]);
    }
    out[2 * n] = 0;
}

static void G_GNUC_PRINTF(2, 3) nfc_say(A5KNfcState *s, const char *fmt, ...)
{
    va_list ap;
    g_autofree char *m = NULL;

    va_start(ap, fmt);
    m = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    if (qemu_chr_fe_backend_connected(&s->chr)) {
        qemu_chr_fe_write_all(&s->chr, (const uint8_t *)m, strlen(m));
        qemu_chr_fe_write_all(&s->chr, (const uint8_t *)"\n", 1);
    }
}

static void nfc_set_field(A5KNfcState *s, bool on)
{
    s->field = on;
    trace_a5k_nfc_rfdet(!on);
    qemu_set_irq(s->rfdet, !on);    /* active low */
}

/* ---- reader command / reply (chip side) ---- */

static void nfc_job_resp(A5KNfcState *s, const uint8_t *r, int len);

static void nfc_cmd_done(A5KNfcState *s, bool ok)
{
    s->outstanding = false;
    timer_del(s->deadline);
    nfc_job_resp(s, ok ? s->resp : NULL, ok ? s->resp_len : 0);
}

static void nfc_deadline_cb(void *opaque)
{
    A5KNfcState *s = opaque;
    if (s->outstanding) {
        trace_a5k_nfc_timeout(s->rx_off, s->resp_len);
    }
    nfc_cmd_done(s, false);
}

/* latch a raw command and interrupt the host; the reply comes over SIO */
static void nfc_send_cmd(A5KNfcState *s, const uint8_t *c, int len)
{
    int n = len > 1 ? c[1] : 0;

    memset(s->cmd, 0, sizeof(s->cmd));
    memcpy(s->cmd, c, MIN(len, NFC_PKT_LEN));
    s->rx_off = 0;
    s->resp_len = 0;
    s->expect = (len > 0 && c[0] == 0x06) ? 2 + 16 * n : 2;
    timer_mod(s->deadline,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NFC_REPLY_NS);
    trace_a5k_nfc_cmd(len > 0 ? c[0] : -1, n, len, s->powered);
    if (!s->powered) {
        /* chip unpowered: the host never sees it; the deadline answers */
        s->outstanding = false;
        return;
    }
    s->outstanding = true;
    s->irq_count++;
    qemu_set_irq(s->irq, 1);        /* one assertion per command */
    qemu_set_irq(s->irq, 0);
}

static void nfc_xfer(void *opaque, int ch, uint32_t mode, uint8_t *buf,
                     int len)
{
    A5KNfcState *s = opaque;
    g_autofree char *hx = NULL;

    if (mode & NFC_SIO_RX) {
        /* chip -> host: the latched command, then zeros */
        trace_a5k_nfc_xfer("rx", len, s->rx_off);
        for (int i = 0; i < len; i++) {
            int o = s->rx_off + i;
            buf[i] = (s->outstanding && o < NFC_PKT_LEN) ? s->cmd[o] : 0;
        }
        if (s->outstanding) {
            s->rx_off += len;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "a5k-nfc: %d-byte read with no command pending\n",
                          len);
        }
        return;
    }
    if (!(mode & NFC_SIO_TX)) {
        qemu_log_mask(LOG_UNIMP, "a5k-nfc: SIO2 mode word 0x%08x "
                      "(neither TX bit29 nor RX bit28), len %d\n", mode, len);
        return;
    }
    hx = g_malloc(2 * len + 1);
    nfc_hex(hx, buf, len);
    trace_a5k_nfc_xfer("tx", len, s->resp_len);
    if (!s->powered) {
        /* first-comm set-parameter, sent before SW = 1 (sub 0x688) */
        trace_a5k_nfc_unpowered_tx(hx);
        return;
    }
    if (s->outstanding && s->rx_off > 0) {
        int n = MIN(len, (int)sizeof(s->resp) - s->resp_len);
        memcpy(s->resp + s->resp_len, buf, n);
        s->resp_len += n;
        /* complete at the expected length, or a status-only reply */
        if (s->resp_len >= s->expect || s->resp_len == 2) {
            g_autofree char *rh = g_malloc(2 * s->resp_len + 1);
            nfc_hex(rh, s->resp, s->resp_len);
            trace_a5k_nfc_resp(s->resp_len, rh);
            nfc_cmd_done(s, true);
        }
        return;
    }
    if (len == NFC_SETPARAM) {
        memcpy(s->setparam, buf, NFC_SETPARAM);
        s->have_setparam = true;
        s->setparam_seen++;
        trace_a5k_nfc_setparam(hx);
        nfc_say(s, "ev setparam %s", hx);
        return;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "a5k-nfc: unexpected %d-byte TX %s\n",
                  len, hx);
}

static void nfc_sw(void *opaque, int n, int level)
{
    A5KNfcState *s = opaque;
    bool on = level;

    if (on == s->powered) {
        return;
    }
    s->powered = on;
    trace_a5k_nfc_power(on);
    if (!on && s->outstanding) {
        s->outstanding = false;     /* dropped; the deadline reports it */
    }
    nfc_say(s, "ev power %s", on ? "on" : "off");
}

static void nfc_sel(void *opaque, int n, int level)
{
    A5KNfcState *s = opaque;
    s->sel = level;
    trace_a5k_nfc_sel(level);
}

/* ---- host jobs ---- */

static void nfc_job_end(A5KNfcState *s, const char *msg)
{
    if (s->job == JOB_TAP && s->tap_step == TAP_READ) {
        /* read finished: hold the phone, then take it away */
        if (msg) {
            nfc_say(s, "%s", msg);
        }
        s->tap_step = TAP_HOLD;
        timer_mod(s->job_t,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->tap_hold_ns);
        return;
    }
    s->job = JOB_NONE;
    if (msg) {
        nfc_say(s, "%s", msg);
    }
}

static void nfc_read_next(A5KNfcState *s)
{
    uint8_t c[2 + 2 * NFC_MAX_BLOCKS];
    int n = MIN(s->rd_nbr, s->rd_nmaxb - s->rd_next + 1);

    c[0] = 0x06;
    c[1] = n;
    for (int i = 0; i < n; i++) {
        c[2 + 2 * i] = 0x80;        /* 2-byte block list element */
        c[3 + 2 * i] = s->rd_next + i;
    }
    nfc_send_cmd(s, c, 2 + 2 * n);
}

static void nfc_decode_pmm(A5KNfcState *s, const uint8_t *p, int len)
{
    /* 00 01 00 01 10 | 10 00 <len16> SSID | 10 01 <len16> pass (2.4) */
    g_autofree char *ssid = NULL, *pass = NULL;
    int o = 5;

    while (o + 4 <= len) {
        int tag = p[o] << 8 | p[o + 1];
        int l = p[o + 2] << 8 | p[o + 3];
        if (o + 4 + l > len) {
            break;
        }
        if (tag == 0x1000) {
            ssid = g_strndup((const char *)p + o + 4, l);
        } else if (tag == 0x1001) {
            pass = g_strndup((const char *)p + o + 4, l);
        }
        o += 4 + l;
    }
    nfc_say(s, "mime application/x-sony-pmm ssid=%s pass=%s",
            ssid ? ssid : "?", pass ? pass : "?");
}

static void nfc_decode_ndef(A5KNfcState *s, const uint8_t *m, int len)
{
    int o = 0;

    while (o + 3 <= len) {
        uint8_t f = m[o];
        int tl = m[o + 1], il = 0, h = 2, pl;
        bool sr = f & 0x10, last = f & 0x40;

        if (sr) {
            pl = m[o + 2];
            h = 3;
        } else {
            if (o + 6 > len) {
                break;
            }
            pl = m[o + 2] << 24 | m[o + 3] << 16 | m[o + 4] << 8 | m[o + 5];
            h = 6;
        }
        if (f & 0x08) {
            il = m[o + h];
            h++;
        }
        if (o + h + tl + il + pl > len) {
            nfc_say(s, "error ndef record overruns the message");
            return;
        }
        g_autofree char *type = g_strndup((const char *)m + o + h, tl);
        const uint8_t *p = m + o + h + tl + il;
        if ((f & 7) == 2 && !strcmp(type, "application/x-sony-pmm")) {
            nfc_decode_pmm(s, p, pl);
        } else if ((f & 7) == 4 && !strcmp(type, "android.com:pkg")) {
            g_autofree char *pkg = g_strndup((const char *)p, pl);
            nfc_say(s, "aar %s", pkg);
        } else {
            g_autofree char *ph = g_malloc(2 * pl + 1);
            nfc_hex(ph, p, pl);
            nfc_say(s, "record tnf=%d type=%s payload=%s", f & 7, type, ph);
        }
        o += h + tl + il + pl;
        if (last) {
            break;
        }
    }
}

/* one reply (r = NULL: timeout) for the running job */
static void nfc_job_resp(A5KNfcState *s, const uint8_t *r, int len)
{
    g_autofree char *hx = NULL;

    if (s->job == JOB_CMD) {
        if (!r) {
            nfc_job_end(s, "timeout");
            return;
        }
        hx = g_malloc(2 * len + 1);
        nfc_hex(hx, r, len);
        s->job = JOB_NONE;
        nfc_say(s, "resp %s", hx);
        return;
    }
    if (s->job != JOB_READ && !(s->job == JOB_TAP &&
                                s->tap_step == TAP_READ)) {
        return;
    }
    if (!r) {
        nfc_job_end(s, "error timeout");
        return;
    }
    if (len < 2 || r[0] || r[1]) {
        hx = g_malloc(2 * len + 1);
        nfc_hex(hx, r, len);
        nfc_job_end(s, "error status");
        nfc_say(s, "resp %s", hx);
        return;
    }
    if (s->rd_next == 0) {
        /* block 0 = Attribute Information Block */
        const uint8_t *a = r + 2;
        int cks = 0;
        if (len < 18) {
            nfc_job_end(s, "error short aib");
            return;
        }
        for (int i = 0; i < 14; i++) {
            cks += a[i];
        }
        hx = g_malloc(33);
        nfc_hex(hx, a, 16);
        nfc_say(s, "aib %s", hx);
        s->rd_nbr = a[1];
        s->rd_nmaxb = a[3] << 8 | a[4];
        s->rd_ln = a[11] << 16 | a[12] << 8 | a[13];
        if (a[0] != 0x10 || cks != (a[14] << 8 | a[15])) {
            nfc_job_end(s, "error aib version/checksum");
            return;
        }
        nfc_say(s, "aib ver=%02x nbr=%d nbw=%d nmaxb=%d writef=%d rw=%d "
                "ln=%d cks=ok", a[0], a[1], a[2], s->rd_nmaxb, a[9], a[10],
                s->rd_ln);
        if (s->rd_nbr < 1 || s->rd_nbr > NFC_MAX_BLOCKS ||
            s->rd_nmaxb * 16 > (int)sizeof(s->ndef) ||
            s->rd_ln > s->rd_nmaxb * 16) {
            nfc_job_end(s, "error aib fields");
            return;
        }
        s->ndef_len = 0;
        s->rd_next = 1;
        if (s->rd_nmaxb == 0) {
            nfc_job_end(s, "done");
            return;
        }
        nfc_read_next(s);
        return;
    }
    {
        int n = (len - 2) / 16;
        memcpy(s->ndef + s->ndef_len, r + 2, 16 * n);
        s->ndef_len += 16 * n;
        s->rd_next += n;
        if (n == 0) {
            nfc_job_end(s, "error empty read");
            return;
        }
    }
    if (s->rd_next <= s->rd_nmaxb) {
        nfc_read_next(s);
        return;
    }
    hx = g_malloc(2 * s->rd_ln + 1);
    nfc_hex(hx, s->ndef, s->rd_ln);
    nfc_say(s, "ndef %s", hx);
    nfc_decode_ndef(s, s->ndef, s->rd_ln);
    nfc_job_end(s, "done");
}

static void nfc_start_read(A5KNfcState *s)
{
    static const uint8_t aib[] = { 0x06, 0x01, 0x80, 0x00 };
    s->rd_next = 0;
    nfc_send_cmd(s, aib, sizeof(aib));
}

static void nfc_job_cb(void *opaque)
{
    A5KNfcState *s = opaque;

    if (s->job != JOB_TAP) {
        return;
    }
    switch (s->tap_step) {
    case TAP_SETTLE:
        s->tap_step = TAP_READ;
        nfc_start_read(s);
        break;
    case TAP_HOLD:
        nfc_set_field(s, false);
        s->job = JOB_NONE;
        nfc_say(s, "ok tap");
        break;
    }
}

static int nfc_parse_hex(const char *t, uint8_t *out, int max)
{
    int n = 0, hi = -1;

    for (; *t; t++) {
        int v;
        if (*t == ' ' || *t == ':') {
            continue;
        }
        if (!g_ascii_isxdigit(*t)) {
            return -1;
        }
        v = g_ascii_xdigit_value(*t);
        if (hi < 0) {
            hi = v;
        } else {
            if (n >= max) {
                return -1;
            }
            out[n++] = hi << 4 | v;
            hi = -1;
        }
    }
    return hi < 0 ? n : -1;
}

static void nfc_line(A5KNfcState *s, char *l)
{
    char *arg;

    g_strstrip(l);
    if (!*l) {
        return;
    }
    arg = strchr(l, ' ');
    if (arg) {
        *arg++ = 0;
        g_strstrip(arg);
    } else {
        arg = l + strlen(l);
    }
    if (!strcmp(l, "status")) {
        char sp[2 * NFC_SETPARAM + 1] = "none";
        if (s->have_setparam) {
            nfc_hex(sp, s->setparam, NFC_SETPARAM);
        }
        nfc_say(s, "status power=%s sel=%d field=%s setparam=%s cmd=%s "
                "irq=%u job=%d", s->powered ? "on" : "off", s->sel,
                s->field ? "on" : "off", sp,
                s->outstanding ? "outstanding" : "idle", s->irq_count,
                s->job);
        return;
    }
    if (!strcmp(l, "field")) {
        if (!strcmp(arg, "on") || !strcmp(arg, "off")) {
            nfc_set_field(s, !strcmp(arg, "on"));
            nfc_say(s, "ok");
        } else {
            nfc_say(s, "error usage: field on|off");
        }
        return;
    }
    if (s->job != JOB_NONE) {
        nfc_say(s, "error busy");
        return;
    }
    if (!strcmp(l, "cmd")) {
        uint8_t c[NFC_PKT_LEN];
        int n = nfc_parse_hex(arg, c, sizeof(c));
        if (n < 1) {
            nfc_say(s, "error usage: cmd <hex>");
            return;
        }
        s->job = JOB_CMD;
        nfc_send_cmd(s, c, n);
        return;
    }
    if (!strcmp(l, "read")) {
        s->job = JOB_READ;
        nfc_start_read(s);
        return;
    }
    if (!strcmp(l, "tap")) {
        uint64_t ms = 500;
        if (*arg && qemu_strtou64(arg, NULL, 10, &ms)) {
            nfc_say(s, "error usage: tap [ms]");
            return;
        }
        s->job = JOB_TAP;
        s->tap_step = TAP_SETTLE;
        s->tap_hold_ns = ms * SCALE_MS;
        s->setparam_seen = 0;
        nfc_set_field(s, true);
        timer_mod(s->job_t,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NFC_SETTLE_NS);
        return;
    }
    nfc_say(s, "error unknown command (field on|off, cmd <hex>, read, "
            "tap [ms], status)");
}

static int nfc_can_receive(void *opaque)
{
    return 256;
}

static void nfc_receive(void *opaque, const uint8_t *buf, int size)
{
    A5KNfcState *s = opaque;

    for (int i = 0; i < size; i++) {
        if (buf[i] == '\n' || buf[i] == '\r') {
            s->line[s->line_len] = 0;
            s->line_len = 0;
            nfc_line(s, s->line);
        } else if (s->line_len < (int)sizeof(s->line) - 1) {
            s->line[s->line_len++] = buf[i];
        }
    }
}

/*
 * The tap job starts reading once the host has re-armed the link: SEL rises
 * to 1 at the end of nfc_rsrc_dev_init_data, after the set-parameter.  The
 * settle timer covers the chip-already-on and the driver-off cases.
 */
static void nfc_sel_tap(void *opaque, int n, int level)
{
    A5KNfcState *s = opaque;
    nfc_sel(opaque, n, level);
    if (level && s->job == JOB_TAP && s->tap_step == TAP_SETTLE &&
        s->setparam_seen) {
        timer_mod(s->job_t, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  SCALE_MS);
    }
}

/* ---- device ---- */

static void nfc_reset_hold(Object *obj, ResetType type)
{
    A5KNfcState *s = A5K_NFC(obj);

    s->powered = false;
    s->sel = false;
    s->field = false;
    s->outstanding = false;
    s->have_setparam = false;
    s->irq_count = 0;
    s->job = JOB_NONE;
    timer_del(s->deadline);
    timer_del(s->job_t);
}

static void nfc_reset_exit(Object *obj, ResetType type)
{
    /* after the GPIO model's own reset (hold phase) cleared the levels */
    A5KNfcState *s = A5K_NFC(obj);
    nfc_set_field(s, false);        /* RFDET idle high: no phone */
}

static void nfc_init(Object *obj)
{
    A5KNfcState *s = A5K_NFC(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_out_named(DEVICE(obj), &s->rfdet, "rfdet", 1);
    qdev_init_gpio_in_named(DEVICE(obj), nfc_sw, "sw", 1);
    qdev_init_gpio_in_named(DEVICE(obj), nfc_sel_tap, "sel", 1);
    s->deadline = timer_new_ns(QEMU_CLOCK_VIRTUAL, nfc_deadline_cb, s);
    s->job_t = timer_new_ns(QEMU_CLOCK_VIRTUAL, nfc_job_cb, s);
}

static void nfc_realize(DeviceState *dev, Error **errp)
{
    A5KNfcState *s = A5K_NFC(dev);

    if (qemu_chr_fe_backend_connected(&s->chr)) {
        qemu_chr_fe_set_handlers(&s->chr, nfc_can_receive, nfc_receive,
                                 NULL, NULL, s, NULL, true);
    }
}

static const Property nfc_props[] = {
    DEFINE_PROP_CHR("chardev", A5KNfcState, chr),
};

static void nfc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = nfc_realize;
    dc->desc = "a5000 NFC dynamic tag (FeliCa Plug class) on SIO2";
    rc->phases.hold = nfc_reset_hold;
    rc->phases.exit = nfc_reset_exit;
    device_class_set_props(dc, nfc_props);
}

DeviceState *a5k_nfc_create(DeviceState *sio, DeviceState *gpio,
                            qemu_irq irq, Chardev *chr)
{
    DeviceState *dev = qdev_new(TYPE_A5K_NFC);

    if (chr) {
        qdev_prop_set_chr(dev, "chardev", chr);
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, irq);
    /* Backup 0xF904DC/DD: RFDET = P0.8; 0xF904D8/D9: SW = P7.10;
     * 0xF904D6/D7: SEL = P7.14 (RE_NFC.md section 1.1) */
    qdev_connect_gpio_out_named(dev, "rfdet", 0,
                                qdev_get_gpio_in(gpio, 0 * 32 + 8));
    qdev_connect_gpio_out_named(gpio, "out", 7 * 32 + 10,
                                qdev_get_gpio_in_named(dev, "sw", 0));
    qdev_connect_gpio_out_named(gpio, "out", 7 * 32 + 14,
                                qdev_get_gpio_in_named(dev, "sel", 0));
    /* Backup 0xF904D2 = 2: SIO channel 2 */
    cxd90014_sio_attach_dir(sio, NFC_SIO_CH, nfc_xfer, A5K_NFC(dev));
    return dev;
}

static const TypeInfo nfc_info = {
    .name = TYPE_A5K_NFC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(A5KNfcState),
    .instance_init = nfc_init,
    .class_init = nfc_class_init,
};

static void nfc_register(void)
{
    type_register_static(&nfc_info);
}
type_init(nfc_register)

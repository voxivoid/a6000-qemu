/*
 * Atheros AR6003 SDIO card (camera Wi-Fi module, mmc1; AR6003 on the reference build, A6000 module unverified)
 *
 * The SD/SDIO side of the module: SD protocol (CMD5/3/7/52/53), CCCR, FBR1,
 * CIS, the function-1 host-interface register file and the four mailboxes.
 * The chip firmware behind the mailboxes (BMI, HTC, WMI) is the HLE in
 * hw/net/ar6003_fw.c.  Spec and evidence: emulator/docs/reference/RE_WIFI.md §3, §7.
 *
 * Function 0 (CMD52 fn 0):
 *   0x00 CCCR/SDIO rev 0x32   0x02 IOE  0x03 IOR (= IOE)  0x04 IEN
 *   0x05 INT pending (bit 1 = fn 1)  0x06 abort/RES  0x07 bus width
 *   0x08 card cap 0x12  0x09..0x0B common CIS ptr  0x10/0x11 fn0 block size
 *   0x12 power ctl  0x13 bus speed (SHS = 1)  0xF0 vendor (ath6kl writes 1)
 *   0x100.. FBR1 (0x109..0x10B CIS ptr, 0x110/0x111 block size)
 *   0x1000 common CIS, 0x1080 function-1 CIS
 * Function 1 (CMD52/CMD53 fn 1), stock ath6kl_sdio.ko usage:
 *   0x400..0x417  IRQ register table (HOST_INT_STATUS .. RX_LOOKAHEAD)
 *   0x418..0x41B  INT_STATUS_ENABLE .. COUNTER_INT_STATUS_ENABLE
 *   0x420/0x440   COUNT[8] / COUNT_DEC[8] (BMI credit = COUNT_DEC[4] 0x450,
 *                 ath6kl_sdio_bmi_write @ 0x16e1c)
 *   0x474/0x478/0x47C  diag window data / write addr / read addr
 *                 (ath6kl_sdio_diag_* @ 0x17110, sub_17044: bytes 1..3
 *                 first, the 4-byte write at the base triggers)
 *   0x800..0x1FFF MBOX0..3, 0x4000..0x6FFF MBOX0 extended window; host
 *                 writes end on the mailbox's last byte (= EOM)
 *
 * Open questions: real CCCR/CIS bytes of the module (RE_WIFI §9).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/sd/sd.h"
#include "hw/net/ar6003.h"
#include "trace.h"

#define CIS_COMMON      0x1000
#define CIS_FUNC1       0x1080

/* common CIS: MANFID first (mmc_io_open @ 0xa0fc stops at tuple 0x20) */
static const uint8_t cis_common[] = {
    0x20, 4, 0x71, 0x02, 0x01, 0x03,    /* MANFID 0x0271 / 0x0301 [?] */
    0x21, 2, 0x0C, 0x00,                /* FUNCID: SDIO */
    0x22, 4, 0x00, 0x80, 0x00, 0x5A,    /* FUNCE fn0: blk 128, 50 MHz */
    0xFF,
};

/* function-1 CIS (Linux cistpl_funce_func needs >= 42 bytes) */
static const uint8_t cis_func1[] = {
    0x21, 2, 0x0C, 0x00,
    0x22, 42,
    0x01,                       /* TPLFE_TYPE: function */
    0x01,                       /* FUNCTION_INFO */
    0x00,                       /* STD_IO_REV */
    0, 0, 0, 0,                 /* CARD_PSN */
    0, 0, 0, 0,                 /* CSA_SIZE */
    0x00,                       /* CSA_PROPERTY */
    0x00, 0x02,                 /* MAX_BLK_SIZE 512 [?] */
    0, 0, 0, 0,                 /* OCR */
    0, 0, 0,                    /* OP_MIN/AVG/MAX_PWR */
    0, 0, 0,                    /* SB_MIN/AVG/MAX_PWR */
    0, 0,                       /* MIN_BW */
    0, 0,                       /* OPT_BW */
    0x64, 0x00,                 /* ENABLE_TIMEOUT_VAL 100 x 10 ms */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0x20, 4, 0x71, 0x02, 0x01, 0x03,
    0xFF,
};

static SDBus *ar6003_bus(AR6003State *s)
{
    BusState *b = qdev_get_parent_bus(DEVICE(s));

    return b ? SD_BUS(b) : NULL;
}

bool ar6003_rx_pending(AR6003State *s)
{
    return !QTAILQ_EMPTY(&s->rxq);
}

static uint8_t ar6003_host_int_status(AR6003State *s)
{
    return ar6003_rx_pending(s) ? 0x01 : 0x00;   /* bit 0 = mbox 0 data */
}

static bool ar6003_fn1_int(AR6003State *s)
{
    return s->powered && (ar6003_host_int_status(s) & s->int_en[0]);
}

void ar6003_update_irq(AR6003State *s)
{
    bool level = ar6003_fn1_int(s) && (s->ien & 0x03) == 0x03;
    SDBus *bus;

    if (level == s->irq_level) {
        return;
    }
    s->irq_level = level;
    trace_ar6003_irq(level);
    bus = ar6003_bus(s);
    if (bus) {
        sdbus_set_card_irq(bus, level);
    }
}

void ar6003_rx_push(AR6003State *s, const uint8_t *data, uint32_t len,
                    uint32_t padded)
{
    AR6003Msg *m;

    if (padded < len) {
        padded = len;
    }
    m = g_malloc0(sizeof(*m) + padded);
    memcpy(m->data, data, len);
    m->len = padded;
    QTAILQ_INSERT_TAIL(&s->rxq, m, next);
    ar6003_update_irq(s);
}

static void ar6003_rx_flush(AR6003State *s)
{
    AR6003Msg *m;

    while ((m = QTAILQ_FIRST(&s->rxq))) {
        QTAILQ_REMOVE(&s->rxq, m, next);
        g_free(m);
    }
}

static uint8_t ar6003_mbox_pop(AR6003State *s)
{
    AR6003Msg *m = QTAILQ_FIRST(&s->rxq);
    uint8_t v;

    if (!m) {
        qemu_log_mask(LOG_GUEST_ERROR, "ar6003: mailbox 0 read while empty\n");
        return 0;
    }
    v = m->data[m->pos++];
    if (m->pos >= m->len) {
        QTAILQ_REMOVE(&s->rxq, m, next);
        g_free(m);
        ar6003_fw_rx_consumed(s);
        ar6003_update_irq(s);
    }
    return v;
}

/* ---- card reset / power ---- */

static void ar6003_card_reset(AR6003State *s)
{
    s->rca = 0;
    s->selected = false;
    s->xfer_active = false;
    s->ioe = 0;
    s->ien = 0;
    s->bus_if = 0;
    s->cccr_f0 = 0;
    s->cccr_hs = 0;
    s->cccr_pwr = 0;
    s->blksz[0] = s->blksz[1] = 0;
    memset(s->int_en, 0, sizeof(s->int_en));
    memset(s->count, 0, sizeof(s->count));
    memset(s->win_data, 0, sizeof(s->win_data));
    memset(s->win_waddr, 0, sizeof(s->win_waddr));
    memset(s->win_raddr, 0, sizeof(s->win_raddr));
    memset(s->hif_misc, 0, sizeof(s->hif_misc));
    s->txlen = 0;
    ar6003_rx_flush(s);
    ar6003_fw_reset(s);
    ar6003_update_irq(s);
}

static void ar6003_power_update(AR6003State *s)
{
    bool on = s->pwd && s->vdd[0] && s->vdd[1];

    if (on == s->powered) {
        return;
    }
    trace_ar6003_power(on);
    s->powered = on;
    /* both edges: full chip reset (PWD_L is the AR6003 reset line) */
    ar6003_card_reset(s);
    if (on) {
        s->count[4] = 1;            /* BMI command credit */
        s->phase = AR6003_PH_BMI;
    }
}

static void ar6003_set_pwd(void *opaque, int n, int level)
{
    AR6003State *s = opaque;

    s->pwd = level;
    ar6003_power_update(s);
}

static void ar6003_set_vdd(void *opaque, int n, int level)
{
    AR6003State *s = opaque;

    s->vdd[n] = level;
    ar6003_power_update(s);
}

/* ---- function 0 ---- */

static uint8_t ar6003_fn0_read(AR6003State *s, uint32_t a)
{
    if (a >= CIS_COMMON && a < CIS_COMMON + sizeof(cis_common)) {
        return cis_common[a - CIS_COMMON];
    }
    if (a >= CIS_FUNC1 && a < CIS_FUNC1 + sizeof(cis_func1)) {
        return cis_func1[a - CIS_FUNC1];
    }
    switch (a) {
    case 0x00: return 0x32;                 /* CCCR 2.00 / SDIO 2.00 [?] */
    case 0x01: return 0x02;                 /* SD spec 2.00 */
    case 0x02: return s->ioe;
    case 0x03: return s->ioe & 0x02;        /* IOR follows IOE at once */
    case 0x04: return s->ien;
    case 0x05: return ar6003_fn1_int(s) ? 0x02 : 0x00;
    case 0x06: return 0;
    case 0x07: return s->bus_if;
    case 0x08: return 0x12;                 /* SMB | S4MI, no LSC [?] */
    case 0x09: return CIS_COMMON & 0xFF;
    case 0x0A: return (CIS_COMMON >> 8) & 0xFF;
    case 0x0B: return (CIS_COMMON >> 16) & 0xFF;
    case 0x0C ... 0x0F: return 0;
    case 0x10: return s->blksz[0] & 0xFF;
    case 0x11: return s->blksz[0] >> 8;
    case 0x12: return s->cccr_pwr;
    case 0x13: return 0x01 | (s->cccr_hs & 0x02);   /* SHS */
    case 0x14 ... 0x16: return 0;
    case 0xF0: return s->cccr_f0;
    case 0x100: return 0x07;                /* std function: WLAN [?] */
    case 0x101 ... 0x108: return 0;
    case 0x109: return CIS_FUNC1 & 0xFF;
    case 0x10A: return (CIS_FUNC1 >> 8) & 0xFF;
    case 0x10B: return (CIS_FUNC1 >> 16) & 0xFF;
    case 0x110: return s->blksz[1] & 0xFF;
    case 0x111: return s->blksz[1] >> 8;
    }
    qemu_log_mask(LOG_UNIMP, "ar6003: fn0 read 0x%05x\n", a);
    return 0;
}

static void ar6003_fn0_write(AR6003State *s, uint32_t a, uint8_t v)
{
    switch (a) {
    case 0x02:
        s->ioe = v & 0x02;
        return;
    case 0x04:
        s->ien = v & 0x03;
        ar6003_update_irq(s);
        return;
    case 0x06:
        if (v & 0x08) {         /* RES: reset the I/O portion */
            trace_ar6003_cccr_res();
            s->ioe = 0;
            s->ien = 0;
            s->bus_if = 0;
            s->rca = 0;
            s->selected = false;
            ar6003_update_irq(s);
        }
        /* AS[2:0] abort: no data transfer is ever left pending */
        return;
    case 0x07:
        s->bus_if = v & 0xE3;
        return;
    case 0x10:
        s->blksz[0] = (s->blksz[0] & 0xFF00) | v;
        return;
    case 0x11:
        s->blksz[0] = (s->blksz[0] & 0x00FF) | (v << 8);
        return;
    case 0x12:
        s->cccr_pwr = v & 0x02;
        return;
    case 0x13:
        s->cccr_hs = v & 0x02;
        return;
    case 0xF0:
        s->cccr_f0 = v;
        return;
    case 0x110:
        s->blksz[1] = (s->blksz[1] & 0xFF00) | v;
        return;
    case 0x111:
        s->blksz[1] = (s->blksz[1] & 0x00FF) | (v << 8);
        return;
    }
    qemu_log_mask(LOG_UNIMP, "ar6003: fn0 write 0x%05x = 0x%02x\n", a, v);
}

/* ---- function 1 (HIF) ---- */

static bool ar6003_is_mbox(uint32_t a)
{
    return (a >= 0x800 && a < 0x2000) || (a >= 0x4000 && a < 0x7000);
}

static bool ar6003_is_mbox0(uint32_t a)
{
    return (a >= 0x800 && a < 0x1000) || (a >= 0x4000 && a < 0x7000);
}

static uint8_t ar6003_fn1_read(AR6003State *s, uint32_t a)
{
    AR6003Msg *m;

    if (ar6003_is_mbox(a)) {
        if (ar6003_is_mbox0(a)) {
            return ar6003_mbox_pop(s);
        }
        qemu_log_mask(LOG_UNIMP, "ar6003: read of mailbox 1-3 0x%04x\n", a);
        return 0;
    }
    switch (a) {
    case 0x400:
        return ar6003_host_int_status(s);
    case 0x401 ... 0x404:       /* CPU/ERROR/COUNTER status, MBOX_FRAME */
        return 0;
    case 0x405:                 /* RX_LOOKAHEAD_VALID */
        return ar6003_rx_pending(s) ? 0x01 : 0x00;
    case 0x406 ... 0x407:
        return 0;
    case 0x408 ... 0x40B:       /* RX_LOOKAHEAD0: next 4 bytes of mbox 0 */
        m = QTAILQ_FIRST(&s->rxq);
        if (!m || m->pos + (a - 0x408) >= m->len) {
            return 0;
        }
        return m->data[m->pos + (a - 0x408)];
    case 0x40C ... 0x417:
        return 0;
    case 0x418 ... 0x41B:
        return s->int_en[a - 0x418];
    case 0x420 ... 0x43F:
        return (a & 3) ? 0 : s->count[(a - 0x420) >> 2];
    case 0x440 ... 0x45F:
        if (a & 3) {
            return 0;
        } else {
            int i = (a - 0x440) >> 2;
            uint8_t v = s->count[i];

            if (v) {
                s->count[i]--;
            }
            return v;
        }
    case 0x474 ... 0x477:
        return s->win_data[a - 0x474];
    case 0x478 ... 0x47B:
        return s->win_waddr[a - 0x478];
    case 0x47C ... 0x47F:
        return s->win_raddr[a - 0x47C];
    }
    if (a >= 0x400 && a < 0x500) {
        qemu_log_mask(LOG_UNIMP, "ar6003: fn1 read 0x%04x\n", a);
        return s->hif_misc[a - 0x400];
    }
    qemu_log_mask(LOG_UNIMP, "ar6003: fn1 read 0x%05x\n", a);
    return 0;
}

static void ar6003_mbox_write(AR6003State *s, uint32_t a, uint8_t v)
{
    bool mbox0 = ar6003_is_mbox0(a);
    bool eom = (a == 0xFFF || a == 0x6FFF);

    if (!mbox0) {
        qemu_log_mask(LOG_UNIMP, "ar6003: write to mailbox 1-3 0x%04x\n", a);
        return;
    }
    if (s->txlen < 0x8000) {
        s->txbuf[s->txlen++] = v;
    }
    if (eom) {
        uint32_t n = s->txlen;

        s->txlen = 0;
        ar6003_fw_mbox_msg(s, s->txbuf, n);
    }
}

static void ar6003_fn1_write(AR6003State *s, uint32_t a, uint8_t v)
{
    if (ar6003_is_mbox(a)) {
        ar6003_mbox_write(s, a, v);
        return;
    }
    switch (a) {
    case 0x400 ... 0x403:       /* W1C causes: nothing is ever latched */
        return;
    case 0x418 ... 0x41B:
        s->int_en[a - 0x418] = v;
        ar6003_update_irq(s);
        return;
    case 0x474 ... 0x477:
        s->win_data[a - 0x474] = v;
        return;
    case 0x478 ... 0x47B:
        s->win_waddr[a - 0x478] = v;
        if (a == 0x478) {
            ar6003_fw_mem_write32(s, ldl_le_p(s->win_waddr),
                                  ldl_le_p(s->win_data));
        }
        return;
    case 0x47C ... 0x47F:
        s->win_raddr[a - 0x47C] = v;
        if (a == 0x47C) {
            stl_le_p(s->win_data,
                     ar6003_fw_mem_read32(s, ldl_le_p(s->win_raddr)));
        }
        return;
    }
    if (a >= 0x400 && a < 0x500) {
        qemu_log_mask(LOG_UNIMP, "ar6003: fn1 write 0x%04x = 0x%02x\n", a, v);
        s->hif_misc[a - 0x400] = v;
        return;
    }
    qemu_log_mask(LOG_UNIMP, "ar6003: fn1 write 0x%05x = 0x%02x\n", a, v);
}

static uint8_t ar6003_io_read(AR6003State *s, int fn, uint32_t a)
{
    switch (fn) {
    case 0: return ar6003_fn0_read(s, a);
    case 1: return ar6003_fn1_read(s, a);
    }
    qemu_log_mask(LOG_GUEST_ERROR, "ar6003: read of function %d\n", fn);
    return 0;
}

static void ar6003_io_write(AR6003State *s, int fn, uint32_t a, uint8_t v)
{
    switch (fn) {
    case 0:
        ar6003_fn0_write(s, a, v);
        return;
    case 1:
        ar6003_fn1_write(s, a, v);
        return;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "ar6003: write of function %d\n", fn);
}

/* ---- SD protocol ---- */

static size_t ar6003_r4(uint8_t *resp, uint32_t v)
{
    stl_be_p(resp, v);
    return 4;
}

static uint8_t ar6003_r5_flags(AR6003State *s)
{
    return s->selected ? 0x10 : 0x00;   /* IO_CURRENT_STATE: CMD / DIS */
}

static size_t ar6003_do_command(SDState *sd, SDRequest *req,
                                uint8_t *resp, size_t respsz)
{
    AR6003State *s = AR6003_SDIO(sd);
    uint32_t arg = req->arg;

    if (!s->powered) {
        return 0;               /* no response: command timeout */
    }
    trace_ar6003_cmd(req->cmd, arg);
    switch (req->cmd) {
    case 0:                     /* GO_IDLE: SD memory reset only */
        s->xfer_active = false;
        return 0;
    case 5:                     /* IO_SEND_OP_COND: R4, ready, 1 function */
        return ar6003_r4(resp, 0x80000000 | (1 << 28) | 0x00FF8000);
    case 3:                     /* SEND_RELATIVE_ADDR: R6 */
        s->rca = 0x0001;
        return ar6003_r4(resp, (uint32_t)s->rca << 16);
    case 7:                     /* SELECT/DESELECT: R1b */
        s->selected = (arg >> 16) == s->rca && s->rca;
        return ar6003_r4(resp, 0x00000000);
    case 52: {                  /* IO_RW_DIRECT: R5 */
        bool wr = arg >> 31;
        int fn = (arg >> 28) & 7;
        bool raw = (arg >> 27) & 1;
        uint32_t a = (arg >> 9) & 0x1FFFF;
        uint8_t d = arg & 0xFF;
        uint8_t flags = ar6003_r5_flags(s);

        if (wr) {
            ar6003_io_write(s, fn, a, d);
            d = raw ? ar6003_io_read(s, fn, a) : 0;
        } else {
            d = ar6003_io_read(s, fn, a);
        }
        trace_ar6003_cmd52(wr, fn, a, d);
        return ar6003_r4(resp, (flags << 8) | d);
    }
    case 53: {                  /* IO_RW_EXTENDED: R5, then data */
        bool wr = arg >> 31;
        int fn = (arg >> 28) & 7;
        bool blk = (arg >> 27) & 1;
        bool inc = (arg >> 26) & 1;
        uint32_t a = (arg >> 9) & 0x1FFFF;
        uint32_t cnt = arg & 0x1FF;
        uint32_t bs = fn <= 1 ? s->blksz[fn] : 0;

        if (blk) {
            cnt = cnt * (bs ? bs : 512);    /* count 0 = infinite: unused */
        } else if (cnt == 0) {
            cnt = 512;
        }
        s->xfer_active = cnt != 0;
        s->xfer_write = wr;
        s->xfer_inc = inc;
        s->xfer_fn = fn;
        s->xfer_addr = a;
        s->xfer_left = cnt;
        trace_ar6003_cmd53(wr, fn, a, cnt, blk, inc);
        return ar6003_r4(resp, (0x20 << 8));    /* state TRN */
    }
    case 12:                    /* STOP: not used with SDIO */
        s->xfer_active = false;
        return 0;
    default:
        /* CMD8/55/ACMD41/CMD1/CMD2/CMD9...: an SDIO-only card is silent */
        return 0;
    }
}

static void ar6003_xfer_advance(AR6003State *s)
{
    if (s->xfer_inc) {
        s->xfer_addr = (s->xfer_addr + 1) & 0x1FFFF;
    }
    if (--s->xfer_left == 0) {
        s->xfer_active = false;
    }
}

static void ar6003_write_byte(SDState *sd, uint8_t v)
{
    AR6003State *s = AR6003_SDIO(sd);

    if (!s->xfer_active || !s->xfer_write) {
        qemu_log_mask(LOG_GUEST_ERROR, "ar6003: data write without CMD53\n");
        return;
    }
    ar6003_io_write(s, s->xfer_fn, s->xfer_addr, v);
    ar6003_xfer_advance(s);
}

static uint8_t ar6003_read_byte(SDState *sd)
{
    AR6003State *s = AR6003_SDIO(sd);
    uint8_t v;

    if (!s->xfer_active || s->xfer_write) {
        qemu_log_mask(LOG_GUEST_ERROR, "ar6003: data read without CMD53\n");
        return 0;
    }
    v = ar6003_io_read(s, s->xfer_fn, s->xfer_addr);
    ar6003_xfer_advance(s);
    return v;
}

static bool ar6003_data_ready(SDState *sd)
{
    AR6003State *s = AR6003_SDIO(sd);

    return s->xfer_active && !s->xfer_write;
}

static bool ar6003_receive_ready(SDState *sd)
{
    AR6003State *s = AR6003_SDIO(sd);

    return s->xfer_active && s->xfer_write;
}

static bool ar6003_get_inserted(SDState *sd)
{
    return true;                /* soldered module */
}

static bool ar6003_get_readonly(SDState *sd)
{
    return false;
}

static void ar6003_set_client_join(Object *obj, const char *v, Error **errp)
{
    ar6003_fw_client_join(AR6003_SDIO(obj), v, errp);
}

static void ar6003_realize(DeviceState *dev, Error **errp)
{
    AR6003State *s = AR6003_SDIO(dev);

    QTAILQ_INIT(&s->rxq);
    s->txbuf = g_malloc0(0x8000);
    ar6003_fw_init(s, errp);
}

static void ar6003_instance_init(Object *obj)
{
    AR6003State *s = AR6003_SDIO(obj);

    qdev_init_gpio_in_named(DEVICE(obj), ar6003_set_pwd, "pwd", 1);
    qdev_init_gpio_in_named(DEVICE(obj), ar6003_set_vdd, "vdd", 2);
    object_property_add_str(obj, "client-join", NULL, ar6003_set_client_join);
    object_property_set_description(obj, "client-join",
        "AP mode: let the virtual client join, \"SSID,PASSPHRASE\"");
    QTAILQ_INIT(&s->rxq);
}

static void ar6003_reset(DeviceState *dev)
{
    AR6003State *s = AR6003_SDIO(dev);

    s->pwd = false;
    s->vdd[0] = s->vdd[1] = false;
    s->powered = false;
    ar6003_card_reset(s);
    s->phase = AR6003_PH_OFF;
}

static const Property ar6003_properties[] = {
    DEFINE_NIC_PROPERTIES(AR6003State, conf),
    DEFINE_PROP_STRING("aps", AR6003State, aps),
    DEFINE_PROP_STRING("client", AR6003State, client),
    DEFINE_PROP_STRING("client-psk", AR6003State, client_psk),
    DEFINE_PROP_UINT32("ready-delay-ms", AR6003State, ready_delay_ms, 10),
};

static void ar6003_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SDCardClass *sc = (SDCardClass *)klass;

    dc->desc = "Atheros AR6003 SDIO Wi-Fi (camera module)";
    dc->bus_type = TYPE_SD_BUS;
    dc->realize = ar6003_realize;
    device_class_set_legacy_reset(dc, ar6003_reset);
    device_class_set_props(dc, ar6003_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
    /* soldered on the board, created by the machine */
    dc->user_creatable = false;

    sc->do_command = ar6003_do_command;
    sc->write_byte = ar6003_write_byte;
    sc->read_byte = ar6003_read_byte;
    sc->data_ready = ar6003_data_ready;
    sc->receive_ready = ar6003_receive_ready;
    sc->get_inserted = ar6003_get_inserted;
    sc->get_readonly = ar6003_get_readonly;
}

static const TypeInfo ar6003_info = {
    .name          = TYPE_AR6003_SDIO,
    .parent        = TYPE_DEVICE,
    .instance_size = sizeof(AR6003State),
    .instance_init = ar6003_instance_init,
    .class_size    = sizeof(SDCardClass),
    .class_init    = ar6003_class_init,
};

static void ar6003_register_types(void)
{
    type_register_static(&ar6003_info);
}

type_init(ar6003_register_types)

/*
 * Sony CXD90014 USB device side: Fujitsu/Socionext F_USB20HDC
 * device controller, its HDMAC and the SoC VBUS/ID glue, plus the "A6KU"
 * virtual cable (USB over a TCP chardev).
 *
 * Spec: docs/reference/RE_USB_DEVICE_CONTROLLER.md (registers, events, DMA) and
 * docs/reference/RE_USB_HOST_PATH.md section 5 (A6KU protocol).  Everything here is
 * S/H (static analysis of the firmware 1.10 kernel driver f_usb20hdc_udc and
 * fusb_otg); section numbers below refer to RE_USB_DEVICE_CONTROLLER.md
 * unless "HP" (host path) is given.
 *
 * Blocks (one QEMU device, three MMIO regions):
 *
 *   region 0  hdc   PA 0xF0210000 size 0x20000   GIC 227 (level)
 *   region 1  hdmac PA 0xF0204000 size 0x1000    GIC 228 / 229 (error only)
 *   region 2  otg   PA 0xF2920000 size 0x1000    GIC 232 (VBUS) / 233 (ID),
 *                                                pulses
 *
 * hdc registers (section 2):
 *   0x000 CONF     bit2 soft reset, self-clearing
 *   0x004 MODE     bit0 host [?] (reads 0), bit1 device enable, bits 2/3
 *   0x008 INTEN    interrupt enables
 *   0x00C INTS     bits 3,4,8,9 latched W0C; bit1 device-event summary [?];
 *                  bits 16+n = OR(EPCTRLn status & IE)
 *   0x018 PHY tuning word (store)
 *   0x040+4n EPCMDn  strobes + EPCTRL field writes (section 2.6); reads 0
 *   0x104 line state: 0 = PC host (SDP)
 *   0x200 DEVC     bit0 force FS, bit16 soft disconnect, 24-29 event IEs
 *   0x204 DEVS     bit0 = 1, bit17 speed (1 = FS), 24-31 events W0C
 *   0x208 FADDR    store; 0x20C TSTAMP frame counter (1 kHz virtual clock)
 *   0x300-0x324 OTG* (store); 0x500 TESTC (store)
 *   0x400/0x420 + {0,4,8,C} DMACk / DMASk / DMATCIk / DMATCk (section 2.5)
 *   0x8000+4n EPCTRLn (write = restore reset state), 0x8040+4n EPCONFn,
 *   0x8080+8n EPCOUNTnA/B; 0x8100-0xFFFF buffer RAM (any width);
 *   0x10000/0x18000 DMA FIFO ports (HDMAC only; see below)
 *
 * hdmac (section 5): +0x00 DMACR (bit31 DE); channel k at +0x10+0x10k:
 *   DMACA (bit31 EB, cleared by hardware at the end), DMACB (bits 18:16 SS,
 *   written 5 at a normal end), DMACSA, DMACDA.
 *
 * otg (section 6): +0x10 bit0 VBUS (active low), bit1 ID: 3 = no cable,
 *   2 = PC cable.  Every other offset is stored.
 *
 * DMA is modelled as option (b) of section 5: the hdc performs the copy
 * itself with the HDMAC memory-side address when DMACk bit0, HDMAC EB and
 * DMACR DE are all set.  The firmware only reads EB, SS, DMATCk and DMASk.
 *
 * Endpoint engine (section 3): at most one packet in flight per EP.  OUT
 * packets are only accepted when the firmware armed the EP (DMA running for
 * it, or the READY IE enabled for PIO), so NAK = the A6KU request stays
 * pending.  Behaviour the spec flags as hang-critical:
 *   - the status-stage ZLP that the firmware pre-loads into the EP0 IN
 *     buffer at SETUP time is only taken after the OUT data stage;
 *   - EP0 STALL is cleared by hardware on the next SETUP;
 *   - INTS / DEVS event bits are write-0-to-clear;
 *   - EPCMD strobes are one-shot (EPCMD reads back 0 = never busy).
 *
 * Virtual cable: the chardev property ("chardev") is the A6KU server.  The
 * cable follows the TCP client: client connected = VBUS on (otg+0x10 = 2,
 * GIC 232 pulse), client gone = VBUS off.  This deviates from HP 5.1
 * ("the connection is not the cable"), as asked by the task.  Without a
 * client (or without a chardev) the cable is unplugged.  VBUS survives a
 * machine reset, so the cable stays plugged across a guest reboot.
 *
 * Open questions (see the spec): DEVC/EPCTRL reset values, INTS bit1/2
 * meaning, DMASk bit0, double-buffer latch rules.  Flushing the EP0 IN
 * buffer on SETUP is a hypothesis of this model (protects against a stale
 * pre-armed ZLP after a stalled or cancelled transfer).
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "chardev/char-fe.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/queue.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "system/address-spaces.h"
#include "trace.h"

#define TYPE_CXD_USBHDC "cxd90014-usbhdc"
OBJECT_DECLARE_SIMPLE_TYPE(CXDUsbHdcState, CXD_USBHDC)

#define NEP             16
#define EP0_MPS         64
#define MAXBUF          2

/* EPCTRL bits (section 2.7) */
#define EPC_EN          (1u << 0)
#define EPC_DIR_IN      (1u << 3)
#define EPC_STALL       (1u << 12)
#define EPC_NAKALL      (1u << 17)
#define EPC_IE(st)      (1u << ((st) - 8))     /* IE of status bit st */
#define EPC_ST26        (1u << 26)
#define EPC_ST27        (1u << 27)
#define EPC_STMASK      (0x1Fu << 26)
#define EPC_IEMASK      (0x1Fu << 18)
/* fields owned by EPCMD: [5:1], [15:14], 16, 17, IEs 18-22 */
#define EPC_FIELDS      (0x3Eu | (3u << 14) | (3u << 16) | EPC_IEMASK)

/* DEVC / DEVS */
#define DEV_FS          (1u << 0)
#define DEVC_DISC       (1u << 16)
#define DEVS_SPEED_FS   (1u << 17)
#define DEV_SETUP       (1u << 27)
#define DEV_RESET_END   (1u << 28)
#define DEV_RESET       (1u << 29)

#define MODE_DEV        (1u << 1)
#define CONF_SRST       (1u << 2)

/* HDMAC */
#define HD_DE           (1u << 31)
#define HD_EB           (1u << 31)

/* A6KU (HP section 5) */
#define AU_MAGIC        0x5541
#define AU_HDR          16
#define AU_MAXLEN       (64 * 1024 * 1024)
enum {
    AU_HELLO = 0x01, AU_RESET, AU_CONTROL, AU_OUT, AU_IN, AU_CANCEL,
    AU_HELLO_ACK = 0x81, AU_RESET_DONE, AU_CONTROL_DONE, AU_OUT_DONE,
    AU_IN_DONE, AU_EVENT = 0x90,
};
enum {
    ST_OK, ST_STALL, ST_NODEV, ST_CANCELLED, ST_OVERFLOW, ST_PROTO,
};
#define EV_ATTACHED     1
#define EV_DETACHED     2

/* control-transfer phases */
enum {
    CP_SETUP,           /* not yet injected */
    CP_IN_DATA,         /* collecting EP0 IN packets */
    CP_IN_STATUS,       /* inject the 0-byte status OUT packet */
    CP_OUT_DATA,        /* feeding EP0 OUT packets */
    CP_STATUS_IN,       /* take the pre-armed IN ZLP */
    CP_WAIT_ACK,        /* wait until the firmware handled the status IRQ */
};

typedef struct AuReq {
    uint8_t type;
    uint8_t ep;
    uint32_t id;
    uint32_t arg;
    GByteArray *out;        /* CONTROL: setup + data; OUT: data */
    GByteArray *in;         /* collected IN data */
    uint32_t pos;           /* OUT bytes delivered */
    bool zlp;               /* OUT: a ZLP still has to follow */
    int phase;
    uint32_t ack_bit;       /* CP_WAIT_ACK: EPCTRL0 status bit */
    QTAILQ_ENTRY(AuReq) next;
} AuReq;

typedef QTAILQ_HEAD(, AuReq) AuQueue;

typedef struct HdcEp {
    uint32_t ctrl;          /* EPCTRL without the live bits 6-11 */
    uint32_t conf;          /* EPCONF */
    uint16_t cnt_lo[MAXBUF];/* EPCOUNT [10:0], CPU written */
    uint16_t cnt_hi[MAXBUF];/* EPCOUNT [26:16], received */
    bool full[MAXBUF];      /* EP0: [0] IN buffer, [1] OUT buffer */
    uint16_t len[MAXBUF];   /* IN: length given by BUFWR */
    uint8_t cpu, usb;
    GByteArray *left;       /* host side: full packets kept after CANCEL */
} HdcEp;

struct CXDUsbHdcState {
    SysBusDevice parent_obj;
    MemoryRegion mr_hdc, mr_hdmac, mr_otg;
    qemu_irq irq_hdc, irq_dma[2], irq_vbus, irq_id;
    CharBackend chr;

    /* hdc */
    uint32_t regs[0x200];   /* 0x000-0x7FF plain storage */
    uint32_t ints;          /* latched bits 3, 4, 8, 9 */
    uint32_t devc, devs;    /* devs: event bits 24-31 only */
    uint32_t dmac[2], dmas[2], dmatci[2], dmatc[2];
    bool dma_zlp[2];        /* IN: a ZLP still has to follow the DMA */
    HdcEp ep[NEP];
    uint8_t ram[0x8000];    /* buffer RAM, index = offset - 0x8000 */

    /* hdmac / otg */
    uint32_t hdmac[0x400];
    uint32_t otg[0x400];
    bool vbus;

    /* A6KU */
    bool connected, hello, attached;
    uint32_t gen;
    GByteArray *rx;
    AuQueue q[32];          /* by endpoint address: n | (IN ? 16 : 0) */
    AuQueue qreset;
    int reset_phase;        /* 0 idle, 1 wait DEVS29 clear, 2 wait DEVS28 */
    bool kicking;
};

/* ---------------------------------------------------------------- helpers */

static inline int qidx(uint8_t ep)
{
    return (ep & 0xF) | ((ep & 0x80) ? 16 : 0);
}

static inline uint32_t ep_mps(HdcEp *e)
{
    return (e->conf >> 13) & 0x7FF;
}

static inline uint32_t ep_nbuf(HdcEp *e)
{
    uint32_t n = ((e->ctrl >> 4) & 3) + 1;
    return n > MAXBUF ? MAXBUF : n;
}

/* buffer RAM index of buffer k (section 2.8 / 2.9) */
static uint32_t ep_bufaddr(CXDUsbHdcState *s, int n, int k)
{
    HdcEp *e = &s->ep[n];
    uint32_t base = (e->conf & 0x1FFF) << 2;
    if (n == 0) {
        return base + (k ? EP0_MPS : 0);
    }
    return base + k * ((ep_mps(e) + 3) & ~3u);
}

static void ram_rw(CXDUsbHdcState *s, uint32_t off, uint8_t *p, uint32_t len,
                   bool write)
{
    if (off + len > sizeof(s->ram) || off < 0x100) {
        qemu_log_mask(LOG_GUEST_ERROR, "usbhdc: buffer 0x%x+%u out of RAM\n",
                      off, len);
        if (!write) {
            memset(p, 0, len);
        }
        return;
    }
    if (write) {
        memcpy(s->ram + off, p, len);
    } else {
        memcpy(p, s->ram + off, len);
    }
}

static uint32_t epctrl_read(CXDUsbHdcState *s, int n)
{
    HdcEp *e = &s->ep[n];
    uint32_t v = e->ctrl;
    if (n == 0) {
        v |= e->full[0] ? (1u << 9) : (1u << 8);
        v |= e->full[1] ? (1u << 11) : (1u << 10);
    } else {
        uint32_t nb = ep_nbuf(e), nfull = 0;
        for (uint32_t k = 0; k < nb; k++) {
            nfull += e->full[k];
        }
        v |= (uint32_t)(e->cpu & 3) << 6;
        if (nfull == 0) {
            v |= 1u << 10;
        }
        if (nfull == nb) {
            v |= 1u << 11;
        }
    }
    return v;
}

static void ep_reset_state(HdcEp *e, int n)
{
    e->ctrl = 0;
    memset(e->full, 0, sizeof(e->full));
    memset(e->len, 0, sizeof(e->len));
    memset(e->cnt_lo, 0, sizeof(e->cnt_lo));
    memset(e->cnt_hi, 0, sizeof(e->cnt_hi));
    e->cpu = e->usb = 0;
}

static uint32_t ints_read(CXDUsbHdcState *s)
{
    uint32_t v = s->ints & 0x318;
    if (s->devs & s->devc & 0xFF000000) {
        v |= 1u << 1;
    }
    for (int n = 0; n < 12; n++) {
        uint32_t c = s->ep[n].ctrl;
        if ((c & EPC_STMASK) & ((c & EPC_IEMASK) << 8)) {
            v |= 1u << (16 + n);
        }
    }
    return v;
}

static void hdc_update_irq(CXDUsbHdcState *s)
{
    uint32_t inten = s->regs[0x8 / 4];
    qemu_set_irq(s->irq_hdc, !!(ints_read(s) & inten & 0x0FFF031E));
}

static uint32_t devs_read(CXDUsbHdcState *s)
{
    uint32_t v = (s->devs & 0xFF000000) | 1;    /* bit0 polled = 1 */
    if (s->devc & DEV_FS) {
        v |= DEVS_SPEED_FS;
    }
    return v;
}

static uint32_t cur_speed(CXDUsbHdcState *s)
{
    return (s->devc & DEV_FS) ? 2 : 3;
}

/* ----------------------------------------------------------- A6KU output */

static void au_send(CXDUsbHdcState *s, uint8_t type, uint8_t ep, uint32_t id,
                    uint32_t arg, const uint8_t *p, uint32_t len)
{
    uint8_t h[AU_HDR];
    if (!s->connected) {
        return;
    }
    stw_le_p(h, AU_MAGIC);
    h[2] = type;
    h[3] = ep;
    stl_le_p(h + 4, id);
    stl_le_p(h + 8, len);
    stl_le_p(h + 12, arg);
    trace_cxd_usbhdc_au_tx(type, ep, id, arg, len);
    qemu_chr_fe_write_all(&s->chr, h, AU_HDR);
    if (len) {
        qemu_chr_fe_write_all(&s->chr, p, len);
    }
}

static void au_send_words(CXDUsbHdcState *s, uint8_t type, uint32_t arg,
                          const uint32_t *w, int nw, uint32_t id)
{
    uint8_t b[16];
    for (int i = 0; i < nw; i++) {
        stl_le_p(b + 4 * i, w[i]);
    }
    au_send(s, type, 0, id, arg, b, 4 * nw);
}

static void req_free(AuReq *r)
{
    if (r->out) {
        g_byte_array_unref(r->out);
    }
    if (r->in) {
        g_byte_array_unref(r->in);
    }
    g_free(r);
}

/* complete request r (already removed from its queue) */
static void req_complete(CXDUsbHdcState *s, AuReq *r, uint32_t status)
{
    uint32_t w;
    switch (r->type) {
    case AU_RESET:
        w = cur_speed(s);
        au_send_words(s, AU_RESET_DONE, status, &w, 1, r->id);
        break;
    case AU_CONTROL:
        au_send(s, AU_CONTROL_DONE, r->ep, r->id, status,
                status == ST_OK && r->in ? r->in->data : NULL,
                status == ST_OK && r->in ? r->in->len : 0);
        break;
    case AU_OUT: {
        uint8_t b[4];
        stl_le_p(b, r->pos);
        au_send(s, AU_OUT_DONE, r->ep, r->id, status, b, 4);
        break;
    }
    case AU_IN:
        au_send(s, AU_IN_DONE, r->ep, r->id, status,
                r->in ? r->in->data : NULL, r->in ? r->in->len : 0);
        break;
    }
    req_free(r);
}

static void finish(CXDUsbHdcState *s, AuQueue *q, AuReq *r, uint32_t status)
{
    QTAILQ_REMOVE(q, r, next);
    trace_cxd_usbhdc_req_done(r->type, r->ep, r->id, status);
    req_complete(s, r, status);
}

static void fail_all(CXDUsbHdcState *s, uint32_t status)
{
    AuReq *r, *t;
    for (int i = 0; i < 32; i++) {
        QTAILQ_FOREACH_SAFE(r, &s->q[i], next, t) {
            finish(s, &s->q[i], r, status);
        }
    }
    QTAILQ_FOREACH_SAFE(r, &s->qreset, next, t) {
        finish(s, &s->qreset, r, status);
    }
    s->reset_phase = 0;
    for (int n = 0; n < NEP; n++) {
        g_byte_array_set_size(s->ep[n].left, 0);
    }
}

static void au_event(CXDUsbHdcState *s, uint32_t ev)
{
    uint32_t w[2] = { s->gen, cur_speed(s) };
    trace_cxd_usbhdc_cable(ev == EV_ATTACHED ? "attached" : "detached",
                           s->gen);
    if (s->hello) {
        au_send_words(s, AU_EVENT, ev, w, 2, 0);
    }
}

/* attach = VBUS present, device enabled and pull-up on (section 4.1/4.6) */
static void update_attach(CXDUsbHdcState *s)
{
    bool cond = s->vbus && (s->regs[0x4 / 4] & MODE_DEV) &&
                !(s->devc & DEVC_DISC);
    if (cond && !s->attached) {
        s->attached = true;
        s->gen++;
        au_event(s, EV_ATTACHED);
    } else if (!cond && s->attached) {
        s->attached = false;
        fail_all(s, ST_NODEV);
        au_event(s, EV_DETACHED);
    }
}

/* ------------------------------------------------------------------- DMA */

static inline int ep_dma_ch(int n)
{
    return (n & 1) ? 1 : 0;      /* section 2.5: odd EP -> ch1, even -> ch0 */
}

static bool dma_running(CXDUsbHdcState *s, int n)
{
    int k = ep_dma_ch(n);
    return n && (s->dmac[k] & 1) && ((s->dmac[k] >> 8) & 0xF) == (uint32_t)n &&
           (s->hdmac[(0x10 + 0x10 * k) / 4] & HD_EB) &&
           (s->hdmac[0] & HD_DE);
}

static void dma_end(CXDUsbHdcState *s, int k)
{
    uint32_t *cb = &s->hdmac[(0x14 + 0x10 * k) / 4];
    s->dmac[k] &= ~1u;
    s->hdmac[(0x10 + 0x10 * k) / 4] &= ~HD_EB;
    *cb = (*cb & ~(7u << 16)) | (5u << 16);     /* SS = normal end */
    s->dma_zlp[k] = false;
    s->ints |= 1u << (8 + k);
    trace_cxd_usbhdc_dma_end(k, s->dmatc[k], s->dmatci[k], s->dmas[k]);
}

static void dma_mem(CXDUsbHdcState *s, int k, bool to_mem, uint8_t *p,
                    uint32_t len)
{
    hwaddr a = s->hdmac[(to_mem ? 0x1C : 0x18) / 4 + 4 * k] + s->dmatc[k];
    if (to_mem) {
        address_space_write(&address_space_memory, a, MEMTXATTRS_UNSPECIFIED,
                            p, len);
    } else {
        address_space_read(&address_space_memory, a, MEMTXATTRS_UNSPECIFIED,
                           p, len);
    }
}

/* ------------------------------------------------------- endpoint engine */

/*
 * IN side: is a packet ready for the host on EP n?  Returns its length or
 * -1.  For DMA the packet is produced on take.
 */
static int ep_in_peek(CXDUsbHdcState *s, int n)
{
    HdcEp *e = &s->ep[n];
    if (n == 0) {
        return e->full[0] ? e->len[0] : -1;
    }
    if (!(e->ctrl & EPC_EN) || !(e->ctrl & EPC_DIR_IN)) {
        return -1;
    }
    if (e->full[e->usb]) {
        return e->len[e->usb];
    }
    if (dma_running(s, n)) {
        int k = ep_dma_ch(n);
        uint32_t rem = s->dmatci[k] - s->dmatc[k];
        if (s->dma_zlp[k]) {
            return 0;
        }
        return MIN(rem, ep_mps(e));
    }
    return -1;
}

static void ep_in_take(CXDUsbHdcState *s, int n, uint8_t *p, uint32_t len)
{
    HdcEp *e = &s->ep[n];
    if (n == 0) {
        ram_rw(s, ep_bufaddr(s, 0, 0), p, len, false);
        e->full[0] = false;
        e->ctrl |= EPC_ST26;                    /* EP0: IN consumed */
        return;
    }
    if (e->full[e->usb]) {
        uint32_t nb = ep_nbuf(e);
        ram_rw(s, ep_bufaddr(s, n, e->usb), p, len, false);
        e->full[e->usb] = false;
        e->usb = (e->usb + 1) % nb;
        e->ctrl |= EPC_ST26;                    /* a buffer became free */
        bool empty = true;
        for (uint32_t k = 0; k < nb; k++) {
            empty &= !e->full[k];
        }
        if (empty) {
            e->ctrl |= EPC_ST27;                /* all transmitted */
        }
        return;
    }
    /* DMA */
    int k = ep_dma_ch(n);
    if (s->dma_zlp[k]) {
        dma_end(s, k);
        return;
    }
    dma_mem(s, k, false, p, len);
    s->dmatc[k] += len;
    if (s->dmatc[k] >= s->dmatci[k]) {
        if ((s->dmac[k] & (1u << 3)) && len == ep_mps(e)) {
            s->dma_zlp[k] = true;               /* ZLP needed (DMACk bit3) */
        } else {
            dma_end(s, k);
        }
    }
}

/* OUT side: can EP n accept a packet now (section 3 rule 2 / 4.4)? */
static bool ep_out_ready(CXDUsbHdcState *s, int n)
{
    HdcEp *e = &s->ep[n];
    if (n == 0) {
        return !e->full[1];
    }
    if (!(e->ctrl & EPC_EN) || (e->ctrl & (EPC_DIR_IN | EPC_NAKALL))) {
        return false;
    }
    if (dma_running(s, n)) {
        return true;
    }
    /* PIO: armed (READY IE on) and all buffers empty (one in flight) */
    if (!(e->ctrl & EPC_IE(26))) {
        return false;
    }
    for (uint32_t k = 0; k < ep_nbuf(e); k++) {
        if (e->full[k]) {
            return false;
        }
    }
    return true;
}

static void ep_out_put(CXDUsbHdcState *s, int n, uint8_t *p, uint32_t len)
{
    HdcEp *e = &s->ep[n];
    if (n == 0) {
        ram_rw(s, ep_bufaddr(s, 0, 1), p, len, true);
        e->cnt_hi[1] = len;
        e->full[1] = true;
        e->ctrl |= EPC_ST27;                    /* EP0: OUT holds a packet */
        return;
    }
    if (dma_running(s, n)) {
        int k = ep_dma_ch(n);
        uint32_t rem = s->dmatci[k] - s->dmatc[k];
        uint32_t l = MIN(len, rem);
        if (l < len) {
            qemu_log_mask(LOG_UNIMP, "usbhdc: EP%d OUT packet of %u bytes "
                          "overflows the DMA chunk (%u left), excess dropped\n",
                          n, len, rem);
        }
        dma_mem(s, k, true, p, l);
        s->dmatc[k] += l;
        if (len < ep_mps(e)) {
            s->dmas[k] |= 3;                    /* short packet / ZLP */
            if (len == 0) {
                e->cnt_hi[e->cpu] = 0;
            }
            dma_end(s, k);
        } else if (s->dmatc[k] >= s->dmatci[k]) {
            dma_end(s, k);
        }
        return;
    }
    ram_rw(s, ep_bufaddr(s, n, e->usb), p, len, true);
    e->cnt_hi[e->usb] = len;
    e->full[e->usb] = true;
    e->usb = (e->usb + 1) % ep_nbuf(e);
    e->ctrl |= EPC_ST26;                        /* data received */
}

/* -------------------------------------------------------- request engine */

static bool run_reset(CXDUsbHdcState *s)
{
    AuReq *r = QTAILQ_FIRST(&s->qreset);
    if (!r) {
        return false;
    }
    switch (s->reset_phase) {
    case 0:
        /* only start when no control transfer is half done */
        if (!QTAILQ_EMPTY(&s->q[0]) &&
            QTAILQ_FIRST(&s->q[0])->phase != CP_SETUP) {
            return false;
        }
        s->reset_phase = 1;
        if (s->devc & DEV_RESET) {
            s->devs |= DEV_RESET;
            return true;
        }
        /* fall through */
    case 1:
        if (s->devs & DEV_RESET) {
            return false;
        }
        for (int n = 0; n < NEP; n++) {
            g_byte_array_set_size(s->ep[n].left, 0);
        }
        s->reset_phase = 2;
        if (s->devc & DEV_RESET_END) {
            s->devs |= DEV_RESET_END;
            return true;
        }
        /* fall through */
    case 2:
        if (s->devs & DEV_RESET_END) {
            return false;
        }
        s->reset_phase = 0;
        finish(s, &s->qreset, r, ST_OK);
        return true;
    }
    return false;
}

static bool run_control(CXDUsbHdcState *s)
{
    AuQueue *q = &s->q[0];
    AuReq *r = QTAILQ_FIRST(q);
    HdcEp *e = &s->ep[0];
    uint8_t pkt[EP0_MPS];

    if (!r || s->reset_phase) {
        return false;
    }
    const uint8_t *setup = r->out->data;
    bool dir_in = setup[0] & 0x80;
    uint32_t wlen = lduw_le_p(setup + 6);

    if (r->phase != CP_SETUP && r->phase != CP_WAIT_ACK &&
        (e->ctrl & EPC_STALL)) {
        finish(s, q, r, ST_STALL);
        return true;
    }
    switch (r->phase) {
    case CP_SETUP:
        if (s->devs & DEV_SETUP) {
            return false;               /* previous SETUP not yet taken */
        }
        e->ctrl &= ~EPC_STALL;          /* hardware clears the EP0 stall */
        e->full[0] = false;             /* hypothesis: SETUP flushes IN */
        ram_rw(s, ep_bufaddr(s, 0, 1), (uint8_t *)setup, 8, true);
        e->cnt_hi[1] = 8;
        e->full[1] = true;              /* even if a status ZLP is there */
        if (s->devc & DEV_SETUP) {
            s->devs |= DEV_SETUP;
        }
        r->in = g_byte_array_new();
        r->pos = 8;
        r->phase = dir_in ? CP_IN_DATA : (wlen ? CP_OUT_DATA : CP_STATUS_IN);
        trace_cxd_usbhdc_setup(setup[0], setup[1], lduw_le_p(setup + 2),
                               lduw_le_p(setup + 4), wlen);
        return true;

    case CP_IN_DATA: {
        int l = ep_in_peek(s, 0);
        if (l < 0 || (s->devs & DEV_SETUP)) {
            return false;
        }
        ep_in_take(s, 0, pkt, l);
        uint32_t room = wlen - MIN(wlen, r->in->len);
        g_byte_array_append(r->in, pkt, MIN((uint32_t)l, room));
        if (l < EP0_MPS || r->in->len >= wlen) {
            r->phase = CP_IN_STATUS;
        }
        return true;
    }
    case CP_IN_STATUS:
        if (e->full[1]) {
            return false;               /* SETUP not yet read (BUFRD) */
        }
        ep_out_put(s, 0, pkt, 0);       /* status-stage ZLP from the host */
        r->phase = CP_WAIT_ACK;
        r->ack_bit = EPC_ST27;
        return true;

    case CP_OUT_DATA: {
        if (!ep_out_ready(s, 0)) {
            return false;
        }
        uint32_t total = 8 + wlen;
        uint32_t l = MIN(EP0_MPS, total - r->pos);
        if (r->out->len < total) {
            finish(s, q, r, ST_PROTO);
            return true;
        }
        ep_out_put(s, 0, r->out->data + r->pos, l);
        r->pos += l;
        if (r->pos >= total) {
            r->phase = CP_STATUS_IN;
        }
        return true;
    }
    case CP_STATUS_IN: {
        int l = ep_in_peek(s, 0);
        if (l < 0 || (s->devs & DEV_SETUP)) {
            return false;
        }
        if (l != 0) {
            qemu_log_mask(LOG_GUEST_ERROR, "usbhdc: EP0 status stage got a "
                          "%d-byte IN packet\n", l);
        }
        ep_in_take(s, 0, pkt, MIN(l, EP0_MPS));
        r->phase = CP_WAIT_ACK;
        r->ack_bit = EPC_ST26;
        return true;
    }
    case CP_WAIT_ACK: {
        /* reply once the firmware took the status event (or disabled it) */
        uint32_t c = e->ctrl;
        if ((c & r->ack_bit) && (c & (r->ack_bit >> 8))) {
            return false;
        }
        finish(s, q, r, ST_OK);
        return true;
    }
    }
    return false;
}

static bool run_out(CXDUsbHdcState *s, int n)
{
    AuQueue *q = &s->q[n];
    AuReq *r = QTAILQ_FIRST(q);
    HdcEp *e = &s->ep[n];

    if (!r || s->reset_phase) {
        return false;
    }
    if (e->ctrl & EPC_STALL) {
        finish(s, q, r, ST_STALL);
        return true;
    }
    if (r->pos >= r->out->len && !r->zlp) {
        finish(s, q, r, ST_OK);
        return true;
    }
    if (!ep_out_ready(s, n)) {
        return false;
    }
    uint32_t mps = ep_mps(e);
    if (!mps) {
        return false;
    }
    if (r->pos >= r->out->len) {
        ep_out_put(s, n, NULL, 0);
        r->zlp = false;
    } else {
        uint32_t l = MIN(mps, r->out->len - r->pos);
        ep_out_put(s, n, r->out->data + r->pos, l);
        r->pos += l;
    }
    trace_cxd_usbhdc_pkt("out", n, r->pos);
    return true;
}

static bool run_in(CXDUsbHdcState *s, int n)
{
    AuQueue *q = &s->q[16 + n];
    AuReq *r = QTAILQ_FIRST(q);
    HdcEp *e = &s->ep[n];

    if (!r || s->reset_phase) {
        return false;
    }
    uint32_t mps = ep_mps(e);
    if (e->ctrl & EPC_STALL) {
        finish(s, q, r, ST_STALL);
        return true;
    }
    if (!mps) {
        return false;
    }
    if (!r->in) {
        r->in = g_byte_array_new();
    }
    /* full packets kept from a cancelled request come first */
    if (e->left->len) {
        uint32_t l = MIN(mps, e->left->len);
        if (r->in->len + l > r->arg) {
            finish(s, q, r, r->in->len ? ST_OK : ST_OVERFLOW);
            return true;
        }
        g_byte_array_append(r->in, e->left->data, l);
        g_byte_array_remove_range(e->left, 0, l);
        if (r->in->len >= r->arg) {
            finish(s, q, r, ST_OK);
        }
        return true;
    }
    int l = ep_in_peek(s, n);
    if (l < 0) {
        return false;
    }
    if (r->in->len + l > r->arg) {
        if (r->in->len) {
            finish(s, q, r, ST_OK);     /* leave the packet for the next */
            return true;
        }
        uint8_t *p = g_malloc(l);
        ep_in_take(s, n, p, l);
        g_byte_array_append(r->in, p, r->arg);
        g_free(p);
        finish(s, q, r, ST_OVERFLOW);
        return true;
    }
    uint8_t *p = g_malloc(MAX(l, 1));
    ep_in_take(s, n, p, l);
    g_byte_array_append(r->in, p, l);
    g_free(p);
    trace_cxd_usbhdc_pkt("in", n, r->in->len);
    if ((uint32_t)l < mps || r->in->len >= r->arg) {
        finish(s, q, r, ST_OK);
    }
    return true;
}

/* run everything that can progress; called after every state change */
static void kick(CXDUsbHdcState *s)
{
    bool prog;
    int guard = 0;

    if (s->kicking) {
        return;
    }
    s->kicking = true;
    do {
        prog = run_reset(s);
        prog |= run_control(s);
        for (int n = 1; n < NEP; n++) {
            prog |= run_out(s, n);
            prog |= run_in(s, n);
        }
    } while (prog && ++guard < 100000);
    s->kicking = false;
    hdc_update_irq(s);
}

/* --------------------------------------------------------------- resets */

static void dma_reset(CXDUsbHdcState *s)
{
    for (int k = 0; k < 2; k++) {
        s->dmac[k] = s->dmas[k] = s->dmatci[k] = s->dmatc[k] = 0;
        s->dma_zlp[k] = false;
    }
}

/* device side reset values (section 2.11) */
static void hdc_soft_reset(CXDUsbHdcState *s)
{
    s->devc = 0x00010020;               /* H: disconnected, bit5 set */
    s->devs = 0;
    s->ints = 0;
    s->regs[0x8 / 4] = 0;               /* INTEN */
    s->regs[0x18 / 4] = 0;              /* PHY word (saved/restored) */
    s->regs[0x208 / 4] = 0;             /* FADDR */
    s->regs[0x500 / 4] = 0;             /* TESTC */
    dma_reset(s);
    for (int n = 0; n < NEP; n++) {
        /* hypothesis: EPCONF survives the soft reset (spec 2.11 says 0);
         * keeping it avoids buffers at 0x8000 if a path skips the layout */
        ep_reset_state(&s->ep[n], n);
    }
}

/* ------------------------------------------------------------ hdc MMIO */

static void epcmd_write(CXDUsbHdcState *s, int n, uint32_t v)
{
    HdcEp *e = &s->ep[n];
    uint32_t old_ie = e->ctrl & EPC_IEMASK;
    uint32_t f = 0;

    /* fields (section 2.6) */
    f |= ((v >> 9) & 1) << 16;
    f |= ((v >> 10) & 1) << 17;
    f |= ((v >> 12) & 0x1F) << 18;
    f |= ((v >> 23) & 3) << 1;
    f |= ((v >> 25) & 1) << 3;
    f |= ((v >> 26) & 3) << 4;
    f |= ((v >> 28) & 3) << 14;
    e->ctrl = (e->ctrl & ~EPC_FIELDS) | f;

    /* strobes, in a fixed order */
    e->ctrl &= ~(((v >> 18) & 0x1F) << 26);    /* 18-22: clear status */
    if (v & (1u << 1)) {                        /* STOP */
        e->ctrl &= ~EPC_EN;
    }
    if (v & (1u << 2)) {                        /* INIT */
        memset(e->full, 0, sizeof(e->full));
        memset(e->len, 0, sizeof(e->len));
        memset(e->cnt_lo, 0, sizeof(e->cnt_lo));
        memset(e->cnt_hi, 0, sizeof(e->cnt_hi));
        e->cpu = e->usb = 0;
    }
    if (v & (1u << 0)) {                        /* START */
        e->ctrl |= EPC_EN;
    }
    if (v & (1u << 5)) {                        /* STALL set */
        e->ctrl |= EPC_STALL;
    }
    if (v & (1u << 6)) {                        /* STALL clear */
        e->ctrl &= ~EPC_STALL;
    }
    if (v & (1u << 7)) {
        qemu_log_mask(LOG_UNIMP, "usbhdc: EPCMD%d bit7 strobe\n", n);
    }
    /* bit 8: data toggle reset, nothing to model */
    if (v & (1u << 3)) {                        /* BUFWR */
        if (n == 0) {
            e->full[0] = true;
            e->len[0] = e->cnt_lo[0] & 0x7FF;
        } else {
            e->full[e->cpu] = true;
            e->len[e->cpu] = e->cnt_lo[e->cpu] & 0x7FF;
            e->cpu = (e->cpu + 1) % ep_nbuf(e);
        }
    }
    if (v & (1u << 4)) {                        /* BUFRD */
        if (n == 0) {
            e->full[1] = false;
        } else {
            e->full[e->cpu] = false;
            e->cpu = (e->cpu + 1) % ep_nbuf(e);
            if (e->full[e->cpu]) {
                e->ctrl |= EPC_ST26;            /* another one is ready */
            }
        }
    }
    /* IN EPn: EMPTY enabled while already empty -> latch it (section 3.4) */
    if (n && (e->ctrl & EPC_DIR_IN) && (e->ctrl & EPC_IE(27)) &&
        !(old_ie & EPC_IE(27))) {
        bool empty = true;
        for (uint32_t k = 0; k < ep_nbuf(e); k++) {
            empty &= !e->full[k];
        }
        if (empty && !dma_running(s, n)) {
            e->ctrl |= EPC_ST27;
        }
    }
    trace_cxd_usbhdc_epcmd(n, v, epctrl_read(s, n));
}

static uint64_t hdc_read(void *opaque, hwaddr off, unsigned size)
{
    CXDUsbHdcState *s = opaque;
    uint32_t v;

    if (off >= 0x8100 && off < 0x10000) {
        uint32_t i = off - 0x8000;
        return ldn_le_p(s->ram + i, size);
    }
    if (off >= 0x10000) {
        qemu_log_mask(LOG_UNIMP, "usbhdc: CPU read of DMA FIFO port 0x%"
                      HWADDR_PRIx "\n", off);
        return 0;
    }
    hwaddr w = off & ~3;
    if (w >= 0x8000) {
        int i = (w - 0x8000) / 4;
        if (i < 16) {
            v = epctrl_read(s, i);
        } else if (i < 32) {
            v = s->ep[i - 16].conf;
        } else {
            int n = (i - 32) / 2, k = (i - 32) & 1;
            v = s->ep[n].cnt_lo[k] | ((uint32_t)s->ep[n].cnt_hi[k] << 16);
        }
    } else if (w >= 0x40 && w < 0x80) {
        v = 0;                                  /* EPCMD: never busy */
    } else {
        switch (w) {
        case 0x000: case 0x004: case 0x008: case 0x018:
        case 0x208: case 0x300: case 0x310: case 0x314: case 0x318:
        case 0x31C: case 0x320: case 0x324: case 0x500:
            v = s->regs[w / 4];
            break;
        case 0x00C:
            v = ints_read(s);
            break;
        case 0x104:
            v = 0;                              /* line state: PC host */
            break;
        case 0x200:
            v = s->devc;
            break;
        case 0x204:
            v = devs_read(s);
            break;
        case 0x20C:
            v = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) & 0x7FF;
            break;
        case 0x400: case 0x420:
            v = s->dmac[(w >> 5) & 1];
            break;
        case 0x404: case 0x424:
            v = s->dmas[(w >> 5) & 1];
            break;
        case 0x408: case 0x428:
            v = s->dmatci[(w >> 5) & 1];
            break;
        case 0x40C: case 0x42C:
            v = s->dmatc[(w >> 5) & 1];
            break;
        default:
            v = s->regs[w / 4];
            qemu_log_mask(LOG_UNIMP, "usbhdc: read 0x%" HWADDR_PRIx
                          " -> 0x%x\n", off, v);
            break;
        }
    }
    trace_cxd_usbhdc_read(off, v);
    return extract32(v, (off & 3) * 8, size * 8);
}

static void hdc_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    CXDUsbHdcState *s = opaque;

    if (off >= 0x8100 && off < 0x10000) {
        stn_le_p(s->ram + (off - 0x8000), size, val);
        return;
    }
    if (off >= 0x10000) {
        qemu_log_mask(LOG_UNIMP, "usbhdc: CPU write of DMA FIFO port 0x%"
                      HWADDR_PRIx "\n", off);
        return;
    }
    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR, "usbhdc: %u-byte write to register "
                      "0x%" HWADDR_PRIx "\n", size, off);
        return;
    }
    uint32_t v = val;
    trace_cxd_usbhdc_write(off, v);

    if (off >= 0x8000) {
        int i = (off - 0x8000) / 4;
        if (i < 16) {
            /* driver writes 0x500/0x400 only at init: restore reset state */
            ep_reset_state(&s->ep[i], i);
            if (v != (i ? 0x400u : 0x500u)) {
                qemu_log_mask(LOG_UNIMP, "usbhdc: EPCTRL%d write 0x%x\n",
                              i, v);
            }
        } else if (i < 32) {
            s->ep[i - 16].conf = v;
        } else {
            int n = (i - 32) / 2, k = (i - 32) & 1;
            s->ep[n].cnt_lo[k] = v & 0x7FF;
        }
        kick(s);
        return;
    }
    if (off >= 0x40 && off < 0x80) {
        epcmd_write(s, (off - 0x40) / 4, v);
        kick(s);
        return;
    }
    switch (off) {
    case 0x000:
        if (v & CONF_SRST) {
            hdc_soft_reset(s);
            update_attach(s);
        }
        s->regs[0] = v & ~CONF_SRST;            /* self-clearing */
        break;
    case 0x004:
        s->regs[1] = v;
        update_attach(s);
        break;
    case 0x008:
        s->regs[2] = v;
        break;
    case 0x00C:
        s->ints &= v | ~0x318u;                 /* W0C bits 3, 4, 8, 9 */
        break;
    case 0x018: case 0x208: case 0x300: case 0x310: case 0x314:
    case 0x318: case 0x31C: case 0x320: case 0x324: case 0x500:
        s->regs[off / 4] = v;
        break;
    case 0x200:
        s->devc = v;
        update_attach(s);
        break;
    case 0x204:
        s->devs &= v | 0x00FFFFFF;              /* W0C bits 24-31 */
        break;
    case 0x400: case 0x420: {
        int k = (off >> 5) & 1;
        if ((v & 1) && !(s->dmac[k] & 1)) {
            s->dmatc[k] = 0;                    /* new chunk */
            s->dmas[k] = 0;
            s->dma_zlp[k] = false;
        }
        s->dmac[k] = v;
        trace_cxd_usbhdc_dmac(k, v, s->dmatci[k]);
        break;
    }
    case 0x408: case 0x428:
        s->dmatci[(off >> 5) & 1] = v;
        break;
    case 0x404: case 0x424: case 0x40C: case 0x42C: case 0x20C: case 0x104:
        qemu_log_mask(LOG_GUEST_ERROR, "usbhdc: write to RO 0x%" HWADDR_PRIx
                      " = 0x%x\n", off, v);
        break;
    default:
        s->regs[off / 4] = v;
        qemu_log_mask(LOG_UNIMP, "usbhdc: write 0x%" HWADDR_PRIx " = 0x%x\n",
                      off, v);
        break;
    }
    kick(s);
}

static const MemoryRegionOps hdc_ops = {
    .read = hdc_read,
    .write = hdc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---------------------------------------------------------- hdmac MMIO */

static uint64_t hdmac_read(void *opaque, hwaddr off, unsigned size)
{
    CXDUsbHdcState *s = opaque;
    uint32_t v = s->hdmac[off / 4];
    if (off >= 0x30) {
        qemu_log_mask(LOG_UNIMP, "usbhdc: hdmac read 0x%" HWADDR_PRIx
                      " -> 0x%x\n", off, v);
    }
    return v;
}

static void hdmac_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    CXDUsbHdcState *s = opaque;
    s->hdmac[off / 4] = val;
    trace_cxd_usbhdc_hdmac_write(off, val);
    if (off >= 0x30) {
        qemu_log_mask(LOG_UNIMP, "usbhdc: hdmac write 0x%" HWADDR_PRIx
                      " = 0x%" PRIx64 "\n", off, val);
    }
    kick(s);
}

static const MemoryRegionOps hdmac_ops = {
    .read = hdmac_read,
    .write = hdmac_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ------------------------------------------------------------ otg MMIO */

static uint64_t otg_read(void *opaque, hwaddr off, unsigned size)
{
    CXDUsbHdcState *s = opaque;
    if (off == 0x10) {
        return s->vbus ? 2 : 3;                 /* VBUS active low, ID = 1 */
    }
    switch (off) {
    case 0x00: case 0x04: case 0x08: case 0x40: case 0x50:
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "usbhdc: otg read 0x%" HWADDR_PRIx
                      " -> 0x%x\n", off, s->otg[off / 4]);
    }
    return s->otg[off / 4];
}

static void otg_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    CXDUsbHdcState *s = opaque;
    switch (off) {
    case 0x00: case 0x04: case 0x08: case 0x40: case 0x50:
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "usbhdc: otg write 0x%" HWADDR_PRIx
                      " = 0x%" PRIx64 "\n", off, val);
    }
    if (off != 0x10) {
        s->otg[off / 4] = val;
    }
}

static const MemoryRegionOps otg_ops = {
    .read = otg_read,
    .write = otg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* --------------------------------------------------------------- cable */

static void set_vbus(CXDUsbHdcState *s, bool on)
{
    if (s->vbus == on) {
        return;
    }
    s->vbus = on;
    trace_cxd_usbhdc_cable(on ? "vbus-on" : "vbus-off", s->gen);
    qemu_irq_pulse(s->irq_vbus);                /* fusb_interrupt_usb_vbus */
    update_attach(s);
    kick(s);
}

/* ----------------------------------------------------------- A6KU input */

static void au_handle(CXDUsbHdcState *s, const uint8_t *h, const uint8_t *p,
                      uint32_t len)
{
    uint8_t type = h[2], ep = h[3];
    uint32_t id = ldl_le_p(h + 4), arg = ldl_le_p(h + 12);
    AuReq *r;

    trace_cxd_usbhdc_au_rx(type, ep, id, arg, len);
    switch (type) {
    case AU_HELLO: {
        uint32_t w[4] = { 1, s->attached, s->gen, cur_speed(s) };
        s->hello = true;
        au_send_words(s, AU_HELLO_ACK, 0, w, 4, id);
        return;
    }
    case AU_CANCEL:
        for (int i = 0; i < 32; i++) {
            QTAILQ_FOREACH(r, &s->q[i], next) {
                if (r->id != arg) {
                    continue;
                }
                if (r->type == AU_IN && r->in && r->in->len) {
                    /* keep packets already taken from the firmware */
                    HdcEp *e = &s->ep[r->ep & 0xF];
                    g_byte_array_prepend(e->left, r->in->data, r->in->len);
                    g_byte_array_set_size(r->in, 0);
                }
                finish(s, &s->q[i], r, ST_CANCELLED);
                kick(s);
                return;
            }
        }
        QTAILQ_FOREACH(r, &s->qreset, next) {
            if (r->id == arg) {
                finish(s, &s->qreset, r, ST_CANCELLED);
                s->reset_phase = 0;
                kick(s);
                return;
            }
        }
        return;
    case AU_RESET: case AU_CONTROL: case AU_OUT: case AU_IN:
        break;
    default:
        au_send(s, type | 0x80, ep, id, ST_PROTO, NULL, 0);
        return;
    }

    r = g_new0(AuReq, 1);
    r->type = type;
    r->ep = ep;
    r->id = id;
    r->arg = arg;
    r->out = g_byte_array_new();
    g_byte_array_append(r->out, p, len);

    bool bad = (type == AU_CONTROL && (len < 8 ||
                (!(p[0] & 0x80) && len < 8u + lduw_le_p(p + 6)))) ||
               (type == AU_OUT && (ep & 0x80 || !(ep & 0xF))) ||
               (type == AU_IN && (!(ep & 0x80) || !(ep & 0xF)));
    if (bad) {
        trace_cxd_usbhdc_req_done(type, ep, id, ST_PROTO);
        req_complete(s, r, ST_PROTO);
        return;
    }
    if (!s->attached) {
        req_complete(s, r, ST_NODEV);
        return;
    }
    if (type == AU_OUT) {
        r->zlp = (len == 0 && (arg & 2)) ||
                 (len > 0 && (arg & 1) &&
                  ep_mps(&s->ep[ep & 0xF]) &&
                  len % ep_mps(&s->ep[ep & 0xF]) == 0);
    }
    if (type == AU_RESET) {
        QTAILQ_INSERT_TAIL(&s->qreset, r, next);
    } else if (type == AU_CONTROL) {
        r->ep = 0;
        QTAILQ_INSERT_TAIL(&s->q[0], r, next);
    } else {
        QTAILQ_INSERT_TAIL(&s->q[qidx(ep)], r, next);
    }
    kick(s);
}

static int au_can_receive(void *opaque)
{
    return 65536;
}

static void au_receive(void *opaque, const uint8_t *buf, int size)
{
    CXDUsbHdcState *s = opaque;

    g_byte_array_append(s->rx, buf, size);
    while (s->rx->len >= AU_HDR) {
        const uint8_t *h = s->rx->data;
        uint32_t len = ldl_le_p(h + 8);
        if (lduw_le_p(h) != AU_MAGIC || len > AU_MAXLEN) {
            qemu_log_mask(LOG_GUEST_ERROR, "usbhdc: bad A6KU header, "
                          "dropping %u buffered bytes\n", s->rx->len);
            g_byte_array_set_size(s->rx, 0);
            return;
        }
        if (s->rx->len < AU_HDR + len) {
            return;
        }
        uint8_t hdr[AU_HDR];
        memcpy(hdr, h, AU_HDR);
        GByteArray *pl = g_byte_array_new();
        g_byte_array_append(pl, h + AU_HDR, len);
        g_byte_array_remove_range(s->rx, 0, AU_HDR + len);
        au_handle(s, hdr, pl->data, len);
        g_byte_array_unref(pl);
    }
}

static void au_chr_event(void *opaque, QEMUChrEvent event)
{
    CXDUsbHdcState *s = opaque;

    switch (event) {
    case CHR_EVENT_OPENED:
        s->connected = true;
        s->hello = false;
        g_byte_array_set_size(s->rx, 0);
        set_vbus(s, true);                      /* cable plugged */
        break;
    case CHR_EVENT_CLOSED:
        s->hello = false;
        s->connected = false;                   /* nobody to reply to */
        fail_all(s, ST_CANCELLED);
        g_byte_array_set_size(s->rx, 0);
        set_vbus(s, false);                     /* cable pulled */
        break;
    default:
        break;
    }
}

/* -------------------------------------------------------------- device */

static void usbhdc_reset(DeviceState *dev)
{
    CXDUsbHdcState *s = CXD_USBHDC(dev);

    /* machine reset: the gadget goes away, the cable (VBUS) stays */
    if (s->attached) {
        s->attached = false;
        fail_all(s, ST_NODEV);
        au_event(s, EV_DETACHED);
    }
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->hdmac, 0, sizeof(s->hdmac));
    memset(s->otg, 0, sizeof(s->otg));
    s->reset_phase = 0;
    hdc_soft_reset(s);
    for (int n = 0; n < NEP; n++) {
        s->ep[n].conf = 0;
    }
    qemu_set_irq(s->irq_hdc, 0);
}

static void usbhdc_realize(DeviceState *dev, Error **errp)
{
    CXDUsbHdcState *s = CXD_USBHDC(dev);

    if (qemu_chr_fe_backend_connected(&s->chr)) {
        qemu_chr_fe_set_handlers(&s->chr, au_can_receive, au_receive,
                                 au_chr_event, NULL, s, NULL, true);
    }
}

static void usbhdc_init(Object *obj)
{
    CXDUsbHdcState *s = CXD_USBHDC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mr_hdc, obj, &hdc_ops, s, "usb-hdc", 0x20000);
    memory_region_init_io(&s->mr_hdmac, obj, &hdmac_ops, s, "usb-hdmac",
                          0x1000);
    memory_region_init_io(&s->mr_otg, obj, &otg_ops, s, "usb-otg", 0x1000);
    sysbus_init_mmio(sbd, &s->mr_hdc);
    sysbus_init_mmio(sbd, &s->mr_hdmac);
    sysbus_init_mmio(sbd, &s->mr_otg);
    sysbus_init_irq(sbd, &s->irq_hdc);          /* 0: GIC 227 */
    sysbus_init_irq(sbd, &s->irq_dma[0]);       /* 1: GIC 228 */
    sysbus_init_irq(sbd, &s->irq_dma[1]);       /* 2: GIC 229 */
    sysbus_init_irq(sbd, &s->irq_vbus);         /* 3: GIC 232 */
    sysbus_init_irq(sbd, &s->irq_id);           /* 4: GIC 233 */
    s->rx = g_byte_array_new();
    for (int i = 0; i < 32; i++) {
        QTAILQ_INIT(&s->q[i]);
    }
    QTAILQ_INIT(&s->qreset);
    for (int n = 0; n < NEP; n++) {
        s->ep[n].left = g_byte_array_new();
    }
}

static const Property usbhdc_props[] = {
    DEFINE_PROP_CHR("chardev", CXDUsbHdcState, chr),
};

static void usbhdc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = usbhdc_realize;
    device_class_set_legacy_reset(dc, usbhdc_reset);
    device_class_set_props(dc, usbhdc_props);
    dc->desc = "CXD90014 F_USB20HDC USB device + HDMAC + VBUS glue (A6KU)";
}

static const TypeInfo usbhdc_info = {
    .name = TYPE_CXD_USBHDC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CXDUsbHdcState),
    .instance_init = usbhdc_init,
    .class_init = usbhdc_class_init,
};

static void usbhdc_register(void)
{
    type_register_static(&usbhdc_info);
}
type_init(usbhdc_register)

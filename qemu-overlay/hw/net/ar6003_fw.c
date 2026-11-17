/*
 * Atheros AR6003 chip-firmware HLE (camera Wi-Fi module; AR6003 on the reference build, A6000 module unverified)
 *
 * HLE boundary (AI_WORKFLOW §1.5): the chip's Xtensa mask ROM (BMI loader)
 * is not in the firmware package, so the downloaded athwlan.bin cannot run.
 * This file reproduces what the stock ath6kl_sdio.ko (QCA olca 3.4.0.189)
 * sees through the SDIO mailbox: the BMI loader protocol, then HTC and the
 * WMI command/event protocol of the "SMARTPHONE" firmware, backed by a
 * "virtual air" (virtual APs, a virtual client) bridged onto one QEMU
 * netdev.  Spec: emulator/docs/reference/RE_WIFI.md §3.4-3.6, §7.4-7.7.
 *
 *  BMI  (raw LE u32 words in mailbox 0, polled):
 *       1 DONE, 2 READ_MEMORY, 3 WRITE_MEMORY, 4 EXECUTE, 5 SET_APP_START,
 *       6/7 SOC register r/w, 8 GET_TARGET_INFO, 13 LZ_STREAM_START,
 *       14 LZ_DATA.  Target RAM: sparse 4 KiB pages, addr & 0x1FFFFF.
 *  HTC  (6-byte header, messages padded to 128 bytes): READY, CONN_SVC,
 *       SETUP_COMPLETE(_EX), credit-report trailers.
 *  WMI  (6-byte header on the control endpoint, wmi_data_hdr on data).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/cutils.h"
#include "qapi/error.h"
#include "net/net.h"
#include "net/eth.h"
#include "hw/net/ar6003.h"
#include "crypto/hmac.h"
#include "crypto/pbkdf.h"
#include "crypto/cipher.h"
#include "crypto/random.h"
#include "trace.h"

/* ---- target memory ---- */

#define AR6003_TARGET_VERSION   0x30000582  /* "ar6003 hw 2.1.1" */
#define AR6003_TARGET_TYPE      3           /* AR6003 in this driver */
#define AR6003_HI_BASE          0x540600    /* host interest area */
#define AR6003_HI_BOARD_DATA    (AR6003_HI_BASE + 0x54)  /* [?] upstream */
#define AR6003_BOARD_ADDR       0x542400    /* hypothesis: RAM address */
#define AR6003_SW_VERSION       0x030400BD  /* 3.4.0.189 (fw-4.bin) */

static uint8_t *ar6003_page(AR6003State *s, uint32_t addr, bool create)
{
    gpointer key = GUINT_TO_POINTER((addr & 0x1FFFFF) >> 12);
    uint8_t *p = g_hash_table_lookup(s->mem, key);

    if (!p && create) {
        p = g_malloc0(4096);
        g_hash_table_insert(s->mem, key, p);
    }
    return p;
}

static void ar6003_mem_write(AR6003State *s, uint32_t addr,
                             const uint8_t *buf, uint32_t len)
{
    while (len) {
        uint32_t off = addr & 0xFFF;
        uint32_t n = MIN(len, 4096 - off);

        memcpy(ar6003_page(s, addr, true) + off, buf, n);
        addr += n;
        buf += n;
        len -= n;
    }
}

static void ar6003_mem_read(AR6003State *s, uint32_t addr,
                            uint8_t *buf, uint32_t len)
{
    while (len) {
        uint32_t off = addr & 0xFFF;
        uint32_t n = MIN(len, 4096 - off);
        uint8_t *p = ar6003_page(s, addr, false);

        if (p) {
            memcpy(buf, p + off, n);
        } else {
            memset(buf, 0, n);
        }
        addr += n;
        buf += n;
        len -= n;
    }
}

uint32_t ar6003_fw_mem_read32(AR6003State *s, uint32_t addr)
{
    uint8_t b[4];

    ar6003_mem_read(s, addr, b, 4);
    trace_ar6003_diag("read", addr, ldl_le_p(b));
    return ldl_le_p(b);
}

void ar6003_fw_mem_write32(AR6003State *s, uint32_t addr, uint32_t val)
{
    uint8_t b[4];

    trace_ar6003_diag("write", addr, val);
    stl_le_p(b, val);
    ar6003_mem_write(s, addr, b, 4);
}

/* ---- HTC ---- */

#define HTC_HDR_LEN             6
#define HTC_FLG_NEED_CREDIT     0x01
#define HTC_FLG_TRAILER         0x02
#define HTC_MSG_READY           1
#define HTC_MSG_CONN_SVC        2
#define HTC_MSG_CONN_SVC_RESP   3
#define HTC_MSG_SETUP_COMPLETE  4
#define HTC_MSG_SETUP_COMPLETE_EX 5
#define HTC_CREDITS             24          /* [?] */
#define HTC_CREDIT_SIZE         1664        /* [?] */
#define HTC_MAX_MSG             1664

#define EP_CTRL     0
#define EP_WMI      1                       /* service 0x100 */
#define EP_DATA_BE  2                       /* 0x101; BK 3, VI 4, VO 5 */

static uint32_t ar6003_pad(uint32_t len)
{
    return ROUND_UP(len, AR6003_MBOX_BLOCK);
}

/*
 * Queue one HTC message (header + payload + credit-report trailer for every
 * credit the host has used since the last report).
 */
static void ar6003_htc_send(AR6003State *s, int eid, const uint8_t *pl,
                            uint32_t len)
{
    uint8_t buf[HTC_HDR_LEN + HTC_MAX_MSG + 32];
    uint8_t trailer[2 + 2 * 8];
    uint32_t tlen = 0;
    uint32_t total;

    if (s->setup_done || s->phase >= AR6003_PH_HTC) {
        uint32_t n = 0;

        for (int e = 1; e < 8; e++) {
            if (s->credits_pending[e]) {
                trailer[2 + 2 * n] = e;
                trailer[3 + 2 * n] = MIN(s->credits_pending[e], 255);
                s->credits_pending[e] -= trailer[3 + 2 * n];
                n++;
            }
        }
        if (n) {
            trailer[0] = 1;                 /* HTC_RECORD_CREDITS */
            trailer[1] = 2 * n;
            tlen = 2 + 2 * n;
        }
    }
    if (len + tlen > HTC_MAX_MSG) {
        qemu_log_mask(LOG_GUEST_ERROR, "ar6003: HTC message too long %u\n",
                      len);
        return;
    }
    buf[0] = eid;
    buf[1] = tlen ? HTC_FLG_TRAILER : 0;
    stw_le_p(buf + 2, len + tlen);
    buf[4] = tlen;
    buf[5] = 0;
    memcpy(buf + HTC_HDR_LEN, pl, len);
    memcpy(buf + HTC_HDR_LEN + len, trailer, tlen);
    total = HTC_HDR_LEN + len + tlen;
    trace_ar6003_htc_tx(eid, len, tlen);
    ar6003_rx_push(s, buf, total, ar6003_pad(total));
}

static bool ar6003_credits_due(AR6003State *s)
{
    for (int e = 1; e < 8; e++) {
        if (s->credits_pending[e]) {
            return true;
        }
    }
    return false;
}

/* credit report without payload on the control endpoint */
static void ar6003_credit_cb(void *opaque)
{
    AR6003State *s = opaque;

    if (s->powered && ar6003_credits_due(s)) {
        ar6003_htc_send(s, EP_CTRL, NULL, 0);
    }
}

static void ar6003_credit_kick(AR6003State *s)
{
    if (!timer_pending(s->credit_timer)) {
        timer_mod(s->credit_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 200 * SCALE_US);
    }
}

void ar6003_fw_rx_consumed(AR6003State *s)
{
    /* nothing: credit reports ride on the next message or the timer */
}

/* ---- WMI ---- */

#define WMI_READY_EVENTID               0x1001
#define WMI_CONNECT_EVENTID             0x1002
#define WMI_DISCONNECT_EVENTID          0x1003
#define WMI_BSSINFO_EVENTID             0x1004
#define WMI_REGDOMAIN_EVENTID           0x1006
#define WMI_SCAN_COMPLETE_EVENTID       0x100A
#define WMI_CANCEL_REMAIN_ON_CHNL_EVENTID 0x9008
#define WMI_TX_STATUS_EVENTID           0x9009

#define WMI_CONNECT_CMDID               1
#define WMI_RECONNECT_CMDID             2
#define WMI_DISCONNECT_CMDID            3
#define WMI_START_SCAN_CMDID            7
#define WMI_ADD_CIPHER_KEY_CMDID        22
#define WMI_DELETE_CIPHER_KEY_CMDID     23
#define WMI_SET_APPIE_CMDID             63
#define WMI_SET_IE_CMDID                74
#define WMI_AP_CONFIG_COMMIT_CMDID      0xF00F
#define WMI_AP_SET_MLME_CMDID           0xF010
#define WMI_REMAIN_ON_CHNL_CMDID        0xF067
#define WMI_CANCEL_REMAIN_ON_CHNL_CMDID 0xF068
#define WMI_SEND_ACTION_CMDID           0xF069
#define WMI_SEND_MGMT_CMDID             0xF08A
#define WMI_BEGIN_SCAN_CMDID            0xF08B

static void ar6003_wmi_event(AR6003State *s, int vif, uint16_t id,
                             const uint8_t *data, uint32_t len)
{
    uint8_t buf[HTC_MAX_MSG];

    if (len + 6 > sizeof(buf) - 32) {
        return;
    }
    stw_le_p(buf, id);
    stw_le_p(buf + 2, vif & 0xF);
    stw_le_p(buf + 4, 0);
    memcpy(buf + 6, data, len);
    trace_ar6003_wmi_evt(vif, id, len);
    ar6003_htc_send(s, EP_WMI, buf, len + 6);
}

static void ar6003_wmi_ready(AR6003State *s)
{
    uint8_t ev[15];

    stl_le_p(ev, AR6003_SW_VERSION);
    stl_le_p(ev + 4, 1);                    /* ABI version, must be 1 */
    memcpy(ev + 8, s->mac, 6);
    ev[14] = 0x03;                          /* phy_cap: 11g [?] */
    s->wmi_ready_sent = true;
    ar6003_wmi_event(s, 0, WMI_READY_EVENTID, ev, sizeof(ev));
}

/* ---- virtual air: frames ---- */

#define WMI_NW_INFRA        1
#define WMI_NW_AP           16
#define WMI_CRYPTO_NONE     1
#define ETH_P_EAPOL_        0x888E

static uint16_t ar6003_ch2freq(int ch)
{
    return ch == 14 ? 2484 : 2407 + 5 * ch;
}

static int ar6003_freq2ch(int freq)
{
    return freq == 2484 ? 14 : (freq - 2407) / 5;
}

/* MAC of vif @i (ath6kl_interface_add @ 0x9a34: vif > 0 flips bit (1 << i)
 * of byte 0, sets the locally administered bit and, with fw capability
 * bit 9 (set in fw-4.bin), flips bit 7 of byte 4) */
static void ar6003_vif_mac(AR6003State *s, int i, uint8_t *mac)
{
    memcpy(mac, s->mac, 6);
    if (i) {
        mac[0] = (mac[0] ^ (1 << i)) | 2;
        mac[4] ^= 0x80;
    }
}

static const uint8_t ar6003_rsn_ie[] = {
    0x30, 0x14, 0x01, 0x00,
    0x00, 0x0f, 0xac, 0x04,                 /* group: CCMP */
    0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,     /* pairwise: CCMP */
    0x01, 0x00, 0x00, 0x0f, 0xac, 0x02,     /* AKM: PSK */
    0x00, 0x00,
};

/* beacon / probe-response IEs of virtual AP @i */
static uint32_t ar6003_ap_ies(AR6003State *s, int i, uint8_t *b)
{
    AR6003AP *ap = &s->ap[i];
    static const uint8_t rates[] = { 0x01, 0x08, 0x82, 0x84, 0x8b, 0x96,
                                     0x0c, 0x12, 0x18, 0x24 };
    static const uint8_t xrates[] = { 0x32, 0x04, 0x30, 0x48, 0x60, 0x6c };
    uint32_t n = 0;
    size_t sl = strlen(ap->ssid);

    b[n++] = 0;
    b[n++] = sl;
    memcpy(b + n, ap->ssid, sl);
    n += sl;
    memcpy(b + n, rates, sizeof(rates));
    n += sizeof(rates);
    b[n++] = 3;
    b[n++] = 1;
    b[n++] = ap->ch;
    memcpy(b + n, xrates, sizeof(xrates));
    n += sizeof(xrates);
    if (ap->psk[0]) {
        memcpy(b + n, ar6003_rsn_ie, sizeof(ar6003_rsn_ie));
        n += sizeof(ar6003_rsn_ie);
    }
    return n;
}

static uint16_t ar6003_ap_capab(AR6003State *s, int i)
{
    return 0x0001 | 0x0400 | (s->ap[i].psk[0] ? 0x0010 : 0);
}

/* WMI data frame (802.3 + LLC/SNAP, ath6kl_wmi_dot3_2_dix) to the host */
static void ar6003_data_rx(AR6003State *s, int vif, const uint8_t *eth,
                           uint32_t len)
{
    uint8_t buf[HTC_MAX_MSG];
    uint32_t pl;

    if (len < 14 || len - 14 + 6 + 14 + 8 > HTC_MAX_MSG - 32) {
        s->dropped++;
        return;
    }
    pl = len - 14;
    memset(buf, 0, 6);
    buf[0] = 60;                            /* rssi [?] */
    stw_le_p(buf + 4, vif & 0xF);           /* info3: vif index */
    memcpy(buf + 6, eth, 12);               /* dst, src */
    stw_be_p(buf + 18, pl + 8);             /* 802.3 length */
    buf[20] = 0xAA;
    buf[21] = 0xAA;
    buf[22] = 0x03;
    buf[23] = buf[24] = buf[25] = 0;
    memcpy(buf + 26, eth + 12, 2 + pl);     /* ethertype + payload */
    s->rx_frames++;
    trace_ar6003_net("to host", vif, len);
    ar6003_htc_send(s, EP_DATA_BE, buf, 6 + 14 + 8 + pl);
}

static void ar6003_net_out(AR6003State *s, int vif, uint8_t *eth,
                           uint32_t len);
static void ar6003_dhcp_start(AR6003State *s, int vif);

/* ---- WPA2-PSK 4-way handshake (IEEE 802.11i, key descriptor v2) ---- */

#define KI_PAIRWISE 0x0008
#define KI_INSTALL  0x0040
#define KI_ACK      0x0080
#define KI_MIC      0x0100
#define KI_SECURE   0x0200
#define KI_ENCDATA  0x1000
#define EAPOL_KEY_HDR   (4 + 95)

static bool ar6003_hmac_sha1(const uint8_t *key, size_t klen,
                             const uint8_t *d, size_t dlen, uint8_t *out20)
{
    g_autoptr(QCryptoHmac) h = NULL;
    uint8_t *res = out20;
    size_t rlen = 20;

    h = qcrypto_hmac_new(QCRYPTO_HASH_ALGO_SHA1, key, klen, NULL);
    if (!h) {
        return false;
    }
    return qcrypto_hmac_bytes(h, (const char *)d, dlen, &res, &rlen,
                              NULL) == 0;
}

static void ar6003_pmk(const char *pass, const char *ssid, uint8_t *pmk)
{
    if (qcrypto_pbkdf2(QCRYPTO_HASH_ALGO_SHA1, (const uint8_t *)pass,
                       strlen(pass), (const uint8_t *)ssid, strlen(ssid),
                       4096, pmk, 32, NULL) < 0) {
        memset(pmk, 0, 32);
    }
}

static void ar6003_ptk(AR6003Eapol *e)
{
    uint8_t d[100], out[80];
    const char *label = "Pairwise key expansion";
    size_t l = strlen(label) + 1;
    bool a_lo = memcmp(e->aa, e->spa, 6) < 0;
    bool n_lo = memcmp(e->anonce, e->snonce, 32) < 0;

    memcpy(d, label, l);
    memcpy(d + l, a_lo ? e->aa : e->spa, 6);
    memcpy(d + l + 6, a_lo ? e->spa : e->aa, 6);
    memcpy(d + l + 12, n_lo ? e->anonce : e->snonce, 32);
    memcpy(d + l + 44, n_lo ? e->snonce : e->anonce, 32);
    for (int i = 0; i < 4; i++) {
        d[l + 76] = i;
        ar6003_hmac_sha1(e->pmk, 32, d, l + 77, out + 20 * i);
    }
    memcpy(e->ptk, out, 48);
}

/* RFC 3394 AES key wrap with the KEK */
static bool ar6003_aes_wrap(const uint8_t *kek, const uint8_t *in,
                            size_t n8, uint8_t *out)
{
    g_autoptr(QCryptoCipher) c = NULL;
    uint8_t a[8], b[16];
    uint8_t *r = out + 8;

    c = qcrypto_cipher_new(QCRYPTO_CIPHER_ALGO_AES_128,
                           QCRYPTO_CIPHER_MODE_ECB, kek, 16, NULL);
    if (!c) {
        return false;
    }
    memset(a, 0xA6, 8);
    memcpy(r, in, n8 * 8);
    for (int j = 0; j < 6; j++) {
        for (size_t i = 0; i < n8; i++) {
            uint64_t t = n8 * j + i + 1;

            memcpy(b, a, 8);
            memcpy(b + 8, r + 8 * i, 8);
            if (qcrypto_cipher_encrypt(c, b, b, 16, NULL) < 0) {
                return false;
            }
            memcpy(a, b, 8);
            for (int k = 0; k < 8; k++) {
                a[7 - k] ^= (t >> (8 * k)) & 0xFF;
            }
            memcpy(r + 8 * i, b + 8, 8);
        }
    }
    memcpy(out, a, 8);
    return true;
}

/*
 * Build and send one EAPOL-Key frame from @src to @dst.  @kd/@kdlen is the
 * key data (wrapped already when KI_ENCDATA).  @mic: sign with the KCK.
 */
static void ar6003_eapol_send(AR6003State *s, int vif, AR6003Eapol *e,
                              const uint8_t *dst, const uint8_t *src,
                              uint16_t ki, const uint8_t *nonce,
                              const uint8_t *kd, uint32_t kdlen, bool to_host)
{
    uint8_t f[14 + EAPOL_KEY_HDR + 128];
    uint8_t *k = f + 14;
    uint8_t mic[20];
    uint32_t blen = 95 + kdlen;

    if (kdlen > 128) {
        return;
    }
    memset(f, 0, sizeof(f));
    memcpy(f, dst, 6);
    memcpy(f + 6, src, 6);
    stw_be_p(f + 12, ETH_P_EAPOL_);
    k[0] = 2;                               /* 802.1X-2004 */
    k[1] = 3;                               /* EAPOL-Key */
    stw_be_p(k + 2, blen);
    k[4] = 2;                               /* RSN key descriptor */
    stw_be_p(k + 5, ki | 2);                /* v2: HMAC-SHA1 / AES */
    stw_be_p(k + 7, (ki & KI_ACK) ? 16 : 0);
    memcpy(k + 9, e->replay, 8);
    if (nonce) {
        memcpy(k + 17, nonce, 32);
    }
    stw_be_p(k + 97, kdlen);
    memcpy(k + 99, kd, kdlen);
    if (ki & KI_MIC) {
        ar6003_hmac_sha1(e->ptk, 16, k, 4 + blen, mic);
        memcpy(k + 81, mic, 16);
    }
    if (to_host) {
        ar6003_data_rx(s, vif, f, 14 + 4 + blen);
    } else {
        ar6003_net_out(s, vif, f, 14 + 4 + blen);
    }
}

static bool ar6003_eapol_mic_ok(AR6003Eapol *e, const uint8_t *k,
                                uint32_t len)
{
    uint8_t tmp[512], mic[20];

    if (len > sizeof(tmp) || len < 4 + 95) {
        return false;
    }
    memcpy(tmp, k, len);
    memset(tmp + 81, 0, 16);
    ar6003_hmac_sha1(e->ptk, 16, tmp, len, mic);
    return !memcmp(mic, k + 81, 16);
}

static void ar6003_replay_inc(AR6003Eapol *e)
{
    for (int i = 7; i >= 0; i--) {
        if (++e->replay[i]) {
            break;
        }
    }
}

/* station mode: the virtual AP is the authenticator (messages 1 and 3) */
static void ar6003_auth_msg1(AR6003State *s, int vif)
{
    AR6003Vif *v = &s->vif[vif];
    AR6003Eapol *e = &v->eapol;

    ar6003_replay_inc(e);
    e->state = 1;
    trace_ar6003_eapol("authenticator sends", vif, 1);
    ar6003_eapol_send(s, vif, e, e->spa, e->aa, KI_PAIRWISE | KI_ACK,
                      e->anonce, NULL, 0, true);
}

static void ar6003_auth_rx(AR6003State *s, int vif, const uint8_t *k,
                           uint32_t len)
{
    AR6003Eapol *e = &s->vif[vif].eapol;
    uint16_t ki = lduw_be_p(k + 5);

    if (!(ki & KI_MIC) || !(ki & KI_PAIRWISE)) {
        return;
    }
    if (e->state == 1 && !(ki & KI_SECURE)) {
        /* message 2: SNonce + supplicant RSN IE */
        uint8_t kd[64], wrapped[72];
        uint8_t gtk[16];
        uint32_t n = 0;

        memcpy(e->snonce, k + 17, 32);
        ar6003_ptk(e);
        if (!ar6003_eapol_mic_ok(e, k, len)) {
            trace_ar6003_eapol("bad MIC (wrong passphrase?)", vif, 2);
            return;
        }
        trace_ar6003_eapol("authenticator got", vif, 2);
        timer_del(s->eapol_timer);
        memcpy(kd, ar6003_rsn_ie, sizeof(ar6003_rsn_ie));
        n = sizeof(ar6003_rsn_ie);
        qcrypto_random_bytes(gtk, sizeof(gtk), NULL);
        kd[n++] = 0xDD;                     /* GTK KDE */
        kd[n++] = 22;
        kd[n++] = 0x00;
        kd[n++] = 0x0F;
        kd[n++] = 0xAC;
        kd[n++] = 0x01;
        kd[n++] = 0x01;                     /* key id 1, Tx = 0 */
        kd[n++] = 0x00;
        memcpy(kd + n, gtk, 16);
        n += 16;
        kd[n++] = 0xDD;                     /* padding to 8 bytes */
        while (n % 8) {
            kd[n++] = 0;
        }
        ar6003_aes_wrap(e->ptk + 16, kd, n / 8, wrapped);
        ar6003_replay_inc(e);
        e->state = 3;
        trace_ar6003_eapol("authenticator sends", vif, 3);
        ar6003_eapol_send(s, vif, e, e->spa, e->aa,
                          KI_PAIRWISE | KI_INSTALL | KI_ACK | KI_MIC |
                          KI_SECURE | KI_ENCDATA, e->anonce, wrapped, n + 8,
                          true);
    } else if (e->state == 3 && (ki & KI_SECURE)) {
        if (!ar6003_eapol_mic_ok(e, k, len)) {
            trace_ar6003_eapol("bad MIC", vif, 4);
            return;
        }
        trace_ar6003_eapol("authenticator got", vif, 4);
        e->state = 4;
    }
}

/* AP mode: the virtual client is the supplicant (messages 2 and 4) */
static void ar6003_supp_rx(AR6003State *s, int vif, const uint8_t *k,
                           uint32_t len)
{
    AR6003Vif *v = &s->vif[vif];
    AR6003Eapol *e = &v->eapol;
    uint16_t ki = lduw_be_p(k + 5);

    if (!(ki & KI_ACK) || !(ki & KI_PAIRWISE)) {
        return;
    }
    memcpy(e->replay, k + 9, 8);
    if (!(ki & KI_MIC)) {
        /* message 1 */
        trace_ar6003_eapol("client got", vif, 1);
        memcpy(e->anonce, k + 17, 32);
        qcrypto_random_bytes(e->snonce, 32, NULL);
        ar6003_ptk(e);
        e->state = 2;
        trace_ar6003_eapol("client sends", vif, 2);
        ar6003_eapol_send(s, vif, e, e->aa, e->spa, KI_PAIRWISE | KI_MIC,
                          e->snonce, ar6003_rsn_ie, sizeof(ar6003_rsn_ie),
                          true);
    } else {
        if (e->state < 2 || !ar6003_eapol_mic_ok(e, k, len)) {
            trace_ar6003_eapol("client: bad MIC (wrong passphrase?)", vif, 3);
            return;
        }
        trace_ar6003_eapol("client got", vif, 3);
        e->state = 4;
        trace_ar6003_eapol("client sends", vif, 4);
        ar6003_eapol_send(s, vif, e, e->aa, e->spa,
                          KI_PAIRWISE | KI_MIC | KI_SECURE, NULL, NULL, 0,
                          true);
        ar6003_dhcp_start(s, vif);
    }
}

static void ar6003_disconnect_evt(AR6003State *s, int vif,
                                  const uint8_t *bssid, uint8_t reason,
                                  uint16_t proto);

static void ar6003_eapol_cb(void *opaque)
{
    AR6003State *s = opaque;
    AR6003Vif *v = &s->vif[0];

    if (!v->connected || v->ap_mode || v->eapol.state != 1) {
        return;
    }
    if (v->eapol.retries++ < 4) {
        ar6003_auth_msg1(s, 0);
        timer_mod(s->eapol_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND);
        return;
    }
    /* no valid message 2 (wrong passphrase): the AP deauthenticates */
    v->connected = false;
    ar6003_disconnect_evt(s, 0, v->bssid, 4, 15);   /* 4-way timeout */
}

/* ---- virtual air: station mode ---- */

static void ar6003_scan_done(void *opaque)
{
    AR6003State *s = opaque;
    int vif = s->scan_vif;
    uint8_t ev[16 + 300];
    uint8_t st[4];

    for (int i = 0; i < s->nap; i++) {
        AR6003AP *ap = &s->ap[i];
        uint32_t n;

        stw_le_p(ev, ar6003_ch2freq(ap->ch));
        ev[2] = 1;                          /* beacon */
        ev[3] = 50;                         /* snr: -45 dBm */
        memcpy(ev + 4, ap->bssid, 6);
        stw_le_p(ev + 10, 0);
        memset(ev + 12, 0, 8);              /* timestamp */
        stw_le_p(ev + 20, 100);             /* beacon interval */
        stw_le_p(ev + 22, ar6003_ap_capab(s, i));
        n = 24 + ar6003_ap_ies(s, i, ev + 24);
        trace_ar6003_air("scan result", vif, i);
        ar6003_wmi_event(s, vif, WMI_BSSINFO_EVENTID, ev, n);
    }
    stl_le_p(st, 0);
    ar6003_wmi_event(s, vif, WMI_SCAN_COMPLETE_EVENTID, st, 4);
}

static void ar6003_disconnect_evt(AR6003State *s, int vif,
                                  const uint8_t *bssid, uint8_t reason,
                                  uint16_t proto)
{
    uint8_t ev[10];

    stw_le_p(ev, proto);
    memcpy(ev + 2, bssid, 6);
    ev[8] = reason;
    ev[9] = 0;
    trace_ar6003_air("disconnect", vif, reason);
    ar6003_wmi_event(s, vif, WMI_DISCONNECT_EVENTID, ev, sizeof(ev));
}

static void ar6003_sta_connect(AR6003State *s, int vif, const uint8_t *p,
                               uint32_t len)
{
    AR6003Vif *v = &s->vif[vif];
    static const uint8_t zero[6];
    char ssid[33] = "";
    uint8_t ev[22 + 512];
    uint32_t n, bl, rq;
    int idx = -1;

    if (len < 53) {
        return;
    }
    memcpy(ssid, p + 8, MIN(p[7], 32));
    for (int i = 0; i < s->nap; i++) {
        if (!strcmp(s->ap[i].ssid, ssid) &&
            (!memcmp(p + 42, zero, 6) || !memcmp(p + 42, s->ap[i].bssid, 6))) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        trace_ar6003_air("connect: no such network", vif, 0);
        ar6003_disconnect_evt(s, vif, p + 42, 1, 0);   /* NO_NETWORK_AVAIL */
        return;
    }
    v->connected = true;
    v->ap_mode = false;
    v->ap_idx = idx;
    memcpy(v->bssid, s->ap[idx].bssid, 6);
    v->freq = ar6003_ch2freq(s->ap[idx].ch);
    memset(&v->eapol, 0, sizeof(v->eapol));

    /* union: ch, bssid, listen_intvl, beacon_intvl, nw_type */
    memset(ev, 0, sizeof(ev));
    stw_le_p(ev, v->freq);
    memcpy(ev + 2, v->bssid, 6);
    stw_le_p(ev + 8, 100);
    stw_le_p(ev + 10, 100);
    stl_le_p(ev + 12, WMI_NW_INFRA);
    /* beacon IEs, assoc request (cap, listen, IEs), assoc response */
    n = 22;
    bl = ar6003_ap_ies(s, idx, ev + n);
    n += bl;
    stw_le_p(ev + n, 0x0401);
    stw_le_p(ev + n + 2, 10);
    rq = 4;
    if (s->ap[idx].psk[0]) {
        memcpy(ev + n + rq, ar6003_rsn_ie, sizeof(ar6003_rsn_ie));
        rq += sizeof(ar6003_rsn_ie);
    }
    n += rq;
    stw_le_p(ev + n, ar6003_ap_capab(s, idx));
    stw_le_p(ev + n + 2, 0);                /* status: success */
    stw_le_p(ev + n + 4, 0xC001);           /* AID 1 */
    n += 6;
    stw_le_p(ev + 16, bl);
    stw_le_p(ev + 18, rq);
    stw_le_p(ev + 20, 6);
    trace_ar6003_air("connected to", vif, idx);
    ar6003_wmi_event(s, vif, WMI_CONNECT_EVENTID, ev, n);

    if (s->ap[idx].psk[0]) {
        AR6003Eapol *e = &v->eapol;

        memcpy(e->pmk, s->ap[idx].pmk, 32);
        memcpy(e->aa, v->bssid, 6);
        ar6003_vif_mac(s, vif, e->spa);
        qcrypto_random_bytes(e->anonce, 32, NULL);
        e->retries = 0;
        e->state = 1;
        /* message 1 from the timer, after the host saw the connect */
        timer_mod(s->eapol_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100 * SCALE_MS);
    }
}

/* ---- virtual air: AP mode (camera's BSS) ---- */

static void ar6003_client_try(AR6003State *s);

static void ar6003_ap_commit(AR6003State *s, int vif, const uint8_t *p,
                             uint32_t len)
{
    AR6003Vif *v = &s->vif[vif];
    uint8_t ev[22];

    if (len < 53) {
        return;
    }
    memset(v->ssid, 0, sizeof(v->ssid));
    memcpy(v->ssid, p + 8, MIN(p[7], 32));
    v->crypto = p[3];
    v->freq = lduw_le_p(p + 40);
    if (!v->freq) {
        v->freq = ar6003_ch2freq(6);
    }
    v->ap_mode = true;
    v->connected = true;
    v->client_joined = false;
    ar6003_vif_mac(s, vif, v->bssid);
    memset(ev, 0, sizeof(ev));
    stw_le_p(ev, v->freq);
    memcpy(ev + 2, v->bssid, 6);
    trace_ar6003_air("AP started", vif, ar6003_freq2ch(v->freq));
    qemu_log_mask(LOG_UNIMP, "ar6003: AP up on vif %d: SSID '%s' channel %d "
                  "crypto %u\n", vif, v->ssid, ar6003_freq2ch(v->freq),
                  v->crypto);
    ar6003_wmi_event(s, vif, WMI_CONNECT_EVENTID, ev, sizeof(ev));
    if (s->client && !strcmp(s->client, "auto")) {
        timer_mod(s->client_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 2 * NANOSECONDS_PER_SECOND);
    } else if (s->client_join_ssid[0] || s->client_join_psk[0]) {
        timer_mod(s->client_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 500 * SCALE_MS);
    }
}

static void ar6003_client_join_vif(AR6003State *s, int vif, const char *psk)
{
    AR6003Vif *v = &s->vif[vif];
    AR6003Eapol *e = &v->eapol;
    bool secure = v->crypto > WMI_CRYPTO_NONE;
    uint8_t ev[22 + 160];
    uint32_t n = 22, rq;
    size_t sl = strlen(v->ssid);

    memset(ev, 0, sizeof(ev));
    ev[0] = 3;                              /* phymode 11g [?] */
    ev[1] = 1;                              /* aid */
    memcpy(ev + 2, s->client_mac, 6);
    ev[8] = 1;                              /* auth: open */
    ev[9] = secure ? 0x10 : 0x01;           /* keymgmt: WPA2_PSK / NONE */
    stw_le_p(ev + 10, secure ? 0x08 : 0x01);/* cipher: AES / NONE */
    ev[12] = 0;
    /*
     * assoc request as the whole management frame
     * (ath6kl_connect_ap_mode_sta @ 0xd028 takes the IEs at +28):
     * 802.11 header, capability, listen interval, SSID, rates, RSN
     */
    rq = 0;
    stw_le_p(ev + n, 0x0000);               /* FC: assoc request */
    stw_le_p(ev + n + 2, 0);
    memcpy(ev + n + 4, v->bssid, 6);
    memcpy(ev + n + 10, s->client_mac, 6);
    memcpy(ev + n + 16, v->bssid, 6);
    stw_le_p(ev + n + 22, 0);
    rq = 24;
    stw_le_p(ev + n + rq, secure ? 0x0411 : 0x0401);
    stw_le_p(ev + n + rq + 2, 10);
    rq += 4;
    ev[n + rq++] = 0;
    ev[n + rq++] = sl;
    memcpy(ev + n + rq, v->ssid, sl);
    rq += sl;
    memcpy(ev + n + rq, "\x01\x08\x82\x84\x8b\x96\x0c\x12\x18\x24", 10);
    rq += 10;
    if (secure) {
        memcpy(ev + n + rq, ar6003_rsn_ie, sizeof(ar6003_rsn_ie));
        rq += sizeof(ar6003_rsn_ie);
    }
    n += rq;
    stw_le_p(ev + 16, 0);
    stw_le_p(ev + 18, rq);
    stw_le_p(ev + 20, 0);
    memset(e, 0, sizeof(*e));
    if (secure) {
        ar6003_pmk(psk, v->ssid, e->pmk);
        memcpy(e->aa, v->bssid, 6);
        memcpy(e->spa, s->client_mac, 6);
    }
    v->client_joined = true;
    trace_ar6003_air("client joins", vif, secure);
    ar6003_wmi_event(s, vif, WMI_CONNECT_EVENTID, ev, n);
    if (!secure) {
        ar6003_dhcp_start(s, vif);
    }
}

static void ar6003_client_try(AR6003State *s)
{
    const char *ssid = s->client_join_ssid;
    const char *psk = s->client_join_psk[0] ? s->client_join_psk :
                      (s->client_psk ? s->client_psk : "");

    for (int i = 0; i < 2; i++) {
        AR6003Vif *v = &s->vif[i];

        if (v->ap_mode && !v->client_joined &&
            (!ssid[0] || !strcmp(ssid, v->ssid))) {
            ar6003_client_join_vif(s, i, psk);
            return;
        }
    }
}

static void ar6003_client_cb(void *opaque)
{
    ar6003_client_try(opaque);
}

/* ---- P2P helpers (remain on channel, action frames) ---- */

#define WMI_REMAIN_ON_CHNL_EVENTID      0x9007

static void ar6003_roc_done(void *opaque)
{
    AR6003State *s = opaque;
    uint8_t ev[9];

    stl_le_p(ev, s->roc_freq);
    stl_le_p(ev + 4, s->roc_dur);
    ev[8] = 0;
    ar6003_wmi_event(s, s->roc_vif, WMI_CANCEL_REMAIN_ON_CHNL_EVENTID, ev,
                     sizeof(ev));
}

static void ar6003_roc(AR6003State *s, int vif, const uint8_t *p,
                       uint32_t len)
{
    uint8_t ev[8];

    if (len < 8) {
        return;
    }
    s->roc_vif = vif;
    s->roc_freq = ldl_le_p(p);
    s->roc_dur = ldl_le_p(p + 4);
    memcpy(ev, p, 8);
    ar6003_wmi_event(s, vif, WMI_REMAIN_ON_CHNL_EVENTID, ev, sizeof(ev));
    timer_mod(s->roc_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              (int64_t)MIN(s->roc_dur, 5000) * SCALE_MS);
}

/* ---- virtual client: DHCP against the camera's dhcpd (AP mode) ---- */

static uint16_t ar6003_ip_csum(const uint8_t *p, int n)
{
    uint32_t sum = 0;

    for (int i = 0; i < n; i += 2) {
        sum += (p[i] << 8) | p[i + 1];
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return ~sum;
}

static void ar6003_dhcp_send(AR6003State *s)
{
    uint8_t f[14 + 20 + 8 + 300];
    uint8_t *ip = f + 14, *udp = ip + 20, *b = udp + 8, *o;
    int vif = s->dhcp_vif;
    uint32_t blen;

    memset(f, 0, sizeof(f));
    memset(f, 0xFF, 6);
    memcpy(f + 6, s->client_mac, 6);
    stw_be_p(f + 12, ETH_P_IP);
    b[0] = 1;                               /* BOOTREQUEST */
    b[1] = 1;
    b[2] = 6;
    stl_be_p(b + 4, s->dhcp_xid);
    stw_be_p(b + 10, 0x8000);               /* broadcast replies */
    memcpy(b + 28, s->client_mac, 6);
    stl_be_p(b + 236, 0x63825363);
    o = b + 240;
    *o++ = 53;
    *o++ = 1;
    *o++ = s->dhcp_state;                   /* 1 DISCOVER / 3 REQUEST */
    if (s->dhcp_state == 3) {
        *o++ = 50;
        *o++ = 4;
        memcpy(o, s->dhcp_offer, 4);
        o += 4;
        *o++ = 54;
        *o++ = 4;
        memcpy(o, s->dhcp_server, 4);
        o += 4;
    }
    *o++ = 12;                              /* host name */
    *o++ = 6;
    memcpy(o, "a6kcli", 6);
    o += 6;
    *o++ = 55;
    *o++ = 3;
    *o++ = 1;
    *o++ = 3;
    *o++ = 6;
    *o++ = 255;
    blen = MAX(o - b, 300);
    stw_be_p(udp, 68);
    stw_be_p(udp + 2, 67);
    stw_be_p(udp + 4, 8 + blen);
    ip[0] = 0x45;
    stw_be_p(ip + 2, 20 + 8 + blen);
    ip[8] = 64;
    ip[9] = 17;
    memset(ip + 16, 0xFF, 4);
    stw_be_p(ip + 10, ar6003_ip_csum(ip, 20));
    trace_ar6003_air("client DHCP sends", vif, s->dhcp_state);
    ar6003_data_rx(s, vif, f, 14 + 20 + 8 + blen);
}

static void ar6003_dhcp_cb(void *opaque)
{
    AR6003State *s = opaque;
    AR6003Vif *v = &s->vif[s->dhcp_vif];

    if (!v->ap_mode || !v->client_joined || s->dhcp_state == 5) {
        return;
    }
    if (s->dhcp_tries++ >= 6) {
        s->dhcp_state = 1;                  /* start over */
        s->dhcp_tries = 0;
    }
    ar6003_dhcp_send(s);
    timer_mod(s->dhcp_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              3 * NANOSECONDS_PER_SECOND);
}

static void ar6003_dhcp_start(AR6003State *s, int vif)
{
    s->dhcp_vif = vif;
    s->dhcp_state = 1;
    s->dhcp_tries = 0;
    s->dhcp_xid = 0xA6000000 | (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) &
                                0xFFFFFF);
    timer_mod(s->dhcp_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              300 * SCALE_MS);
}

/* a frame from the camera to the virtual client: DHCP replies stay here */
static bool ar6003_dhcp_rx(AR6003State *s, const uint8_t *eth, uint32_t len)
{
    const uint8_t *ip = eth + 14, *udp, *b, *o, *end;
    int type = 0;
    uint8_t server[4] = { 0 };

    if (len < 14 + 20 + 8 + 240 || lduw_be_p(eth + 12) != ETH_P_IP ||
        ip[9] != 17) {
        return false;
    }
    udp = ip + (ip[0] & 0xF) * 4;
    if (udp + 8 + 240 > eth + len || lduw_be_p(udp + 2) != 68) {
        return false;
    }
    b = udp + 8;
    if (b[0] != 2 || ldl_be_p(b + 4) != s->dhcp_xid ||
        memcmp(b + 28, s->client_mac, 6)) {
        return true;                        /* someone else's: drop */
    }
    end = eth + len;
    for (o = b + 240; o + 2 <= end && *o != 255; o += (*o ? o[1] + 2 : 1)) {
        if (*o == 53 && o[1] >= 1) {
            type = o[2];
        } else if (*o == 54 && o[1] >= 4) {
            memcpy(server, o + 2, 4);
        }
    }
    if (type == 2 && s->dhcp_state == 1) {          /* OFFER */
        memcpy(s->dhcp_offer, b + 16, 4);
        memcpy(s->dhcp_server, server, 4);
        s->dhcp_state = 3;
        s->dhcp_tries = 0;
        ar6003_dhcp_send(s);
    } else if (type == 5 && s->dhcp_state == 3) {   /* ACK */
        memcpy(s->client_ip, b + 16, 4);
        s->dhcp_state = 5;
        timer_del(s->dhcp_timer);
        qemu_log_mask(LOG_UNIMP, "ar6003: virtual client %d.%d.%d.%d "
                      "(lease from %d.%d.%d.%d)\n", s->client_ip[0],
                      s->client_ip[1], s->client_ip[2], s->client_ip[3],
                      server[0], server[1], server[2], server[3]);
        trace_ar6003_air("client DHCP bound", s->dhcp_vif, s->client_ip[3]);
    } else if (type == 6) {                         /* NAK */
        s->dhcp_state = 1;
    }
    return true;
}

/* ---- WMI commands ---- */

static void ar6003_wmi_cmd(AR6003State *s, const uint8_t *p, uint32_t len)
{
    uint16_t id;
    int vif;

    if (len < 6) {
        qemu_log_mask(LOG_GUEST_ERROR, "ar6003: short WMI command\n");
        return;
    }
    id = lduw_le_p(p);
    vif = lduw_le_p(p + 2) & 0xF;
    trace_ar6003_wmi_cmd(vif, id, len - 6);
    p += 6;
    len -= 6;
    if (vif > 1) {
        qemu_log_mask(LOG_GUEST_ERROR, "ar6003: WMI for vif %d\n", vif);
        return;
    }
    switch (id) {
    case WMI_START_SCAN_CMDID:
    case WMI_BEGIN_SCAN_CMDID:
        s->scan_vif = vif;
        timer_mod(s->scan_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 60 * SCALE_MS);
        break;
    case WMI_CONNECT_CMDID:
        ar6003_sta_connect(s, vif, p, len);
        break;
    case WMI_RECONNECT_CMDID:
        break;
    case WMI_DISCONNECT_CMDID:
        if (s->vif[vif].connected) {
            AR6003Vif *v = &s->vif[vif];

            timer_del(s->eapol_timer);
            v->connected = false;
            v->ap_mode = false;
            v->client_joined = false;
            ar6003_disconnect_evt(s, vif, v->bssid, 3, 0);  /* CMD */
        }
        break;
    case WMI_AP_CONFIG_COMMIT_CMDID:
        ar6003_ap_commit(s, vif, p, len);
        break;
    case WMI_REMAIN_ON_CHNL_CMDID:
        ar6003_roc(s, vif, p, len);
        break;
    case WMI_CANCEL_REMAIN_ON_CHNL_CMDID:
        if (timer_pending(s->roc_timer)) {
            timer_del(s->roc_timer);
            ar6003_roc_done(s);
        }
        break;
    case WMI_SEND_ACTION_CMDID:
    case WMI_SEND_MGMT_CMDID:
        /* no P2P peer in the air: not acknowledged */
        if (len >= 4) {
            uint8_t ev[5];

            memcpy(ev, p, 4);
            ev[4] = 0;
            ar6003_wmi_event(s, vif, WMI_TX_STATUS_EVENTID, ev, sizeof(ev));
        }
        break;
    case WMI_AP_SET_MLME_CMDID:
        /* deauth / disassoc of the virtual client */
        if (len >= 9 && s->vif[vif].client_joined &&
            !memcmp(p, s->client_mac, 6)) {
            s->vif[vif].client_joined = false;
            ar6003_disconnect_evt(s, vif, s->client_mac, 4, lduw_le_p(p + 6));
        }
        break;
    default:
        /* accepted: configuration with no event in reply */
        break;
    }
}

static void ar6003_data_tx(AR6003State *s, int eid, const uint8_t *p,
                           uint32_t len)
{
    uint8_t eth[1600];
    uint32_t off = 6, pl;
    uint16_t info2;
    int vif;

    if (len < 6) {
        return;
    }
    info2 = lduw_le_p(p + 2);
    vif = lduw_le_p(p + 4) & 0xF;
    if (p[1] & 0x03) {                      /* not a data message */
        return;
    }
    if ((p[1] >> 6) != 0) {
        qemu_log_mask(LOG_UNIMP, "ar6003: TX data type %d\n", p[1] >> 6);
        return;
    }
    if (info2 >> 13) {
        off += 12;                          /* WMI_MAX_TX_META_SZ */
    }
    /* 802.3 + LLC/SNAP (ath6kl_wmi_dix_2_dot3) -> Ethernet II */
    if (len < off + 14 + 8 || vif > 1) {
        return;
    }
    p += off;
    len -= off;
    pl = len - 22;
    if (lduw_be_p(p + 12) >= 8 && lduw_be_p(p + 12) - 8 < pl) {
        pl = lduw_be_p(p + 12) - 8;         /* 802.3 length field */
    }
    if (pl + 14 > sizeof(eth)) {
        return;
    }
    memcpy(eth, p, 12);
    memcpy(eth + 12, p + 20, 2 + pl);
    s->tx_frames++;
    trace_ar6003_net("from host", vif, 14 + pl);
    if (lduw_be_p(eth + 12) == ETH_P_EAPOL_ && 14 + pl >= 14 + 4 + 95) {
        AR6003Vif *v = &s->vif[vif];

        if (v->ap_mode) {
            if (v->client_joined && !memcmp(eth, s->client_mac, 6)) {
                ar6003_supp_rx(s, vif, eth + 14, pl);
            }
        } else if (v->connected) {
            ar6003_auth_rx(s, vif, eth + 14, pl);
        }
        return;                             /* never on the wire */
    }
    if (s->vif[vif].ap_mode && s->dhcp_state && s->dhcp_state != 5 &&
        (!memcmp(eth, s->client_mac, 6) || (eth[0] & 1)) &&
        ar6003_dhcp_rx(s, eth, 14 + pl)) {
        return;
    }
    ar6003_net_out(s, vif, eth, 14 + pl);
}

/* ---- HTC host->target ---- */

static void ar6003_htc_ctrl(AR6003State *s, const uint8_t *p, uint32_t len)
{
    uint16_t id;

    if (len < 2) {
        return;
    }
    id = lduw_le_p(p);
    switch (id) {
    case HTC_MSG_CONN_SVC: {
        uint16_t svc = len >= 4 ? lduw_le_p(p + 2) : 0;
        uint8_t r[10];

        stw_le_p(r, HTC_MSG_CONN_SVC_RESP);
        stw_le_p(r + 2, svc);
        if (svc >= 0x100 && svc <= 0x104) {
            r[4] = 0;                       /* HTC_SERVICE_SUCCESS */
            r[5] = EP_WMI + (svc - 0x100);
        } else {
            r[4] = 1;                       /* HTC_SERVICE_NOT_FOUND */
            r[5] = 0;
        }
        stw_le_p(r + 6, HTC_MAX_MSG);
        r[8] = 0;
        r[9] = 0;
        trace_ar6003_htc_svc(svc, r[5]);
        ar6003_htc_send(s, EP_CTRL, r, sizeof(r));
        break;
    }
    case HTC_MSG_SETUP_COMPLETE:
    case HTC_MSG_SETUP_COMPLETE_EX:
        s->setup_done = true;
        s->phase = AR6003_PH_WMI;
        /* the firmware's WMI layer comes up: READY shortly after */
        timer_mod(s->fw_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 2 * SCALE_MS);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "ar6003: HTC control message %u\n", id);
    }
}

static void ar6003_htc_rx(AR6003State *s, const uint8_t *buf, uint32_t len)
{
    int eid;
    uint32_t plen;

    if (len < HTC_HDR_LEN) {
        qemu_log_mask(LOG_GUEST_ERROR, "ar6003: short HTC frame %u\n", len);
        return;
    }
    eid = buf[0];
    plen = lduw_le_p(buf + 2);
    if (plen + HTC_HDR_LEN > len) {
        qemu_log_mask(LOG_GUEST_ERROR, "ar6003: HTC len %u > frame %u\n",
                      plen, len);
        return;
    }
    trace_ar6003_htc_rx(eid, buf[1], plen);
    buf += HTC_HDR_LEN;
    if (eid == EP_CTRL) {
        ar6003_htc_ctrl(s, buf, plen);
        return;
    }
    if (eid < 8) {
        s->credits_pending[eid]++;
    }
    if (eid == EP_WMI) {
        ar6003_wmi_cmd(s, buf, plen);
    } else if (eid >= EP_DATA_BE && eid <= EP_DATA_BE + 3) {
        ar6003_data_tx(s, eid, buf, plen);
    } else {
        qemu_log_mask(LOG_GUEST_ERROR, "ar6003: HTC frame for EP %d\n", eid);
    }
    ar6003_credit_kick(s);
}

/* ---- BMI ---- */

enum {
    BMI_DONE = 1, BMI_READ_MEMORY, BMI_WRITE_MEMORY, BMI_EXECUTE,
    BMI_SET_APP_START, BMI_READ_SOC_REGISTER, BMI_WRITE_SOC_REGISTER,
    BMI_GET_TARGET_INFO, BMI_ROMPATCH_INSTALL, BMI_ROMPATCH_UNINSTALL,
    BMI_ROMPATCH_ACTIVATE, BMI_ROMPATCH_DEACTIVATE, BMI_LZ_STREAM_START,
    BMI_LZ_DATA,
};

static void ar6003_bmi_reply(AR6003State *s, const uint8_t *b, uint32_t len)
{
    ar6003_rx_push(s, b, len, len);         /* raw, unpadded */
}

static void ar6003_bmi(AR6003State *s, const uint8_t *p, uint32_t len)
{
    uint32_t cmd, a = 0, n = 0;
    uint8_t r[16];

    if (len < 4) {
        return;
    }
    cmd = ldl_le_p(p);
    if (len >= 8) {
        a = ldl_le_p(p + 4);
    }
    if (len >= 12) {
        n = ldl_le_p(p + 8);
    }
    trace_ar6003_bmi(cmd, a, n);
    switch (cmd) {
    case BMI_DONE:
        s->phase = AR6003_PH_BOOT;
        qemu_log_mask(LOG_UNIMP, "ar6003: BMI DONE (%u image bytes, %u LZ "
                      "bytes): firmware HLE starts\n", s->bmi_bytes,
                      s->lz_bytes);
        timer_mod(s->fw_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  s->ready_delay_ms * SCALE_MS);
        break;
    case BMI_READ_MEMORY: {
        g_autofree uint8_t *d = g_malloc0(n ? MIN(n, 0x1000) : 1);

        n = MIN(n, 0x1000);
        ar6003_mem_read(s, a, d, n);
        ar6003_bmi_reply(s, d, n);
        break;
    }
    case BMI_WRITE_MEMORY:
        n = MIN(n, len - 12);
        ar6003_mem_write(s, a, p + 12, n);
        s->bmi_bytes += n;
        break;
    case BMI_EXECUTE:
        /* OTP image: cannot run (no Xtensa core), report success */
        stl_le_p(r, 0);
        ar6003_bmi_reply(s, r, 4);
        break;
    case BMI_SET_APP_START:
        break;
    case BMI_READ_SOC_REGISTER:
        stl_le_p(r, ar6003_fw_mem_read32(s, a));
        ar6003_bmi_reply(s, r, 4);
        break;
    case BMI_WRITE_SOC_REGISTER:
        ar6003_fw_mem_write32(s, a, n);
        break;
    case BMI_GET_TARGET_INFO:
        /* ath6kl_bmi_get_target_info @ 0x404c: sentinel, count 12 */
        stl_le_p(r, 0xFFFFFFFF);
        stl_le_p(r + 4, 12);
        stl_le_p(r + 8, AR6003_TARGET_VERSION);
        stl_le_p(r + 12, AR6003_TARGET_TYPE);
        ar6003_bmi_reply(s, r, 16);
        break;
    case BMI_LZ_STREAM_START:
        s->lz_addr = a;
        break;
    case BMI_LZ_DATA:
        /* a = length here; compressed image data is discarded */
        s->lz_bytes += MIN(a, len - 8);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "ar6003: BMI command %u\n", cmd);
    }
    s->count[4] = 1;                        /* next command credit */
}

void ar6003_fw_mbox_msg(AR6003State *s, const uint8_t *buf, uint32_t len)
{
    switch (s->phase) {
    case AR6003_PH_BMI:
        ar6003_bmi(s, buf, len);
        break;
    case AR6003_PH_HTC:
    case AR6003_PH_WMI:
        ar6003_htc_rx(s, buf, len);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "ar6003: mailbox message (%u bytes) "
                      "in phase %d\n", len, s->phase);
    }
}

static void ar6003_fw_tick(void *opaque)
{
    AR6003State *s = opaque;
    uint8_t r[10];

    switch (s->phase) {
    case AR6003_PH_BOOT:
        /* HTC READY: credits, credit size, max EP, HTC 2.1, no bundling */
        stw_le_p(r, HTC_MSG_READY);
        stw_le_p(r + 2, HTC_CREDITS);
        stw_le_p(r + 4, HTC_CREDIT_SIZE);
        r[6] = 8;
        r[7] = 0;
        r[8] = 1;
        r[9] = 0;
        s->phase = AR6003_PH_HTC;
        ar6003_htc_send(s, EP_CTRL, r, sizeof(r));
        break;
    case AR6003_PH_WMI:
        if (!s->wmi_ready_sent) {
            ar6003_wmi_ready(s);
        }
        break;
    }
}

/* ---- netdev bridge ---- */

static bool ar6003_is_mcast(const uint8_t *mac)
{
    return mac[0] & 1;
}

/*
 * AP mode: the netdev peer stands in for the virtual client at L2.  Frames
 * from the peer get the client's MAC as source (Ethernet and ARP sender),
 * frames from the camera to the client get the peer's MAC.
 */
static void ar6003_arp_rewrite(uint8_t *eth, uint32_t len,
                               const uint8_t *from, const uint8_t *to)
{
    if (len >= 42 && lduw_be_p(eth + 12) == ETH_P_ARP) {
        if (!memcmp(eth + 22, from, 6)) {
            memcpy(eth + 22, to, 6);        /* sender hw address */
        }
        if (!memcmp(eth + 32, from, 6)) {
            memcpy(eth + 32, to, 6);        /* target hw address */
        }
    }
}

static void ar6003_net_out(AR6003State *s, int vif, uint8_t *eth,
                           uint32_t len)
{
    AR6003Vif *v = &s->vif[vif];

    if (!v->connected) {
        s->dropped++;
        return;
    }
    if (v->ap_mode) {
        if (!v->client_joined || (v->eapol.state != 4 && v->crypto > 1)) {
            s->dropped++;
            return;
        }
        if (!memcmp(eth, s->client_mac, 6) && s->peer_valid) {
            memcpy(eth, s->peer_mac, 6);
        }
        if (s->peer_valid) {
            ar6003_arp_rewrite(eth, len, s->client_mac, s->peer_mac);
        }
    }
    trace_ar6003_net("to netdev", vif, len);
    qemu_send_packet(qemu_get_queue(s->nic), eth, len);
}

static ssize_t ar6003_nc_receive(NetClientState *nc, const uint8_t *buf,
                                 size_t size)
{
    AR6003State *s = qemu_get_nic_opaque(nc);
    uint8_t eth[1600];

    /* the air is lossy: frames nobody can take now are dropped */
    if (size < 14 || size > sizeof(eth) || s->phase != AR6003_PH_WMI) {
        s->dropped++;
        return size;
    }
    memcpy(eth, buf, size);
    if (lduw_be_p(eth + 12) == ETH_P_EAPOL_) {
        return size;                        /* the air's own business */
    }
    for (int i = 0; i < 2; i++) {
        AR6003Vif *v = &s->vif[i];
        uint8_t mac[6];

        if (!v->connected) {
            continue;
        }
        ar6003_vif_mac(s, i, mac);
        if (v->ap_mode) {
            if (!v->client_joined ||
                (v->crypto > 1 && v->eapol.state != 4)) {
                continue;
            }
            if (!s->peer_valid || memcmp(s->peer_mac, eth + 6, 6)) {
                memcpy(s->peer_mac, eth + 6, 6);
                s->peer_valid = true;
            }
            memcpy(eth + 6, s->client_mac, 6);
            ar6003_arp_rewrite(eth, size, s->peer_mac, s->client_mac);
        } else if (v->eapol.state != 0 && v->eapol.state != 4) {
            continue;                       /* port not yet authorized */
        }
        if (ar6003_is_mcast(eth) || !memcmp(eth, mac, 6)) {
            ar6003_data_rx(s, i, eth, size);
            return size;
        }
    }
    s->dropped++;
    return size;
}

static NetClientInfo ar6003_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .receive = ar6003_nc_receive,
};

/* ---- setup ---- */

static void ar6003_parse_aps(AR6003State *s, Error **errp)
{
    const char *spec = s->aps ? s->aps : "ssid=QEMU-AP,ch=6";
    g_auto(GStrv) aps = g_strsplit(spec, ";", AR6003_MAX_APS + 1);

    s->nap = 0;
    for (int i = 0; aps[i] && s->nap < AR6003_MAX_APS; i++) {
        AR6003AP *ap = &s->ap[s->nap];
        g_auto(GStrv) kv = g_strsplit(aps[i], ",", 0);

        if (!*aps[i]) {
            continue;
        }
        memset(ap, 0, sizeof(*ap));
        ap->ch = 6;
        ap->bssid[0] = 0x02;
        ap->bssid[1] = 0xa5;
        ap->bssid[2] = 0x4b;
        ap->bssid[3] = 0x00;
        ap->bssid[4] = 0x01;
        ap->bssid[5] = s->nap + 1;
        for (int j = 0; kv[j]; j++) {
            const char *v = strchr(kv[j], '=');
            uint64_t num;

            if (!v) {
                error_setg(errp, "ar6003 aps: expected key=value in '%s'",
                           kv[j]);
                return;
            }
            v++;
            if (g_str_has_prefix(kv[j], "ssid=")) {
                if (strlen(v) > 32) {
                    error_setg(errp, "ar6003 aps: SSID longer than 32");
                    return;
                }
                pstrcpy(ap->ssid, sizeof(ap->ssid), v);
            } else if (g_str_has_prefix(kv[j], "psk=")) {
                if (strlen(v) < 8 || strlen(v) > 63) {
                    error_setg(errp, "ar6003 aps: passphrase must be 8..63 "
                               "characters");
                    return;
                }
                pstrcpy(ap->psk, sizeof(ap->psk), v);
            } else if (g_str_has_prefix(kv[j], "ch=")) {
                if (qemu_strtou64(v, NULL, 0, &num) || num < 1 || num > 13) {
                    error_setg(errp, "ar6003 aps: ch must be 1..13");
                    return;
                }
                ap->ch = num;
            } else if (g_str_has_prefix(kv[j], "bssid=")) {
                unsigned int m[6];

                if (sscanf(v, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2],
                           &m[3], &m[4], &m[5]) != 6) {
                    error_setg(errp, "ar6003 aps: bad bssid '%s'", v);
                    return;
                }
                for (int k = 0; k < 6; k++) {
                    ap->bssid[k] = m[k];
                }
            } else {
                error_setg(errp, "ar6003 aps: unknown key '%s'", kv[j]);
                return;
            }
        }
        if (!ap->ssid[0]) {
            error_setg(errp, "ar6003 aps: entry without ssid=");
            return;
        }
        s->nap++;
    }
}

void ar6003_fw_client_join(AR6003State *s, const char *spec, Error **errp)
{
    g_auto(GStrv) kv = g_strsplit(spec, ",", 2);

    if (!kv[0] || !kv[1] || strlen(kv[0]) > 32 || strlen(kv[1]) > 63 ||
        (kv[1][0] && strlen(kv[1]) < 8)) {
        error_setg(errp, "client-join: expected \"SSID,PASSPHRASE\" (SSID may "
                   "be empty = the camera's BSS; passphrase 8..63 or empty)");
        return;
    }
    pstrcpy(s->client_join_ssid, sizeof(s->client_join_ssid), kv[0]);
    pstrcpy(s->client_join_psk, sizeof(s->client_join_psk), kv[1]);
    ar6003_client_try(s);
}

void ar6003_fw_init(AR6003State *s, Error **errp)
{
    static const uint8_t def_mac[6] = { 0x02, 0xa5, 0x4b, 0x00, 0x00, 0x01 };
    static const MACAddr zero;

    ar6003_parse_aps(s, errp);
    if (*errp) {
        return;
    }
    if (!memcmp(&s->conf.macaddr, &zero, sizeof(zero))) {
        memcpy(s->conf.macaddr.a, def_mac, 6);
    }
    memcpy(s->mac, s->conf.macaddr.a, 6);
    s->client_mac[0] = 0x02;
    s->client_mac[1] = 0xa5;
    s->client_mac[2] = 0x4b;
    s->client_mac[3] = 0x00;
    s->client_mac[4] = 0x02;
    s->client_mac[5] = 0x01;
    s->mem = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                                   g_free);
    s->fw_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ar6003_fw_tick, s);
    s->credit_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ar6003_credit_cb, s);
    s->scan_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ar6003_scan_done, s);
    s->client_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ar6003_client_cb, s);
    s->eapol_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ar6003_eapol_cb, s);
    s->roc_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ar6003_roc_done, s);
    s->dhcp_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ar6003_dhcp_cb, s);
    for (int i = 0; i < s->nap; i++) {
        if (s->ap[i].psk[0]) {
            ar6003_pmk(s->ap[i].psk, s->ap[i].ssid, s->ap[i].pmk);
        }
    }
    s->nic = qemu_new_nic(&ar6003_net_info, &s->conf,
                          object_get_typename(OBJECT(s)),
                          DEVICE(s)->id, &DEVICE(s)->mem_reentrancy_guard,
                          s);
}

void ar6003_fw_reset(AR6003State *s)
{
    if (!s->mem) {
        return;
    }
    g_hash_table_remove_all(s->mem);
    ar6003_fw_mem_write32(s, AR6003_HI_BOARD_DATA, AR6003_BOARD_ADDR);
    timer_del(s->fw_timer);
    timer_del(s->credit_timer);
    timer_del(s->scan_timer);
    timer_del(s->client_timer);
    timer_del(s->eapol_timer);
    timer_del(s->roc_timer);
    timer_del(s->dhcp_timer);
    s->dhcp_state = 0;
    s->peer_valid = false;
    s->phase = AR6003_PH_OFF;
    s->lz_addr = s->lz_bytes = s->bmi_bytes = 0;
    memset(s->credits_pending, 0, sizeof(s->credits_pending));
    s->setup_done = false;
    s->wmi_ready_sent = false;
    memset(s->vif, 0, sizeof(s->vif));
}

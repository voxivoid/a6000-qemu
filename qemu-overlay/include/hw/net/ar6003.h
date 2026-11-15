/*
 * Atheros AR6003 (hw 2.1.1) SDIO Wi-Fi module of the Sony a5000.
 *
 * Shared state of the SDIO card (hw/sd/ar6003_sdio.c: SD protocol, CCCR,
 * CIS, host-interface registers, mailboxes) and of the chip-firmware HLE
 * (hw/net/ar6003_fw.c: BMI, HTC, WMI, virtual air, WPA2-PSK).
 * Spec: emulator/docs/RE_WIFI.md.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_NET_AR6003_H
#define HW_NET_AR6003_H

#include "hw/qdev-core.h"
#include "net/net.h"
#include "qemu/queue.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_AR6003_SDIO "ar6003-sdio"
OBJECT_DECLARE_SIMPLE_TYPE(AR6003State, AR6003_SDIO)

#define AR6003_MAX_APS      4
#define AR6003_MBOX_BLOCK   128     /* mbox_info.block_size */

/* one target->host message in mailbox 0 */
typedef struct AR6003Msg {
    QTAILQ_ENTRY(AR6003Msg) next;
    uint32_t len;               /* bytes the host reads (padded for HTC) */
    uint32_t pos;
    uint8_t data[];
} AR6003Msg;

/* a virtual access point of the "air" (station mode) */
typedef struct AR6003AP {
    char ssid[33];
    char psk[64];               /* empty = open network */
    uint8_t bssid[6];
    uint8_t ch;                 /* 2.4 GHz channel 1..13 */
    uint8_t pmk[32];
} AR6003AP;

/* WPA2-PSK 4-way handshake state (one per role) */
typedef struct AR6003Eapol {
    int state;                  /* 0 idle, 1..4 = last message sent/seen */
    uint8_t anonce[32], snonce[32];
    uint8_t ptk[48];            /* KCK 16 | KEK 16 | TK 16 */
    uint8_t replay[8];
    uint8_t pmk[32];
    uint8_t aa[6], spa[6];      /* authenticator / supplicant address */
    int retries;
} AR6003Eapol;

/* per-vif (ath0 = 0 station, ath1 = 1 P2P/AP) state */
typedef struct AR6003Vif {
    bool connected;
    bool ap_mode;               /* AP_CONFIG_COMMIT seen */
    int ap_idx;                 /* station: joined virtual AP */
    uint8_t bssid[6];
    uint16_t freq;
    /* AP mode (camera is the BSS) */
    char ssid[33];
    uint8_t crypto;             /* WMI crypto type of the BSS */
    bool client_joined;
    uint8_t rsn_ie[64];
    uint8_t rsn_ie_len;
    AR6003Eapol eapol;
} AR6003Vif;

struct AR6003State {
    DeviceState parent_obj;

    /* properties */
    NICConf conf;
    char *aps;                  /* "ssid=..,psk=..,ch=..[,bssid=..];..." */
    char *client;               /* virtual client: "auto" or "off" */
    char *client_psk;
    uint32_t ready_delay_ms;

    NICState *nic;
    uint8_t mac[6];

    /* power: pwd = CHIP_PWD_L (p1.4), vdd = p6.2 && p6.3 */
    bool pwd, vdd[2], powered;

    /* SD state */
    uint16_t rca;
    bool selected;
    /* data phase of the current CMD53 */
    bool xfer_active, xfer_write, xfer_inc;
    uint8_t xfer_fn;
    uint32_t xfer_addr, xfer_left;

    /* CCCR / FBR */
    uint8_t ioe, ien, bus_if, cccr_f0, cccr_hs, cccr_pwr;
    uint16_t blksz[2];

    /* function 1 host interface (0x400..0x4FF) */
    uint8_t int_en[4];          /* 0x418..0x41B */
    uint8_t count[8];           /* 0x420/0x440 credit counters */
    uint8_t win_data[4], win_waddr[4], win_raddr[4];
    uint8_t hif_misc[0x100];    /* other bytes: read back, logged */
    bool irq_level;

    /* mailbox 0: host->target accumulation, target->host FIFO */
    uint8_t *txbuf;
    uint32_t txlen;
    QTAILQ_HEAD(, AR6003Msg) rxq;

    /* firmware HLE */
    int phase;                  /* AR6003_PH_* */
    GHashTable *mem;            /* target RAM pages, (addr & 0x1FFFFF) >> 12 */
    uint32_t lz_addr, lz_bytes;
    uint32_t bmi_bytes;         /* image bytes written over BMI */
    uint16_t credits_pending[8];
    bool setup_done;
    bool wmi_ready_sent;
    QEMUTimer *fw_timer;        /* BMI DONE -> HTC READY, WMI READY */
    QEMUTimer *credit_timer;
    QEMUTimer *scan_timer;
    QEMUTimer *client_timer;
    QEMUTimer *eapol_timer;
    QEMUTimer *roc_timer;           /* remain-on-channel expiry */
    uint32_t roc_freq, roc_dur;
    int roc_vif;
    int scan_vif;

    AR6003AP ap[AR6003_MAX_APS];
    int nap;
    AR6003Vif vif[2];
    uint8_t client_mac[6];
    uint8_t peer_mac[6];            /* netdev peer (AP mode) */
    bool peer_valid;
    /* virtual client's DHCP (AP mode) */
    QEMUTimer *dhcp_timer;
    int dhcp_state;                 /* 0 idle, 1 discover, 3 request, 5 bound */
    int dhcp_vif, dhcp_tries;
    uint32_t dhcp_xid;
    uint8_t dhcp_offer[4], dhcp_server[4], client_ip[4];
    char client_join_ssid[33];
    char client_join_psk[64];
    uint64_t tx_frames, rx_frames, dropped;
};

enum {
    AR6003_PH_OFF,
    AR6003_PH_BMI,              /* boot ROM BMI loader */
    AR6003_PH_BOOT,             /* BMI DONE seen, firmware "booting" */
    AR6003_PH_HTC,              /* HTC READY sent, services connecting */
    AR6003_PH_WMI,              /* SETUP_COMPLETE seen */
};

/* hw/sd/ar6003_sdio.c */
void ar6003_rx_push(AR6003State *s, const uint8_t *data, uint32_t len,
                    uint32_t padded);
bool ar6003_rx_pending(AR6003State *s);
void ar6003_update_irq(AR6003State *s);

/* hw/net/ar6003_fw.c */
void ar6003_fw_init(AR6003State *s, Error **errp);
void ar6003_fw_reset(AR6003State *s);
void ar6003_fw_mbox_msg(AR6003State *s, const uint8_t *buf, uint32_t len);
uint32_t ar6003_fw_mem_read32(AR6003State *s, uint32_t addr);
void ar6003_fw_mem_write32(AR6003State *s, uint32_t addr, uint32_t val);
void ar6003_fw_rx_consumed(AR6003State *s);
void ar6003_fw_client_join(AR6003State *s, const char *spec, Error **errp);

#endif

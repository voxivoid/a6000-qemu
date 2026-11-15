/*
 * CXD90014 HDMI transmitter core (a5000), PA 0xF2A24000, 16 KiB, no IRQ.
 *
 * docs/RE_HDMI.md (hdmi.ko, firmware 1.10).  The core is an 8-bit register
 * file on a 32-bit stride: page 0 (system / interrupt / DDC) at +0x000,
 * page 1 (audio-video / InfoFrames) at +0x400, TMDS PHY at +0x2000.
 * hdmi.ko polls it from osal_utimer cell programs; nothing is interrupt
 * driven.
 *
 *   +0x014  soft reset (3 then 0)                         RAM
 *   +0x020  bit0 SYS_CTRL1.PD# (1 = TX powered)            RAM
 *   +0x024  SYS_STAT (RO): bit0 "not stable" [?] (reads 0), bit1 HPD,
 *           bit2 RSEN
 *           (hdmi_core_check_p_stable @0x9984, hpd_exec_hpd_state @0xfee8,
 *           hdmi_core_check_rsen_state @0x9904)
 *   +0x1C4  INTR1: bit6 = HPD changed, write 1 to clear
 *           (hpd_exec_hpd_intr @0xfe7c, hpd_exec_hpd_intr_reset @0xfdec)
 *   +0x3B4  DDC_ADDR (0xA0)   +0x3B8 DDC_SEGM   +0x3BC DDC_OFFSET
 *   +0x3C0  DDC_COUNT1        +0x3C4 DDC_COUNT2
 *   +0x3C8  DDC_STATUS: bit3 FIFO full / data ready, bit5 NO_ACK,
 *           bit6 BUS_LOW (hdmi_core_chk_edid_status @0x8f94)
 *   +0x3CC  DDC_CMD: 0xF abort, 0x9 clear FIFO, 0xA clock SCL,
 *           0x4 enhanced (segment) sequential read
 *   +0x3D0  DDC_DATA (FIFO pop, hdmi_core_edid_set_buffer @0xa794)
 *   +0x3D4  DDC_FIFOCNT [?] (not read by the driver)
 *   +0x4F4  bit0 AV DPD (1 = powered)
 *   +0x51C  [6:0] AVI InfoFrame VIC (hdmi_core_task_content)
 *   +0x2008 bit0 TMDS PHY on
 *   other page-0/1, InfoFrame and PHY registers: RAM (see the doc's table)
 *
 * Machine side: property "hpd" (cable plugged; can be changed at run time,
 * qom-set /machine/hdmi-tx hpd on), "edid" = optional file (128 or 256
 * bytes) replacing the built-in generic EDID.  GPIO out 0 = TMDS active
 * (PHY on && AV powered && HPD), consumed by the VPH line output.
 *
 * Open questions (doc §5): the exact "last partial chunk" DDC status
 * (the driver always reads 128 = 8 x 16 bytes); the meaning of SYS_STAT
 * bit0 (the driver waits for it to be 0; see hdmi_sys_stat).
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "trace.h"

#define TYPE_CXD_HDMI "cxd90014-hdmi"
OBJECT_DECLARE_SIMPLE_TYPE(CxdHdmiState, CXD_HDMI)

#define HDMI_SIZE   0x4000
#define EDID_SIZE   256
#define FIFO_SIZE   16

#define SYS_CTRL1   0x020
#define SYS_STAT    0x024
#define INTR1       0x1C4
#define DDC_ADDR    0x3B4
#define DDC_SEGM    0x3B8
#define DDC_OFFSET  0x3BC
#define DDC_COUNT1  0x3C0
#define DDC_COUNT2  0x3C4
#define DDC_STATUS  0x3C8
#define DDC_CMD     0x3CC
#define DDC_DATA    0x3D0
#define DDC_FIFOCNT 0x3D4
#define AV_DPD      0x4F4
#define AVI_VIC     0x51C
#define PHY_TMDS    0x2008

#define DDC_ST_FIFO_FULL  (1u << 3)
#define DDC_ST_NO_ACK     (1u << 5)

struct CxdHdmiState {
    SysBusDevice parent_obj;
    MemoryRegion mr;
    uint32_t regs[HDMI_SIZE / 4];
    uint8_t edid[EDID_SIZE];
    char *edid_file;
    bool hpd;
    bool tmds;
    qemu_irq tmds_out;
    /* DDC engine */
    uint8_t fifo[FIFO_SIZE];
    unsigned fifo_n, fifo_rd;
    unsigned ddc_ptr, ddc_left;
    bool ddc_nack;
};

#define R(o) (s->regs[(o) / 4])

/*
 * Offsets hdmi.ko has descriptors for (docs/RE_HDMI.md §1.2).  Accesses
 * outside them are still RAM-backed but logged under -d unimp.
 */
static bool hdmi_known(hwaddr off)
{
    return off == 0x014 || off == SYS_CTRL1 || off == SYS_STAT ||
           off == 0x0CC || off == 0x120 || off == INTR1 || off == 0x1D4 ||
           off == 0x1E4 || off == 0x208 ||
           (off >= DDC_ADDR && off <= DDC_FIFOCNT) ||
           (off >= 0x404 && off < 0x780) ||
           (off >= 0x2004 && off <= 0x2034);
}

/* ---- default EDID: generic, generated here (not from any real sink) ---- */

static void edid_dtd(uint8_t *d, unsigned clk10k, unsigned ha, unsigned hb,
                     unsigned va, unsigned vb, unsigned hso, unsigned hsw,
                     unsigned vso, unsigned vsw, unsigned hmm, unsigned vmm,
                     bool interlaced)
{
    d[0] = clk10k & 0xff;
    d[1] = clk10k >> 8;
    d[2] = ha & 0xff;
    d[3] = hb & 0xff;
    d[4] = (ha >> 8) << 4 | (hb >> 8);
    d[5] = va & 0xff;
    d[6] = vb & 0xff;
    d[7] = (va >> 8) << 4 | (vb >> 8);
    d[8] = hso & 0xff;
    d[9] = hsw & 0xff;
    d[10] = (vso & 15) << 4 | (vsw & 15);
    d[11] = (hso >> 8) << 6 | (hsw >> 8) << 4 | (vso >> 4) << 2 | (vsw >> 4);
    d[12] = hmm & 0xff;
    d[13] = vmm & 0xff;
    d[14] = (hmm >> 8) << 4 | (vmm >> 8);
    d[15] = 0;
    d[16] = 0;
    d[17] = (interlaced ? 0x80 : 0) | 0x1E;    /* digital separate, +H +V */
}

static void edid_text(uint8_t *d, uint8_t tag, const char *s)
{
    memset(d, 0, 18);
    d[3] = tag;
    memset(d + 5, ' ', 13);
    size_t n = MIN(strlen(s), 13);
    memcpy(d + 5, s, n);
    if (n < 13) {
        d[5 + n] = '\n';
    }
}

static void edid_checksum(uint8_t *b)
{
    uint8_t sum = 0;
    for (int i = 0; i < 127; i++) {
        sum += b[i];
    }
    b[127] = -sum;
}

/*
 * Block 0: EDID 1.3, "QEMU HDMI", preferred 1920x1080p60.  Block 1: CEA-861
 * with VICs 16 (native), 31, 5, 20, 4, 19, 2, 3, 17, basic LPCM audio and an
 * HDMI VSDB (OUI 00-0C-03) with physical address 1.0.0.0, which cec.ko needs
 * before it allocates a logical address
 * (cec_core_status_task_check_phy_addr_state).
 */
static void hdmi_default_edid(uint8_t *e)
{
    static const uint8_t hdr[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
    static const uint8_t chroma[10] = {   /* sRGB primaries, D65 */
        0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54,
    };
    uint8_t *b = e;

    memset(e, 0, EDID_SIZE);
    memcpy(b, hdr, 8);
    /* manufacturer "QEM" (5-bit letters, A = 1) */
    uint16_t mfg = ('Q' - '@') << 10 | ('E' - '@') << 5 | ('M' - '@');
    b[8] = mfg >> 8;
    b[9] = mfg & 0xff;
    b[10] = 0x01;                   /* product code 0x0001 */
    b[16] = 1;                      /* week 1 */
    b[17] = 2014 - 1990;            /* year */
    b[18] = 1;                      /* EDID 1.3 */
    b[19] = 3;
    b[20] = 0x80;                   /* digital input */
    b[21] = 52;                     /* 52 x 29 cm */
    b[22] = 29;
    b[23] = 120;                    /* gamma 2.2 */
    b[24] = 0x0A;                   /* RGB colour, preferred timing in DTD 1 */
    memcpy(b + 25, chroma, 10);
    b[35] = 0x20;                   /* established: 640x480@60 */
    for (int i = 0; i < 8; i++) {   /* standard timings: unused = 01 01 */
        b[38 + 2 * i] = 1;
        b[39 + 2 * i] = 1;
    }
    b[38] = 1280 / 8 - 31;          /* 1280x720@60, 16:9 */
    b[39] = 0xC0;
    b[40] = 1920 / 8 - 31;          /* 1920x1080@60, 16:9 */
    b[41] = 0xC0;
    edid_dtd(b + 54, 14850, 1920, 280, 1080, 45, 88, 44, 4, 5, 509, 286,
             false);                                        /* 1080p60 */
    edid_dtd(b + 72, 7425, 1280, 370, 720, 30, 110, 40, 5, 5, 509, 286,
             false);                                        /* 720p60 */
    edid_text(b + 90, 0xFC, "QEMU HDMI");
    /* range limits: 23-61 Hz V, 15-68 kHz H, 150 MHz */
    memset(b + 108, 0, 18);
    b[111] = 0xFD;
    b[113] = 23;
    b[114] = 61;
    b[115] = 15;
    b[116] = 68;
    b[117] = 15;
    b[118] = 0;
    b[119] = 0x0A;
    memset(b + 120, ' ', 6);
    b[126] = 1;                     /* one extension block */
    edid_checksum(b);

    b = e + 128;
    b[0] = 0x02;                    /* CEA-861 extension */
    b[1] = 0x03;
    b[3] = 0x71;                    /* basic audio, YCbCr 4:4:4/4:2:2, 1 DTD */
    unsigned p = 4;
    static const uint8_t vics[] = { 0x80 | 16, 31, 5, 20, 4, 19, 2, 3, 17 };
    b[p++] = 2 << 5 | sizeof(vics);                 /* video data block */
    memcpy(b + p, vics, sizeof(vics));
    p += sizeof(vics);
    b[p++] = 1 << 5 | 3;                            /* audio: LPCM 2 ch */
    b[p++] = 1 << 3 | 1;
    b[p++] = 0x07;                                  /* 32 / 44.1 / 48 kHz */
    b[p++] = 0x07;                                  /* 16 / 20 / 24 bit */
    b[p++] = 4 << 5 | 3;                            /* speaker allocation */
    b[p++] = 0x01;                                  /* FL/FR */
    b[p++] = 0;
    b[p++] = 0;
    b[p++] = 3 << 5 | 5;                            /* HDMI VSDB */
    b[p++] = 0x03;                                  /* OUI 00-0C-03 */
    b[p++] = 0x0C;
    b[p++] = 0x00;
    b[p++] = 0x10;                                  /* physical address */
    b[p++] = 0x00;                                  /* 1.0.0.0 */
    b[2] = p;                                       /* DTDs start here */
    edid_dtd(b + p, 14850, 1920, 280, 1080, 45, 88, 44, 4, 5, 509, 286,
             false);
    edid_checksum(b);
}

/* ---- state ---- */

static void hdmi_update_tmds(CxdHdmiState *s)
{
    bool on = s->hpd && (R(PHY_TMDS) & 1) && (R(AV_DPD) & 1);
    if (on != s->tmds) {
        s->tmds = on;
        trace_cxd90014_hdmi_tmds(on, R(AVI_VIC) & 0x7f);
        qemu_set_irq(s->tmds_out, on);
    }
}

static uint32_t hdmi_sys_stat(CxdHdmiState *s)
{
    bool phy_on = (R(PHY_TMDS) & 1) && (R(AV_DPD) & 1);
    /*
     * bit0 reads 0.  The power-on cell program reads it through
     * hdmi_core_check_p_stable @0x9984, which returns 3 (= retry, sub_D0E4;
     * -EIO after 20 retries) while bit0 is SET and 2 (= continue) once it
     * is clear (V: with bit0 = PD# = 1 the power-on failed with -5 and the
     * TX was never powered).  hypothesis: bit0 is a "clock not yet stable"
     * flag on this core, not OMAP4's P_STABLE; doc §5 q2.
     */
    return (s->hpd ? 2 : 0) | (s->hpd && phy_on ? 4 : 0);
}

static void ddc_fill(CxdHdmiState *s)
{
    while (s->ddc_left && s->fifo_n < FIFO_SIZE) {
        unsigned w = (s->fifo_rd + s->fifo_n) % FIFO_SIZE;
        s->fifo[w] = s->edid[s->ddc_ptr % EDID_SIZE];
        s->ddc_ptr++;
        s->ddc_left--;
        s->fifo_n++;
    }
}

static void ddc_clear(CxdHdmiState *s)
{
    s->fifo_n = s->fifo_rd = 0;
}

static void ddc_cmd(CxdHdmiState *s, unsigned cmd)
{
    switch (cmd) {
    case 0xF:                       /* abort */
        s->ddc_left = 0;
        s->ddc_nack = false;
        ddc_clear(s);
        break;
    case 0x9:                       /* clear FIFO */
        ddc_clear(s);
        break;
    case 0xA:                       /* clock SCL (bus recovery) */
        break;
    case 0x4:                       /* enhanced DDC sequential read */
    case 0x2:                       /* sequential read [?] (not used) */
        ddc_clear(s);
        s->ddc_nack = !s->hpd || (R(DDC_ADDR) & 0xfe) != 0xA0;
        if (s->ddc_nack) {
            s->ddc_left = 0;
        } else {
            s->ddc_ptr = (cmd == 0x4 ? (R(DDC_SEGM) & 0xff) * 256 : 0) +
                         (R(DDC_OFFSET) & 0xff);
            s->ddc_left = (R(DDC_COUNT1) & 0xff) |
                          (R(DDC_COUNT2) & 3) << 8;
            ddc_fill(s);
        }
        trace_cxd90014_hdmi_ddc_read(R(DDC_ADDR) & 0xff, R(DDC_SEGM) & 0xff,
                                     R(DDC_OFFSET) & 0xff, s->ddc_left +
                                     s->fifo_n, s->ddc_nack);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "cxd90014-hdmi: DDC command 0x%x\n", cmd);
        break;
    }
}

static uint64_t hdmi_read(void *opaque, hwaddr off, unsigned size)
{
    CxdHdmiState *s = opaque;
    uint32_t v;

    off &= ~3;
    switch (off) {
    case SYS_STAT:
        v = hdmi_sys_stat(s);
        break;
    case DDC_STATUS:
        v = (R(DDC_STATUS) & ~(DDC_ST_FIFO_FULL | DDC_ST_NO_ACK)) |
            (s->ddc_nack ? DDC_ST_NO_ACK : 0) |
            (s->fifo_n == FIFO_SIZE || (s->fifo_n && !s->ddc_left) ?
             DDC_ST_FIFO_FULL : 0);
        break;
    case DDC_DATA:
        if (s->fifo_n) {
            v = s->fifo[s->fifo_rd];
            s->fifo_rd = (s->fifo_rd + 1) % FIFO_SIZE;
            s->fifo_n--;
            ddc_fill(s);
        } else {
            v = 0xff;
            qemu_log_mask(LOG_GUEST_ERROR, "cxd90014-hdmi: DDC FIFO empty\n");
        }
        break;
    case DDC_FIFOCNT:
        v = s->fifo_n;
        break;
    default:
        v = R(off);
        if (!hdmi_known(off)) {
            qemu_log_mask(LOG_UNIMP, "cxd90014-hdmi: rd 0x%03" HWADDR_PRIx
                          " -> 0x%x\n", off, v);
        }
        break;
    }
    return v;
}

static void hdmi_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    CxdHdmiState *s = opaque;
    uint32_t v = val;

    off &= ~3;
    trace_cxd90014_hdmi_write(off, v);
    switch (off) {
    case SYS_STAT:
    case DDC_FIFOCNT:
        qemu_log_mask(LOG_GUEST_ERROR, "cxd90014-hdmi: write to RO 0x%03"
                      HWADDR_PRIx "\n", off);
        return;
    case INTR1:
        R(off) &= ~v;                       /* write 1 to clear */
        return;
    case DDC_CMD:
        R(off) = v;
        ddc_cmd(s, v & 0xf);
        return;
    case DDC_DATA:
        return;                             /* write FIFO: not used */
    default:
        if (!hdmi_known(off)) {
            qemu_log_mask(LOG_UNIMP, "cxd90014-hdmi: wr 0x%03" HWADDR_PRIx
                          " = 0x%x\n", off, v);
        }
        R(off) = v;
        break;
    }
    if (off == PHY_TMDS || off == AV_DPD) {
        hdmi_update_tmds(s);
    }
}

static const MemoryRegionOps hdmi_ops = {
    .read = hdmi_read, .write = hdmi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void hdmi_set_hpd_level(CxdHdmiState *s, bool on)
{
    if (on == s->hpd) {
        return;
    }
    s->hpd = on;
    R(INTR1) |= 0x40;                       /* HPD change */
    if (!on) {
        s->ddc_left = 0;
        ddc_clear(s);
    }
    trace_cxd90014_hdmi_hpd(on);
    hdmi_update_tmds(s);
}

static bool hdmi_get_hpd(Object *obj, Error **errp)
{
    return CXD_HDMI(obj)->hpd;
}

static void hdmi_set_hpd(Object *obj, bool v, Error **errp)
{
    hdmi_set_hpd_level(CXD_HDMI(obj), v);
}

static bool hdmi_get_tmds(Object *obj, Error **errp)
{
    return CXD_HDMI(obj)->tmds;
}

static void hdmi_reset(DeviceState *dev)
{
    CxdHdmiState *s = CXD_HDMI(dev);
    memset(s->regs, 0, sizeof(s->regs));
    s->ddc_left = 0;
    s->ddc_nack = false;
    ddc_clear(s);
    s->tmds = false;
    qemu_set_irq(s->tmds_out, 0);
}

static void hdmi_realize(DeviceState *dev, Error **errp)
{
    CxdHdmiState *s = CXD_HDMI(dev);

    hdmi_default_edid(s->edid);
    if (s->edid_file) {
        g_autofree gchar *buf = NULL;
        gsize len;
        if (!g_file_get_contents(s->edid_file, &buf, &len, NULL) ||
            (len != 128 && len != 256)) {
            error_setg(errp, "cxd90014-hdmi: edid=%s: need a 128 or 256 "
                       "byte file", s->edid_file);
            return;
        }
        memset(s->edid, 0, EDID_SIZE);
        memcpy(s->edid, buf, len);
    }
}

static void hdmi_init(Object *obj)
{
    CxdHdmiState *s = CXD_HDMI(obj);
    memory_region_init_io(&s->mr, obj, &hdmi_ops, s, "hdmi", HDMI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mr);
    qdev_init_gpio_out(DEVICE(obj), &s->tmds_out, 1);
    object_property_add_bool(obj, "hpd", hdmi_get_hpd, hdmi_set_hpd);
    object_property_set_description(obj, "hpd",
        "HDMI cable plugged (hot-plug detect high)");
    object_property_add_bool(obj, "tmds", hdmi_get_tmds, NULL);
}

static const Property hdmi_props[] = {
    DEFINE_PROP_STRING("edid", CxdHdmiState, edid_file),
};

static void hdmi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = hdmi_realize;
    device_class_set_legacy_reset(dc, hdmi_reset);
    device_class_set_props(dc, hdmi_props);
}

static const TypeInfo hdmi_info = {
    .name = TYPE_CXD_HDMI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CxdHdmiState),
    .instance_init = hdmi_init,
    .class_init = hdmi_class_init,
};

static void hdmi_register(void)
{
    type_register_static(&hdmi_info);
}
type_init(hdmi_register)

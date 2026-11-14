/*
 * Sony CXD90014 NAND subsystem (a5000).
 *
 *   nand_reg  PA 0x00020000  NAND controller registers        IRQ 172
 *   arm7io    PA 0x00011000  ARM7 sub-CPU mailbox (latch/bell) IRQ 170
 *   arm7_sram PA 0x00000000  ARM7 SRAM (owned by the board; we access it
 *                            through the system address space)
 *   clkrst3   PA 0xF2900000  clock/reset block (ARM7 + NAND resets)
 *
 * The ARM7 runs Sony's NAND firmware, which is not part of any firmware
 * image we have (it lives in reserved NAND blocks).  This model therefore
 * implements that firmware at the mailbox level (HLE), exactly as lld.ko
 * uses it; see emulator/docs/NAND_PROTOCOL.md for the full protocol and the
 * provenance of every field.  Everything above the mailbox — lld.ko, the
 * MUMIN FTL in nand.ko, the block devices — is the unmodified firmware.
 *
 * Flash chip: 1 die, 1 plane, 2048 blocks x 64 pages x (4096 data +
 * 8 "extra" bytes as seen through the ARM7 firmware), SLC.  Backing file
 * (property "file"): [data 512 MiB][extra 1 MiB][programmed bitmap 16 KiB],
 * created erased (0xFF) when missing.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "system/address-spaces.h"
#include "trace.h"
#include <sys/mman.h>

#define TYPE_CXD_NAND "cxd90014-nand"
OBJECT_DECLARE_SIMPLE_TYPE(CXDNandState, CXD_NAND)

#define NBLK        2048
#define PPB         64
#define PSZ         4096
#define XSZ         8
#define NPAGES      (NBLK * PPB)
#define DATA_SIZE   ((uint64_t)NPAGES * PSZ)
#define XTRA_SIZE   ((uint64_t)NPAGES * XSZ)
#define BMAP_SIZE   (NPAGES / 8)
#define FILE_SIZE   (DATA_SIZE + XTRA_SIZE + BMAP_SIZE)

/* ARM7 SRAM mailbox (SRAM PA 0) */
#define SRAM_FIRM_STATUS 0x3F40
#define SRAM_CMD         0x3F48
#define SRAM_API         0x3F54
#define SRAM_RESULT      0x3F5C
#define SRAM_DETAIL      0x3F64
#define SRAM_POUT1       0x3F68
#define SRAM_POUT2       0x3F6C

/* ARM7 firmware result codes (lld boss_lld_local_change_lld_ans_from_firm) */
#define AR_OK        0
#define AR_BLANK     13      /* -> 0xFFFDFFF0 */
#define AR_PARAM     14

struct CXDNandState {
    SysBusDevice parent_obj;
    MemoryRegion reg_mr, arm7io_mr, clkrst_mr;
    uint32_t reg[0x10000 / 4];
    uint32_t clkrst[0x1000 / 4];
    uint32_t latch;          /* arm7io+0: ARM7 -> CA5 done */
    bool arm7_booting;       /* released from reset, next bell = boot */
    qemu_irq nand_irq, arm7_irq;
    char *file;
    uint8_t *mem;            /* mmap of the backing file */
    uint8_t scratch[0x20000];
};

/* ---------------- backing store ---------------- */

static inline uint8_t *pg_data(CXDNandState *s, uint32_t blk, uint32_t pg)
{
    return s->mem + ((uint64_t)blk * PPB + pg) * PSZ;
}

static inline uint8_t *pg_extra(CXDNandState *s, uint32_t blk, uint32_t pg)
{
    return s->mem + DATA_SIZE + ((uint64_t)blk * PPB + pg) * XSZ;
}

static inline bool pg_prog(CXDNandState *s, uint32_t blk, uint32_t pg)
{
    uint32_t i = blk * PPB + pg;
    return s->mem[DATA_SIZE + XTRA_SIZE + i / 8] & (1 << (i & 7));
}

static inline void pg_set_prog(CXDNandState *s, uint32_t blk, uint32_t pg)
{
    uint32_t i = blk * PPB + pg;
    s->mem[DATA_SIZE + XTRA_SIZE + i / 8] |= 1 << (i & 7);
}

/* NAND program semantics: bits can only go 1 -> 0 */
static void prog_bytes(uint8_t *dst, const uint8_t *src, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        dst[i] &= src[i];
    }
}

static void erase_blk(CXDNandState *s, uint32_t blk)
{
    memset(pg_data(s, blk, 0), 0xff, (size_t)PPB * PSZ);
    memset(pg_extra(s, blk, 0), 0xff, PPB * XSZ);
    for (int p = 0; p < PPB; p++) {
        uint32_t i = blk * PPB + p;
        s->mem[DATA_SIZE + XTRA_SIZE + i / 8] &= ~(1 << (i & 7));
    }
}

/* ---------------- guest memory helpers (DDR physical) ---------------- */

static uint32_t g32(hwaddr pa)
{
    return address_space_ldl_le(&address_space_memory, pa,
                                MEMTXATTRS_UNSPECIFIED, NULL);
}

static void p32(hwaddr pa, uint32_t v)
{
    address_space_stl_le(&address_space_memory, pa, v,
                         MEMTXATTRS_UNSPECIFIED, NULL);
}

static void gread(hwaddr pa, void *buf, size_t n)
{
    address_space_read(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED,
                       buf, n);
}

static void gwrite(hwaddr pa, const void *buf, size_t n)
{
    address_space_write(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED,
                        buf, n);
}

/* scatter/gather list SG{count, SGE*}, SGE{len, addr} */
typedef struct SGCur {
    hwaddr e;           /* phys of current SGE */
    uint32_t left;      /* SGEs left */
    uint32_t len, addr; /* remaining part of current SGE */
} SGCur;

static void sg_init(SGCur *c, hwaddr sg)
{
    c->left = g32(sg);
    c->e = g32(sg + 4);
    c->len = 0;
}

static bool sg_xfer(SGCur *c, uint8_t *buf, size_t n, bool to_guest)
{
    while (n) {
        if (!c->len) {
            if (!c->left) {
                return false;
            }
            c->len = g32(c->e);
            c->addr = g32(c->e + 4);
            c->e += 8;
            c->left--;
            continue;
        }
        uint32_t k = MIN(c->len, n);
        if (to_guest) {
            gwrite(c->addr, buf, k);
        } else {
            gread(c->addr, buf, k);
        }
        buf += k; n -= k; c->addr += k; c->len -= k;
    }
    return true;
}

/* argument sub-structures */
static uint32_t pblk_of(hwaddr pblk_pa)          /* PBLK{die,plane,pblk} */
{
    return g32(pblk_pa + 8);
}

static uint32_t grp_blk(hwaddr grp_pa, uint32_t plane, uint32_t *nplanes)
{
    hwaddr info = g32(grp_pa + 4);               /* PBLKGROUP{g0, GINFO*} */
    uint32_t n = g32(info + 4);                  /* GINFO{i0, n, pblks*} */
    if (nplanes) {
        *nplanes = n;
    }
    return g32(g32(info + 8) + 4 * MIN(plane, n ? n - 1 : 0));
}

/* ---------------- ARM7 firmware commands ---------------- */

static uint32_t do_read(CXDNandState *s, uint32_t blk, uint32_t page,
                        uint32_t n, hwaddr sg, hwaddr xbuf, uint32_t *done)
{
    SGCur c;
    uint32_t res = AR_OK;
    trace_cxd_nand_op("read", blk, page, n);
    if (blk >= NBLK || page + n > PPB) {
        return AR_PARAM;
    }
    sg_init(&c, sg);
    for (uint32_t i = 0; i < n; i++) {
        if (!pg_prog(s, blk, page + i)) {
            res = AR_BLANK;          /* hypothesis: blank-page detection */
        }
        sg_xfer(&c, pg_data(s, blk, page + i), PSZ, true);
        if (xbuf) {
            gwrite(xbuf + 8 * i, pg_extra(s, blk, page + i), XSZ);
        }
    }
    *done = n;
    return res;
}

static uint32_t do_write(CXDNandState *s, uint32_t blk, uint32_t page,
                         uint32_t n, hwaddr sg, hwaddr xbuf, uint32_t *done)
{
    SGCur c;
    uint8_t tmp[PSZ], x[XSZ];
    trace_cxd_nand_op("write", blk, page, n);
    if (blk >= NBLK || page + n > PPB) {
        return AR_PARAM;
    }
    sg_init(&c, sg);
    for (uint32_t i = 0; i < n; i++) {
        sg_xfer(&c, tmp, PSZ, false);
        prog_bytes(pg_data(s, blk, page + i), tmp, PSZ);
        if (xbuf) {
            gread(xbuf + 8 * i, x, XSZ);
            prog_bytes(pg_extra(s, blk, page + i), x, XSZ);
        }
        pg_set_prog(s, blk, page + i);
    }
    *done = n;
    return AR_OK;
}

static int last_written(CXDNandState *s, uint32_t blk)
{
    for (int p = PPB - 1; p >= 0; p--) {
        if (pg_prog(s, blk, p)) {
            return p;
        }
    }
    return -1;
}

/* ---- LDEC: Sony LZ77 (kernel lib/lz77/lz77_inflate.c) ---- */
static size_t lz77_chunk(const uint8_t *in, size_t inlen, size_t *ip,
                         uint8_t *out, size_t outcap, size_t op)
{
    static const uint8_t lens[16] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
                                     14, 15, 16, 32, 64};
    size_t i = *ip;
    if (i >= inlen) {
        return op;
    }
    uint8_t type = in[i++];
    if (type == 0x0f) {
        uint32_t l = in[i + 1] | in[i + 2] << 8;
        i += 3;
        l = MIN(l, outcap - op);
        memcpy(out + op, in + i, l);
        *ip = i + l;
        return op + l;
    }
    if (type != 0xf0) {
        *ip = inlen;
        return op;
    }
    for (;;) {
        if (i >= inlen) {
            break;
        }
        uint8_t flags = in[i++];
        for (int b = 0; b < 8; b++) {
            if (flags >> b & 1) {
                uint8_t d0 = in[i], d1 = in[i + 1];
                i += 2;
                uint32_t l = lens[d0 >> 4];
                uint32_t bd = (d0 & 0xf) << 8 | d1;
                if (bd == 0) {
                    *ip = i;
                    return op;
                }
                for (uint32_t k = 0; k < l && op < outcap; k++, op++) {
                    out[op] = op >= bd ? out[op - bd] : 0;
                }
            } else if (op < outcap) {
                out[op++] = in[i++];
            } else {
                i++;
            }
        }
    }
    *ip = i;
    return op;
}

static uint32_t do_ldec(CXDNandState *s, hwaddr api)
{
    /* the API word points at the LDEC_REQ copy (lld: entry[4] = entry+5):
     * {n_items, items*, byte_off, out_size>>12, dst_phys, dst_len} */
    hwaddr arg = g32(api);
    uint32_t nitems = g32(arg), items = g32(arg + 4), off = g32(arg + 8);
    hwaddr dst = g32(arg + 16);
    uint32_t dlen = g32(arg + 20);
    hwaddr detail = g32(SRAM_DETAIL);
    size_t clen = 0;
    uint32_t res = AR_OK;

    trace_cxd_nand_op("ldec", nitems, off, dlen);
    if (nitems > 1024 || dlen > 0x100000 || off >= 512) {
        qemu_log_mask(LOG_GUEST_ERROR, "cxd-nand: bad ldec request\n");
        return AR_PARAM;
    }
    for (uint32_t i = 0; i < nitems; i++) {
        hwaddr it = items + 12 * i;
        hwaddr pgp = g32(it);           /* PAGEGROUP{die,plane,page,grp*} */
        uint32_t soff = g32(it + 4), nsec = g32(it + 8);
        uint32_t plane = g32(pgp + 4), page = g32(pgp + 8);
        uint32_t blk = grp_blk(g32(pgp + 12), plane, NULL);
        uint64_t sec = (uint64_t)page * (PSZ / 512) + soff;
        for (uint32_t k = 0; k < nsec; k++, sec++) {
            uint32_t pp = sec / (PSZ / 512);
            if (blk >= NBLK || pp >= PPB || clen + 512 > sizeof(s->scratch)) {
                res = AR_PARAM;
                break;
            }
            if (!pg_prog(s, blk, pp)) {
                res = AR_BLANK;
            }
            memcpy(s->scratch + clen,
                   pg_data(s, blk, pp) + (sec % (PSZ / 512)) * 512, 512);
            clen += 512;
        }
        if (detail) {
            p32(detail + 4 * i, 0);     /* per-item ECC status: clean */
        }
    }
    if (res != AR_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, "cxd-nand: ldec read error %u\n", res);
        return res == AR_BLANK ? 12 : res;   /* blank compressed data = bad */
    }
    g_autofree uint8_t *out = g_malloc(dlen);
    size_t ip = off, op = 0;
    while (op < dlen && ip < clen) {
        size_t before = op;
        op = lz77_chunk(s->scratch, clen, &ip, out, dlen, op);
        if (op == before) {
            break;
        }
    }
    if (op < dlen) {
        memset(out + op, 0, dlen - op);
    }
    gwrite(dst, out, dlen);
    return AR_OK;
}

static void arm7_command(CXDNandState *s)
{
    uint32_t cmd = g32(SRAM_CMD);
    hwaddr a = g32(SRAM_API);
    uint32_t res = AR_OK, out1 = 0, out2 = 0, n, blk, page, np;
    hwaddr pp, x;

    switch (cmd) {
    case 0:     /* ldec read */
        res = do_ldec(s, a);
        break;
    case 6:     /* read_pages_in_pblk(PPAGE, n, -, SG, extra) */
    case 9:     /* write_pages_in_pblk */
        pp = g32(a);
        blk = pblk_of(g32(pp));
        page = g32(pp + 4);
        n = g32(a + 4);
        res = (cmd == 6 ? do_read : do_write)(s, blk, page, n, g32(a + 12),
                                              g32(a + 16), &out1);
        break;
    case 5:     /* read_pages_in_pblkgroup(PAGEGROUP, n, -, SG, extra) */
    case 8:     /* write_pages_in_pblkgroup */
        pp = g32(a);
        blk = grp_blk(g32(pp + 12), g32(pp + 4), NULL);
        page = g32(pp + 8);
        n = g32(a + 4);
        res = (cmd == 5 ? do_read : do_write)(s, blk, page, n, g32(a + 12),
                                              g32(a + 16), &out1);
        break;
    case 7:     /* read_extra_of_page(PPAGE, extra8) */
    case 10:    /* write_extra_of_page */
        pp = g32(a);
        blk = pblk_of(g32(pp));
        page = g32(pp + 4);
        x = g32(a + 4);
        trace_cxd_nand_op(cmd == 7 ? "rdextra" : "wrextra", blk, page, 1);
        if (blk >= NBLK || page >= PPB) {
            res = AR_PARAM;
        } else if (cmd == 7) {
            gwrite(x, pg_extra(s, blk, page), XSZ);
            res = pg_prog(s, blk, page) ? AR_OK : AR_BLANK;
        } else {
            uint8_t e[XSZ];
            gread(x, e, XSZ);
            prog_bytes(pg_extra(s, blk, page), e, XSZ);
            pg_set_prog(s, blk, page);
        }
        break;
    case 4: {   /* read_extra_of_top_pages(PBLK, n, extra, stat):
                 * extra of page 0 of n consecutive blocks starting at PBLK
                 * (V: MUMIN asks blk 0xc n 2036 = every user block) */
        blk = pblk_of(g32(a));
        n = g32(a + 4);
        x = g32(a + 8);
        hwaddr st = g32(a + 12);
        trace_cxd_nand_op("topextra", blk, 0, n);
        for (uint32_t i = 0; i < n && blk + i < NBLK; i++) {
            gwrite(x + 8 * i, pg_extra(s, blk + i, 0), XSZ);
            address_space_stb(&address_space_memory, st + i, 0,
                              MEMTXATTRS_UNSPECIFIED, NULL);
        }
        /* blank pages are not an error here: MUMIN rejects 0xFFFDFFF0
         * from read_extra_of_top_pages as "Invalid error" (V) */
        res = blk + n > NBLK ? AR_PARAM : AR_OK;
        break;
    }
    case 15: {  /* read_extra_pagegroup(PBLKGROUP, page, extra16, stat) */
        page = g32(a + 4);
        x = g32(a + 8);
        hwaddr st = g32(a + 12);
        grp_blk(g32(a), 0, &np);
        bool any = false;
        for (uint32_t p = 0; p < np && p < 2; p++) {
            blk = grp_blk(g32(a), p, NULL);
            gwrite(x + 8 * p, pg_extra(s, blk, page), XSZ);
            address_space_stb(&address_space_memory, st + p, 0,
                              MEMTXATTRS_UNSPECIFIED, NULL);
            any |= pg_prog(s, blk, page);
        }
        res = AR_OK;    /* same rule as cmd 4 */
        (void)any;
        break;
    }
    case 11:    /* erase_pblkgroup(PBLKGROUP) -> failed-block mask */
        grp_blk(g32(a), 0, &np);
        for (uint32_t p = 0; p < np && p < 2; p++) {
            erase_blk(s, grp_blk(g32(a), p, NULL));
        }
        break;
    case 12:    /* erase_pblk(PBLK) */
        blk = pblk_of(g32(a));
        if (blk >= NBLK) {
            res = AR_PARAM;
        } else {
            erase_blk(s, blk);
        }
        break;
    case 13: {  /* search_last_written_pages_in_pblkgroup */
        grp_blk(g32(a), 0, &np);
        int l0 = last_written(s, grp_blk(g32(a), 0, NULL));
        int l1 = np > 1 ? last_written(s, grp_blk(g32(a), 1, NULL)) : l0;
        out1 = l0; out2 = l1;
        res = l0 < 0 ? AR_BLANK : AR_OK;
        break;
    }
    case 14: {  /* search_last_written_page_in_pblk(PBLK) */
        int l = last_written(s, pblk_of(g32(a)));
        out1 = l;
        res = l < 0 ? AR_BLANK : AR_OK;
        break;
    }
    case 2:     /* mark_bad_pblkgroup(PBLKGROUP, buf16, buf4) */
    case 3: {   /* mark_bad_pblk(PBLK, buf8) */
        uint8_t e[16];
        if (cmd == 3) {
            blk = pblk_of(g32(a));
            gread(g32(a + 4), e, 8);
            np = 1;
        } else {
            gread(g32(a + 4), e, 16);
            grp_blk(g32(a), 0, &np);
        }
        for (uint32_t p = 0; p < np && p < 2; p++) {
            uint32_t b = cmd == 3 ? blk : grp_blk(g32(a), p, NULL);
            if (b >= NBLK) {
                continue;
            }
            /* hypothesis: marker goes into the extra of the first pages */
            for (int pg = 0; pg < 2; pg++) {
                prog_bytes(pg_extra(s, b, pg), e + 8 * (cmd == 2 ? p : 0), 8);
                pg_set_prog(s, b, pg);
            }
        }
        break;
    }
    default:
        qemu_log_mask(LOG_UNIMP, "cxd-nand: ARM7 cmd %u not implemented\n",
                      cmd);
        res = AR_PARAM;
    }
    trace_cxd_nand_arm7_cmd(cmd, res, out1);
    p32(SRAM_POUT1, out1);
    p32(SRAM_POUT2, out2);
    p32(SRAM_RESULT, res);
}

static void arm7_update_irq(CXDNandState *s)
{
    qemu_set_irq(s->arm7_irq, s->latch & 1);
}

static void arm7_doorbell(CXDNandState *s)
{
    if (s->arm7_booting) {
        /* firmware start-up after reset release (mumin_devif_firm_reset) */
        s->arm7_booting = false;
        p32(SRAM_FIRM_STATUS, 2);
    } else if ((s->clkrst[0xD0 / 4] & 1) == 0) {
        arm7_command(s);
    } else {
        return;     /* ARM7 held in reset: never answers */
    }
    s->latch = 1;
    arm7_update_irq(s);
}

/* ---------------- NAND controller (direct path) ---------------- */

static void nandc_update_irq(CXDNandState *s)
{
    bool lvl = (s->reg[0x410 / 4] && (s->reg[0x420 / 4] & 8)) ||
               (s->reg[0x720 / 4] && (s->reg[0x730 / 4] & 2));
    qemu_set_irq(s->nand_irq, lvl);
}

static uint64_t nreg_read(void *opaque, hwaddr off, unsigned size)
{
    CXDNandState *s = opaque;
    return s->reg[off / 4];
}

static void nreg_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    CXDNandState *s = opaque;
    switch (off) {
    case 0x000:
        if (v & 1) {            /* controller + NAND reset: done at once */
            s->reg[0x410 / 4] |= 0x2000;
        }
        break;
    case 0x410:                 /* write-back-to-clear status */
    case 0x720:
        s->reg[off / 4] &= ~v;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "nand-reg: wr 0x%04" HWADDR_PRIx " = 0x%08"
                      PRIx64 "\n", off, v);
        s->reg[off / 4] = v;
    }
    nandc_update_irq(s);
}

/* ---------------- arm7io ---------------- */

static uint64_t a7_read(void *opaque, hwaddr off, unsigned size)
{
    CXDNandState *s = opaque;
    return off == 0 ? s->latch : 0;
}

static void a7_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    CXDNandState *s = opaque;
    if (off == 0) {
        s->latch = v;
        arm7_update_irq(s);
    } else if (off == 4 && (v & 1)) {
        arm7_doorbell(s);
    } else {
        qemu_log_mask(LOG_UNIMP, "arm7io: wr 0x%03" HWADDR_PRIx " = 0x%08"
                      PRIx64 "\n", off, v);
    }
}

/* ---------------- clkrst3 ---------------- */

static uint64_t ck_read(void *opaque, hwaddr off, unsigned size)
{
    CXDNandState *s = opaque;
    if (off == 0xD4 || off == 0xD8) {
        return s->clkrst[0xD0 / 4];
    }
    return s->clkrst[off / 4];
}

static void ck_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    CXDNandState *s = opaque;
    uint32_t *st = &s->clkrst[0xD0 / 4];
    switch (off) {
    case 0xD8:                  /* assert resets */
        *st |= v;
        break;
    case 0xD4:                  /* release resets */
        if ((*st & 1) && (v & 1)) {
            s->arm7_booting = true;
        }
        if ((*st & 0x1F00) && (v & 0x1F00)) {
            s->reg[0x410 / 4] |= 0x2000;    /* NAND side out of reset */
            nandc_update_irq(s);
        }
        *st &= ~v;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "clkrst3: wr 0x%03" HWADDR_PRIx " = 0x%08"
                      PRIx64 "\n", off, v);
        s->clkrst[off / 4] = v;
    }
}

static const MemoryRegionOps nreg_ops = {
    .read = nreg_read, .write = nreg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};
static const MemoryRegionOps a7_ops = {
    .read = a7_read, .write = a7_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};
static const MemoryRegionOps ck_ops = {
    .read = ck_read, .write = ck_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/*
 * Controller state left by the bootloader = nand.ko init_reg_param from the
 * real camera's snapshot (VA 0x3f03bb4c, 50 words), applied the way
 * boss_hw_nandc_set_init_reg does it.
 */
static const uint32_t init_reg_param[50] = {
    0x00000000, 0x000001f4, 0x000001f4, 0x000001f4, 0x000001f4, 0x00000001,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000001,
    0x00000001, 0x00000001, 0x00001f1f, 0x00000b4d, 0x0000001a, 0x0000000c,
    0x00000000, 0x00000040, 0x00000001, 0x00001000, 0x00000100, 0x00000000,
    0x00000000, 0x00420010, 0x00000000, 0x00000000, 0x00000000, 0x00000008,
    0x00000003, 0x00000001, 0x0001a000, 0x00000000, 0x0000ffff, 0x00000001,
    0x00000000, 0x00000000, 0x00000000, 0x0000001a, 0x001f013b, 0x0000ffff,
    0x000000ec, 0x00000000, 0x00000010, 0x00000001, 0x00000800, 0x0000002e,
    0x00000005, 0x0000c350,
};

static void nand_reset(DeviceState *dev)
{
    CXDNandState *s = CXD_NAND(dev);
    memset(s->reg, 0, sizeof(s->reg));
    memset(s->clkrst, 0, sizeof(s->clkrst));
    for (int p = 0; p <= 52; p += 4) {
        s->reg[(0x10 + 4 * p) / 4] = init_reg_param[p / 4];
    }
    for (int p = 56; p <= 164; p += 4) {
        s->reg[(0x20 + 4 * p) / 4] = init_reg_param[p / 4];
    }
    s->reg[0x300 / 4] = init_reg_param[168 / 4];
    s->reg[0x3C0 / 4] = init_reg_param[172 / 4];
    s->reg[0x780 / 4] = init_reg_param[176 / 4];
    s->reg[0x6D0 / 4] = 8;
    s->reg[0x6E0 / 4] = 8;
    s->latch = 0;
    s->arm7_booting = false;
}

static void nand_realize(DeviceState *dev, Error **errp)
{
    CXDNandState *s = CXD_NAND(dev);
    if (!s->file) {
        s->mem = mmap(NULL, FILE_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANON, -1, 0);
        if (s->mem == MAP_FAILED) {
            error_setg_errno(errp, errno, "cxd-nand: no memory");
            return;
        }
        memset(s->mem, 0xff, DATA_SIZE + XTRA_SIZE);
        memset(s->mem + DATA_SIZE + XTRA_SIZE, 0, BMAP_SIZE);
        return;
    }
    int fd = open(s->file, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        error_setg_errno(errp, errno, "cxd-nand: cannot open %s", s->file);
        return;
    }
    struct stat st;
    fstat(fd, &st);
    bool fresh = st.st_size != (off_t)FILE_SIZE;
    if (fresh && ftruncate(fd, FILE_SIZE) < 0) {
        error_setg_errno(errp, errno, "cxd-nand: cannot size %s", s->file);
        close(fd);
        return;
    }
    s->mem = mmap(NULL, FILE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (s->mem == MAP_FAILED) {
        error_setg_errno(errp, errno, "cxd-nand: cannot map %s", s->file);
        return;
    }
    if (fresh) {
        memset(s->mem, 0xff, DATA_SIZE + XTRA_SIZE);   /* factory erased */
        memset(s->mem + DATA_SIZE + XTRA_SIZE, 0, BMAP_SIZE);
    }
}

static void nand_init(Object *obj)
{
    CXDNandState *s = CXD_NAND(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    memory_region_init_io(&s->reg_mr, obj, &nreg_ops, s, "nand-reg", 0x10000);
    memory_region_init_io(&s->arm7io_mr, obj, &a7_ops, s, "arm7io", 0x1000);
    memory_region_init_io(&s->clkrst_mr, obj, &ck_ops, s, "clkrst3", 0x1000);
    sysbus_init_mmio(sbd, &s->reg_mr);
    sysbus_init_mmio(sbd, &s->arm7io_mr);
    sysbus_init_mmio(sbd, &s->clkrst_mr);
    sysbus_init_irq(sbd, &s->nand_irq);
    sysbus_init_irq(sbd, &s->arm7_irq);
}

static const Property nand_props[] = {
    DEFINE_PROP_STRING("file", CXDNandState, file),
};

static void nand_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = nand_realize;
    device_class_set_legacy_reset(dc, nand_reset);
    device_class_set_props(dc, nand_props);
}

static const TypeInfo nand_info = {
    .name = TYPE_CXD_NAND,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CXDNandState),
    .instance_init = nand_init,
    .class_init = nand_class_init,
};

static void nand_register(void)
{
    type_register_static(&nand_info);
}
type_init(nand_register)

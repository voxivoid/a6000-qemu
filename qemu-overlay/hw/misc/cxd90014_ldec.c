/*
 * CXD90014 "BOSS" LDEC hardware LZ77 decoder, memory-to-memory mode (a5000).
 *
 * Used through /dev/ldec (initrd ldec.ko, ioctl 1) by libObj to unpack UI
 * images (libObj sub_5334CC "Decode LZ77 format").  Programming model from
 * ldec.ko (boss_hw_ldec_set / _process / _stop, ldec_ioctl):
 *
 *   regs PA 0xF2903000, IRQ 171
 *     +0x00  written 0 at setup [?]
 *     +0x04  1 = hold/reset, 0 = start (set: 1, program, 0)
 *     +0x0C  output size in 4 KiB pages
 *     +0x10  output physical address
 *     +0x24  1 = abort (boss_hw_ldec_stop)
 *     +0x28  0x7B, +0x2C 4, +0x30 0x7B at setup; the IRQ handler writes the
 *            status it read back to +0x30 (= acknowledge)  [?]
 *     +0x34  status: 1 = done, 4 = error (-EIO), else -EACCES
 *   input  PA 0xF0020000..0xF002FFFF: the compressed stream, pushed by the
 *          BOSS DMAC page by page (destination wraps every 64 KiB).
 *
 * The stream is the Sony LZ77 format of the kernel's lib/lz77 (also used by
 * the NAND ldec path, hw/block/cxd90014_nand.c).  The hardware's own end
 * condition is not known [?]: here decoding completes once every byte
 * received forms complete chunks and the output has reached the last page
 * ((pages-1)*4096 < out <= pages*4096 -- the caller sizes pages as
 * ceil(bytes/4096)).
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "system/address-spaces.h"

#define TYPE_CXD_LDEC "cxd90014-ldec"
OBJECT_DECLARE_SIMPLE_TYPE(LdecState, CXD_LDEC)

#define LDEC_MAX_IN   (8 << 20)
#define LDEC_MAX_OUT  (16 << 20)

struct LdecState {
    SysBusDevice parent_obj;
    MemoryRegion regs, win;
    uint32_t r[0x40];
    uint8_t *in;
    size_t inlen;
    bool running;
    qemu_irq irq;
};

#define R(o) (s->r[(o) / 4])

/*
 * Decode as much of in[0..inlen) as forms whole chunks.  Returns the output
 * length; *complete is true when the input ended exactly on a chunk
 * boundary.  Returns -1 on a malformed stream.
 */
static long lz77_stream(const uint8_t *in, size_t inlen, uint8_t *out,
                        size_t cap, bool *complete)
{
    static const uint8_t lens[16] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
                                     14, 15, 16, 32, 64};
    size_t i = 0, op = 0;
    *complete = false;
    while (i < inlen) {
        uint8_t type = in[i];
        if (type == 0x0f) {                 /* stored chunk */
            if (i + 4 > inlen) {
                return op;
            }
            uint32_t l = in[i + 2] | in[i + 3] << 8;
            if (i + 4 + l > inlen) {
                return op;
            }
            memcpy(out + op, in + i + 4, MIN(l, cap - op));
            op += MIN(l, cap - op);
            i += 4 + l;
            continue;
        }
        if (type != 0xf0) {
            return -1;
        }
        size_t j = i + 1, o = op;
        bool end = false;
        while (!end) {
            if (j >= inlen) {
                return op;                  /* chunk not complete yet */
            }
            uint8_t flags = in[j++];
            for (int b = 0; b < 8 && !end; b++) {
                if (flags >> b & 1) {
                    if (j + 2 > inlen) {
                        return op;
                    }
                    uint8_t d0 = in[j], d1 = in[j + 1];
                    j += 2;
                    uint32_t l = lens[d0 >> 4];
                    uint32_t bd = (d0 & 0xf) << 8 | d1;
                    if (bd == 0) {
                        end = true;
                        break;
                    }
                    for (uint32_t k = 0; k < l && o < cap; k++, o++) {
                        out[o] = o >= bd ? out[o - bd] : 0;
                    }
                } else {
                    if (j >= inlen) {
                        return op;
                    }
                    if (o < cap) {
                        out[o++] = in[j];
                    }
                    j++;
                }
            }
        }
        op = o;
        i = j;
    }
    *complete = true;
    return op;
}

static void ldec_update(LdecState *s)
{
    qemu_set_irq(s->irq, s->running == false && R(0x34) != 0);
}

static void ldec_finish(LdecState *s, uint32_t status)
{
    s->running = false;
    R(0x34) = status;
    ldec_update(s);
}

static void ldec_try(LdecState *s)
{
    uint32_t pages = R(0x0C);
    size_t cap = (size_t)pages * 4096;
    bool complete;

    if (!s->running || !pages || cap > LDEC_MAX_OUT) {
        return;
    }
    g_autofree uint8_t *out = g_malloc0(cap);
    long n = lz77_stream(s->in, s->inlen, out, cap, &complete);
    if (n < 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "cxd-ldec: bad stream at %zu bytes\n",
                      s->inlen);
        ldec_finish(s, 4);
        return;
    }
    if (complete && (size_t)n > cap - 4096) {
        address_space_write(&address_space_memory, R(0x10),
                            MEMTXATTRS_UNSPECIFIED, out, n);
        ldec_finish(s, 1);
    }
}

static uint64_t ldec_read(void *opaque, hwaddr off, unsigned size)
{
    LdecState *s = opaque;
    return off < sizeof(s->r) ? R(off) : 0;
}

static void ldec_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    LdecState *s = opaque;
    if (off >= sizeof(s->r)) {
        return;
    }
    switch (off) {
    case 0x04:
        if (v & 1) {                    /* hold / reset */
            s->running = false;
            R(0x34) = 0;
        } else if (R(0x04) & 1) {       /* released: start a new job */
            s->inlen = 0;
            s->running = true;
            R(0x34) = 0;
        }
        break;
    case 0x24:
        if (v & 1) {
            s->running = false;
        }
        break;
    case 0x30:                          /* acknowledge [?] */
        R(off) = v;
        qemu_set_irq(s->irq, 0);
        return;
    }
    R(off) = v;
    ldec_update(s);
}

static void win_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    LdecState *s = opaque;
    if (!s->running || s->inlen + size > LDEC_MAX_IN) {
        return;
    }
    for (unsigned i = 0; i < size; i++) {
        s->in[s->inlen++] = v >> (8 * i);
    }
    ldec_try(s);
}

static uint64_t win_read(void *opaque, hwaddr off, unsigned size)
{
    return 0;
}

static const MemoryRegionOps ldec_ops = {
    .read = ldec_read, .write = ldec_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static const MemoryRegionOps win_ops = {
    .read = win_read, .write = win_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void ldec_reset(DeviceState *dev)
{
    LdecState *s = CXD_LDEC(dev);
    memset(s->r, 0, sizeof(s->r));
    s->inlen = 0;
    s->running = false;
}

static void ldec_init(Object *obj)
{
    LdecState *s = CXD_LDEC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    memory_region_init_io(&s->regs, obj, &ldec_ops, s, "ldec", 0x100);
    memory_region_init_io(&s->win, obj, &win_ops, s, "ldec-in", 0x10000);
    sysbus_init_mmio(sbd, &s->regs);
    sysbus_init_mmio(sbd, &s->win);
    sysbus_init_irq(sbd, &s->irq);
    s->in = g_malloc(LDEC_MAX_IN);
}

static void ldec_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_legacy_reset(dc, ldec_reset);
}

static const TypeInfo ldec_info = {
    .name = TYPE_CXD_LDEC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(LdecState),
    .instance_init = ldec_init,
    .class_init = ldec_class_init,
};

static void ldec_register(void)
{
    type_register_static(&ldec_info);
}
type_init(ldec_register)

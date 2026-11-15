/* CXD90014 SIO controller: slave attachment for board-level models. */
#ifndef HW_MISC_CXD90014_SIO_H
#define HW_MISC_CXD90014_SIO_H

#include "hw/sysbus.h"

/*
 * Called when a transfer starts on the channel.  @buf holds the @len TX
 * bytes (from the +0x100 FIFO window); the slave overwrites it with the RX
 * bytes it shifts out at the same time (full-duplex SPI).
 */
typedef void (*CXDSioSlaveXfer)(void *opaque, int ch, uint8_t *buf, int len);

void cxd90014_sio_attach(DeviceState *sio, int ch, CXDSioSlaveXfer fn,
                         void *opaque);

/*
 * Same, but the slave also gets the channel's +0x04 word at the start of
 * the transfer (@mode).  Seen on ch2 (av-cam SIO server, docs/RE_NFC.md
 * section 1.2): bit29 = transmit, bit28 = receive [H, two modes observed].
 * A half-duplex slave needs it: TX and RX share the length range and the
 * FIFO keeps stale bytes.
 */
typedef void (*CXDSioSlaveXferDir)(void *opaque, int ch, uint32_t mode,
                                   uint8_t *buf, int len);

void cxd90014_sio_attach_dir(DeviceState *sio, int ch, CXDSioSlaveXferDir fn,
                             void *opaque);

#endif

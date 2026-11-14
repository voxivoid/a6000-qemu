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

#endif

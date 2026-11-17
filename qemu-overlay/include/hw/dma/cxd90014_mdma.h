/*
 * CXD90014 av-cam memory DMA: peripheral-paced channels.
 * A channel whose source or destination equals the registered peripheral
 * FIFO address (UART1 DR for the E-mount lens) does not copy memory; the
 * peer device produces / consumes the bytes (docs/reference/RE_LENS_EMOUNT.md 9.4).
 */
#ifndef HW_DMA_CXD90014_MDMA_H
#define HW_DMA_CXD90014_MDMA_H

#include "hw/qdev-core.h"

/* Called when a channel with dst == fifo starts: the bytes it sends. */
typedef void (*CxdMdmaPeriphTx)(void *opaque, const uint8_t *buf, size_t len);

/*
 * Called when the guest reads the armed RX channel's +0x04 (progress):
 * the peer may call cxd90014_mdma_periph_rx for bytes that are due.
 */
typedef void (*CxdMdmaPeriphPull)(void *opaque);

void cxd90014_mdma_set_periph(DeviceState *dev, uint32_t fifo,
                              uint32_t ns_per_byte,
                              CxdMdmaPeriphTx tx, CxdMdmaPeriphPull pull,
                              void *opaque);

/*
 * Feed bytes received by the peripheral into the armed channel whose
 * src == fifo.  Returns how many were taken (0 when nothing is armed).
 */
size_t cxd90014_mdma_periph_rx(DeviceState *dev, const uint8_t *buf,
                               size_t len);

#endif

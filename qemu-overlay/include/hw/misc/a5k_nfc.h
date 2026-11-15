/* a5000 board NFC dynamic tag (FeliCa Plug class) on SIO2 (docs/RE_NFC.md) */
#ifndef HW_MISC_A5K_NFC_H
#define HW_MISC_A5K_NFC_H

#include "hw/sysbus.h"
#include "chardev/char.h"

#define TYPE_A5K_NFC "a5k-nfc"

/*
 * Create the tag and wire it: SIO channel 2 of @sio, SW = P7.10 and
 * SEL = P7.14 ("out" lines of @gpio), RFDET -> P0.8 (input of @gpio),
 * data-ready IRQ -> @irq (GIC 125).  @chr (may be NULL) = "virtual phone".
 */
DeviceState *a5k_nfc_create(DeviceState *sio, DeviceState *gpio,
                            qemu_irq irq, Chardev *chr);

#endif

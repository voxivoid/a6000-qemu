/* CXD90014 ADC unit — analog inputs driven by the board (key ladders). */
#ifndef HW_MISC_CXD90014_ADC_H
#define HW_MISC_CXD90014_ADC_H

#include "hw/qdev-core.h"

/* Set the 10-bit level of channel ch (0..11) as the next scans read it. */
void cxd90014_adc_set(DeviceState *dev, int ch, uint16_t level);

#endif

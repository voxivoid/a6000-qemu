#ifndef HW_MISC_CXD90014_JOG_H
#define HW_MISC_CXD90014_JOG_H

#include "hw/qdev-core.h"

#define TYPE_CXD90014_JOG "cxd90014-jog"

/* Rotate the control wheel by n detents (positive = CW). */
void cxd90014_jog_step(DeviceState *dev, int n);

#endif

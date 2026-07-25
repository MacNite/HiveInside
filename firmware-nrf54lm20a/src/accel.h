/* accel.h — accelerometer readout.
 *
 * Auto-detects either the XIAO nRF54LM20A Sense on-board LSM6DS3TR-C IMU
 * (0x6A/0x6B) or a prototype-style external LIS3DH/LIS2DH12 (0x18/0x19) on
 * any enabled I²C bus, then reads one X/Y/Z acceleration sample in milli-g.
 */
#pragma once

#include "measurement.h"

/* Capture one acceleration sample into m (sets m->accel_ok). */
void accel_read(struct measurement *m);

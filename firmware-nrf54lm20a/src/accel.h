/* accel.h — vibration capture + FFT band analysis.
 *
 * Auto-detects either the XIAO nRF54LM20A Sense on-board LSM6DS3TR-C IMU
 * (0x6A/0x6B) or an external LIS3DH/LIS2DH12 (0x18/0x19) on any
 * enabled I²C bus, then samples ~2.5 s of acceleration at ~400 Hz. It reports
 * both the mean (static/orientation) X/Y/Z and the AC vibration reduced to the
 * shared swarm/fanning/activity bands (the shared ecosystem bands).
 */
#pragma once

#include "measurement.h"

/* Capture one vibration snapshot into m (sets m->accel_ok). */
void accel_read(struct measurement *m);

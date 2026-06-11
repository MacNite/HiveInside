// sensors_i2c.h — SHT40, LIS2DH12 and LPS22HB drivers on the shared I2C bus.
#pragma once

#include "measurement.h"

// Initialise the I2C bus and all enabled sensors. Returns true if at least one
// sensor came up. Individual sensor presence is reflected per-field in the
// Measurement struct.
bool sensorsInit();

// Take a one-shot reading from every enabled I2C sensor into `m`.
void sensorsRead(Measurement& m);

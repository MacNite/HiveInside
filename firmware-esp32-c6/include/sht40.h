// sht40.h — SHT40 temperature + humidity over I2C (Adafruit SHT4x driver).
#pragma once

#include "config.h"
#include "measurement.h"

#if ENABLE_SHT40

namespace sht40 {
// Probe the sensor on the shared Wire bus. Returns true if present.
bool begin();
// Fill m.temp_c / m.humidity_pct / m.sht_ok. Safe to call every cycle.
void read(Measurement& m);
} // namespace sht40

#endif // ENABLE_SHT40

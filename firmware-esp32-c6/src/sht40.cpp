// sht40.cpp — SHT40 climate read via the Adafruit SHT4x library.
#include "sht40.h"

#if ENABLE_SHT40

#include <Wire.h>
#include <Adafruit_SHT4x.h>

namespace sht40 {

static Adafruit_SHT4x sht;
static bool present = false;

bool begin() {
  // Adafruit_SHT4x::begin() probes the fixed I2C address on the given bus.
  present = sht.begin(&Wire);
  if (present) {
    // Low precision keeps the SHT40 conversion short and lowers per-cycle energy.
    sht.setPrecision(SHT4X_LOW_PRECISION);
    sht.setHeater(SHT4X_NO_HEATER);
    Serial.println("[SHT40] present (low precision)");
  } else {
    Serial.println("[SHT40] not found");
  }
  return present;
}

void read(Measurement& m) {
  if (!present) {
    m.sht_ok = false;
    return;
  }
  sensors_event_t humidity, temp;
  if (sht.getEvent(&humidity, &temp)) {
    m.temp_c = temp.temperature;
    m.humidity_pct = humidity.relative_humidity;
    m.sht_ok = true;
    Serial.printf("[SHT40] %.2f C  %.1f %%RH\n", m.temp_c, m.humidity_pct);
  } else {
    m.sht_ok = false;
    Serial.println("[SHT40] read failed");
  }
}

} // namespace sht40

#endif // ENABLE_SHT40

// battery.cpp — LiPo voltage estimate from an external divider on an ADC pin.
#include "battery.h"

#if ENABLE_BATTERY

namespace battery {

void begin() {
  analogReadResolution(12); // 0..4095
  // ~11 dB attenuation lets the C6 ADC read up to ~2.5 V at the pin, so the
  // divider should keep V(pin) below that across the LiPo range.
  analogSetPinAttenuation(PIN_VBAT_ADC, ADC_11db);
}

void read(Measurement& m) {
  // Average a handful of samples to settle ADC noise.
  uint32_t acc = 0;
  const int N = 16;
  for (int i = 0; i < N; i++) acc += analogReadMilliVolts(PIN_VBAT_ADC);
  float vPin = (acc / (float)N) / 1000.0f; // V at the ADC pin
  float vBat = vPin * VBAT_DIVIDER;

  m.battery_v = vBat;
  float pct = (vBat - VBAT_EMPTY_V) / (VBAT_FULL_V - VBAT_EMPTY_V) * 100.0f;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  m.battery_pct = (uint8_t)(pct + 0.5f);
  m.battery_ok = true;

  Serial.printf("[BATT] %.3f V (pin %.3f V) ~%u%%\n", vBat, vPin, m.battery_pct);
}

} // namespace battery

#endif // ENABLE_BATTERY

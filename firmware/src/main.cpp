// main.cpp — HiveInside top-level loop.
//
// Cycle: wake -> read sensors -> capture mic bands -> read battery ->
//        advertise BTHome -> sleep. A button press triggers an immediate
//        identify advertisement and (long press) factory reset / key rotation.
//
// Power model: between cycles the nRF52840 enters low-power sleep. On the
// Nice!Nano prototype we use delay-based timing for simplicity; the production
// build should switch to a System-ON sleep with an RTC wake (see TODO).
#include <Arduino.h>

#include "config.h"
#include "measurement.h"
#include "sensors_i2c.h"
#include "mic_i2s.h"
#include "ble_bthome.h"
#include "button.h"

static uint32_t lastMeasure = 0;

// Read battery voltage via ADC and map to a rough percentage for a 3V coin /
// LiPo cell. Calibrate against the actual divider on the final board.
static void readBattery(Measurement& m) {
  analogReadResolution(12);
  uint32_t raw = analogRead(PIN_VBAT_ADC);
  // Nice!Nano has a 1/2 divider and 3.6V reference by default; adjust for the
  // custom PCB divider. This is a placeholder mapping.
  float v = (raw / 4095.0f) * 3.6f * 2.0f;
  m.battery_v = v;

  // crude coin-cell curve: 3.0V = 100%, 2.0V = 0%
  float pct = (v - 2.0f) / (3.0f - 2.0f) * 100.0f;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  m.battery_pct = (uint8_t)pct;
}

static void runMeasurementCycle() {
  Measurement m;

  sensorsRead(m);
  micCaptureBands(m.mic);
  readBattery(m);

  bleAdvertise(m);
}

void setup() {
  // Serial is only useful on the USB prototype; harmless if unused.
  Serial.begin(115200);

  buttonInit();
  sensorsInit();
  bleInit();

  // First reading right away so the device shows up quickly in HA.
  runMeasurementCycle();
  lastMeasure = millis();
}

void loop() {
  // Handle button events promptly.
  switch (buttonPoll()) {
    case ButtonEvent::ShortPress:
      // Identify: send an immediate advertisement.
      runMeasurementCycle();
      lastMeasure = millis();
      break;
    case ButtonEvent::LongPress:
      // TODO: factory reset / rotate BTHome encryption key, then re-advertise.
      runMeasurementCycle();
      lastMeasure = millis();
      break;
    case ButtonEvent::None:
    default:
      break;
  }

  // Periodic measurement.
  if (millis() - lastMeasure >= MEASURE_INTERVAL_MS) {
    runMeasurementCycle();
    lastMeasure = millis();
  }

  // TODO: replace busy-wait with System-ON sleep + RTC/GPIOTE wake to hit the
  // ~µA sleep currents the hardware is capable of. For prototype bring-up a
  // light delay keeps the loop responsive to the button.
  delay(50);
}

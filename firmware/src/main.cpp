// main.cpp — HiveInside ESP32-C6 prototype top level.
//
// Cycle: read SHT40 (temp/humidity) -> capture LIS3DH vibration FFT -> capture
// INMP441 acoustic FFT -> read battery -> publish over BLE (BTHome advertising
// or GATT, per BLE_MODE in config.h).
//
// This prototype runs a simple millis()-based loop and keeps BLE up between
// cycles (the radio is the point of the device). A deep-sleep variant for the
// advertising mode is straightforward to add later (esp_deep_sleep + timer
// wake); it is left out here so GATT clients can stay connected.
#include <Arduino.h>
#include <Wire.h>

#include "config.h"
#include "measurement.h"
#include "sht40.h"
#include "accel.h"
#include "mic.h"
#include "battery.h"
#include "ble_link.h"

static uint32_t lastMeasure = 0;
static uint8_t packetId = 0;
static int lastButton = HIGH;

static void led(bool on) {
#if defined(PIN_LED)
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, (LED_ACTIVE_LOW ? !on : on));
#endif
}

static void runCycle() {
  led(true);
  Measurement m;
  m.packet_id = packetId++;

#if ENABLE_SHT40
  sht40::read(m);
#endif
#if ENABLE_ACCEL
  accel::read(m);
#endif
#if ENABLE_MIC
  mic::read(m);
#endif
#if ENABLE_BATTERY
  battery::read(m);
#endif

  ble::publish(m);
  led(false);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n[HiveInside] ESP32-C6 prototype fw %s | BLE mode: %s\n",
                HIVEINSIDE_FW_VERSION, ble::modeName());

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(400000);

#if ENABLE_SHT40
  sht40::begin();
#endif
#if ENABLE_ACCEL
  accel::begin();
#endif
#if ENABLE_MIC
  mic::begin();
#endif
#if ENABLE_BATTERY
  battery::begin();
#endif

  ble::begin();

  runCycle(); // publish immediately so the device shows up quickly
  lastMeasure = millis();
}

void loop() {
  // BOOT button: a short press forces an immediate measurement/publish.
  int b = digitalRead(PIN_BUTTON);
  if (lastButton == HIGH && b == LOW) {
    delay(30); // debounce
    if (digitalRead(PIN_BUTTON) == LOW) {
      Serial.println("[BTN] manual publish");
      runCycle();
      lastMeasure = millis();
    }
  }
  lastButton = b;

  if (millis() - lastMeasure >= MEASURE_INTERVAL_MS) {
    runCycle();
    lastMeasure = millis();
  }

  delay(20);
}

// main.cpp — HiveInside ESP32-C6 prototype top level.
//
// Cycle: read SHT40 (temp/humidity) -> capture LIS3DH vibration FFT -> capture
// INMP441 acoustic FFT -> read battery -> publish over BLE (BTHome advertising
// or GATT, per BLE_MODE in config.h).
//
// Deep sleep (DEEP_SLEEP_ENABLED=1, default OFF):
//   After each cycle the device advertises for ADV_BURST_MS then sleeps for
//   MEASURE_INTERVAL_MS. Wake sources: timer (always) and optionally a button
//   on PIN_WAKE_BUTTON (must be GPIO0–7 on ESP32-C6; GPIO9 is not LP-capable).
//
// Pairing mode (long press >= PAIRING_LONG_PRESS_MS on PIN_BUTTON):
//   Suppresses sleep for PAIRING_WINDOW_MS and blinks the LED fast so the user
//   knows the device is discoverable. Short press still forces an immediate
//   measurement. Pairing itself is done on the HiveScale provisioning portal.
#include <Arduino.h>
#include <Wire.h>

#if DEEP_SLEEP_ENABLED
#include "esp_sleep.h"
#endif

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
static unsigned long buttonPressedAt = 0;
static bool buttonLongFired = false;

static void led(bool on) {
#if defined(PIN_LED)
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, (LED_ACTIVE_LOW ? !on : on));
#endif
}

// One-shot I2C probe so a bring-up failure is obvious: if neither sensor ACKs
// here, the later i2c_master_transmit_receive errors are a wiring problem —
// not a firmware bug.
static void i2cScan() {
  Serial.println("[I2C] scanning bus...");
  uint8_t found = 0;
  for (uint8_t addr = 0x08; addr < 0x78; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[I2C]   device at 0x%02X\n", addr);
      found++;
    }
  }
  if (found == 0)
    Serial.println("[I2C]   none found — check wiring, power and 4.7k pull-ups "
                   "on SDA/SCL");
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

#if DEEP_SLEEP_ENABLED
static void enterDeepSleep() {
  Serial.printf("[SLEEP] Entering deep sleep for %lu s\n", MEASURE_INTERVAL_MS / 1000UL);
  Serial.flush();
  ble::shutdown();
  Wire.end();
#if PIN_WAKE_BUTTON >= 0
  // Button wake: only LP IO pins (GPIO0–7) work on ESP32-C6.
  esp_sleep_enable_ext1_wakeup(1ULL << PIN_WAKE_BUTTON, ESP_EXT1_WAKEUP_ANY_LOW);
#endif
  esp_sleep_enable_timer_wakeup((uint64_t)MEASURE_INTERVAL_MS * 1000ULL);
  esp_deep_sleep_start();
}
#endif

void setup() {
  Serial.begin(115200);
  delay(200);

#if DEEP_SLEEP_ENABLED
  esp_sleep_wakeup_cause_t wakeReason = esp_sleep_get_wakeup_cause();
  bool wokeFromButton = (wakeReason == ESP_SLEEP_WAKEUP_EXT1);
  const char* wakeStr = wokeFromButton        ? "button"
                        : (wakeReason == ESP_SLEEP_WAKEUP_TIMER) ? "timer"
                        : "power-on";
  Serial.printf("\n[HiveInside] fw %s | BLE: %s | wake: %s\n",
                HIVEINSIDE_FW_VERSION, ble::modeName(), wakeStr);
#else
  Serial.printf("\n[HiveInside] ESP32-C6 prototype fw %s | BLE mode: %s\n",
                HIVEINSIDE_FW_VERSION, ble::modeName());
#endif

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(I2C_CLOCK_HZ);
  i2cScan();

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

  runCycle();  // publish immediately so the device shows up right after boot/wake
  lastMeasure = millis();

#if DEEP_SLEEP_ENABLED
  if (wokeFromButton) {
    Serial.println("[SLEEP] Button wake: entering pairing mode");
    ble::enterPairingMode();
  }
#endif
}

void loop() {
  int b = digitalRead(PIN_BUTTON);

  // Falling edge: record press start (with debounce).
  if (b == LOW && lastButton == HIGH) {
    delay(30);
    b = digitalRead(PIN_BUTTON);
    if (b == LOW) {
      buttonPressedAt = millis();
      buttonLongFired = false;
    }
  }

  // Long hold: enter pairing mode.
  if (b == LOW && !buttonLongFired && buttonPressedAt > 0 &&
      millis() - buttonPressedAt >= PAIRING_LONG_PRESS_MS) {
    buttonLongFired = true;
    Serial.println("[BTN] long press: entering pairing mode");
    ble::enterPairingMode();
  }

  // Rising edge without a preceding long-press: short press = immediate publish.
  if (b == HIGH && lastButton == LOW && !buttonLongFired) {
    Serial.println("[BTN] short press: manual publish");
    runCycle();
    lastMeasure = millis();
  }

  lastButton = b;

  // Periodic measurement.
  if (millis() - lastMeasure >= MEASURE_INTERVAL_MS) {
    runCycle();
    lastMeasure = millis();
  }

  // Fast LED blink (5 Hz) while the pairing window is open.
#if defined(PIN_LED)
  if (ble::isPairingActive()) {
    led((millis() / 100) % 2);
  }
#endif

#if DEEP_SLEEP_ENABLED
  // Sleep once the advertising window has elapsed — unless pairing is active.
  unsigned long windowMs = ble::isPairingActive() ? PAIRING_WINDOW_MS : ADV_BURST_MS;
  if (millis() - lastMeasure >= windowMs) {
    enterDeepSleep();
  }
#endif

  delay(20);
}

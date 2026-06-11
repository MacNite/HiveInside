// config.h — HiveInside compile-time configuration and pin map.
//
// Pin assignments below are for the Nice!Nano v2 prototype. The production
// Ebyte E73 PCB uses the same logical signals; override the GPIO numbers under
// HIVEINSIDE_CUSTOM_PCB once the board pads are finalised against the Ebyte
// pinout (module pads 12=P0.26, 14=P0.06, 35=P0.24, 34=P0.22, 30=P0.17,
// 33=P0.13 in the current wiring draft — see docs/wiring.md).
#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Identity / advertising
// ---------------------------------------------------------------------------
#define HIVEINSIDE_FW_VERSION   "0.1.0"
#define BTHOME_DEVICE_NAME      "HiveInside"   // shows up in Home Assistant

// How often to wake, sample, advertise, and go back to sleep.
#define MEASURE_INTERVAL_MS     (5UL * 60UL * 1000UL)   // 5 minutes

// BLE advertising burst length each cycle.
#define ADV_DURATION_MS         3000
#define ADV_INTERVAL_MS         100

// ---------------------------------------------------------------------------
// Pin map (Nice!Nano v2 silk labels in comments)
// ---------------------------------------------------------------------------
#if defined(HIVEINSIDE_CUSTOM_PCB)
  // ---- Ebyte E73 custom PCB ----
  #define PIN_I2C_SDA   26   // P0.26  (module pad 12)
  #define PIN_I2C_SCL   6    // P0.06  (module pad 14)
  #define PIN_I2S_SCK   24   // P0.24  (module pad 35)
  #define PIN_I2S_WS    22   // P0.22  (module pad 34)
  #define PIN_I2S_SD    17   // P0.17  (module pad 30)
  #define PIN_MIC_EN    13   // P0.13  (module pad 33) -> MOSFET gate, LOW=on
  #define PIN_BUTTON    2    // P0.02  pairing/identify button to GND
  #define PIN_VBAT_ADC  4    // P0.04/AIN2 battery divider (or VDDHDIV5)
#else
  // ---- Nice!Nano v2 prototype ----
  #define PIN_I2C_SDA   17   // "017"
  #define PIN_I2C_SCL   20   // "020"
  #define PIN_I2S_SCK   22   // "022"
  #define PIN_I2S_WS    24   // "024"
  #define PIN_I2S_SD    8    // "D08"
  #define PIN_MIC_EN    6    // "D06"  -> MOSFET gate (or direct enable), LOW=on
  #define PIN_BUTTON    9    // "009"  pairing/identify button to GND
  #define PIN_VBAT_ADC  31   // "031"/AIN7 battery sense (Nice!Nano has divider)
#endif

// Active-low button (internal pull-up, press = LOW)
#define BUTTON_ACTIVE_LOW       1
// Long-press threshold for factory reset / key rotation (ms)
#define BUTTON_LONGPRESS_MS     5000

// ---------------------------------------------------------------------------
// I2C sensor addresses
// ---------------------------------------------------------------------------
#define ADDR_LIS2DH12   0x18   // SA0 = GND
#define ADDR_SHT40      0x44   // fixed for SHT40-AD1B
#define ADDR_LPS22HB    0x5C   // SA0 = GND

// ---------------------------------------------------------------------------
// Microphone / FFT (mirrors HiveScale band layout)
// ---------------------------------------------------------------------------
#define MIC_SAMPLE_RATE_HZ      16000
#define FFT_SAMPLE_COUNT        2048   // power of two; ~7.8 Hz resolution
#define MIC_STARTUP_MS          20     // ICS-43434 datasheet start-up time

// FFT band edges (Hz) — keep aligned with HiveScale insights.py
#define BAND_SUBBASS_LO   50
#define BAND_SUBBASS_HI   150
#define BAND_HUM_LO       150
#define BAND_HUM_HI       300
#define BAND_PIPING_LO    300
#define BAND_PIPING_HI    550
#define BAND_STRESS_LO    550
#define BAND_STRESS_HI    1500
#define BAND_HIGH_LO      1500
#define BAND_HIGH_HI      3000

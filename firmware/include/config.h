// config.h — HiveInside ESP32-C6 prototype: pins, sensor settings, BLE mode.
//
// This is a *prototype* firmware that brings the HiveInside sensor suite up on
// cheap, widely-available parts before the nRF52840 production board:
//
//   * ESP32-C6 SuperMini   — MCU + BLE 5 (Bluetooth LE) radio
//   * LIS3DH               — 3-axis accelerometer (I2C)        -> swarm vibration
//   * SHT40                — temperature + humidity (I2C)
//   * INMP441              — I2S MEMS microphone               -> acoustic FFT
//
// All values can be overridden from platformio.ini build_flags so you can wire
// the breakouts to whatever pins are convenient without editing this file.
#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Identity / timing
// ---------------------------------------------------------------------------
#ifndef HIVEINSIDE_FW_VERSION
#define HIVEINSIDE_FW_VERSION "0.1.0-esp32c6"
#endif

// BLE local name — what Home Assistant / nRF Connect shows. Keep it short so it
// fits in the advertisement alongside the BTHome service data.
#ifndef BLE_DEVICE_NAME
#define BLE_DEVICE_NAME "HiveInside"
#endif

// How often to wake, sample all sensors and (re)publish over BLE.
#ifndef MEASURE_INTERVAL_MS
#define MEASURE_INTERVAL_MS (5UL * 60UL * 1000UL) // 5 minutes
#endif

// ---------------------------------------------------------------------------
// BLE MODE — the headline "firmware variable" the prototype is built around.
//
//   BLE_MODE_ADVERTISING (0): connectionless broadcast. Each cycle the device
//       advertises a BTHome v2 service-data payload (temp / humidity / battery /
//       a curated set of vibration + acoustic RMS values) plus a compact
//       manufacturer-specific blob carrying the full measurement struct
//       (all FFT bands). Lowest power, no pairing, native Home Assistant
//       discovery. The radio is only on for ADV_BURST_MS each cycle.
//
//   BLE_MODE_GATT (1): connectable GATT server. The device stays advertising as
//       connectable; a central (phone / laptop / gateway) connects and reads or
//       subscribes to characteristics. Exposes the standard Battery +
//       Environmental-Sensing services for generic clients, plus a custom
//       HiveInside service whose JSON characteristic carries the complete
//       measurement (every FFT band, RMS, peak). Higher power, richer access.
//
// Select at build time with -DBLE_MODE=BLE_MODE_GATT in platformio.ini
// (defaults to advertising here).
// ---------------------------------------------------------------------------
#define BLE_MODE_ADVERTISING 0
#define BLE_MODE_GATT 1

#ifndef BLE_MODE
#define BLE_MODE BLE_MODE_ADVERTISING
#endif

// Advertising-mode radio burst per cycle (ms). Long enough for a scanner to
// catch a couple of packets; short enough to keep the average current low.
#ifndef ADV_BURST_MS
#define ADV_BURST_MS 3000
#endif

// ---------------------------------------------------------------------------
// Pin map — ESP32-C6 SuperMini (override any of these in platformio.ini).
//
// ADC1 lives on GPIO0..GPIO6 on the C6, so the battery sense pin must be one of
// those; I2C and I2S can be routed to any GPIO via the IO-MUX/GPIO matrix.
// The defaults below avoid the strapping pins (GPIO8/GPIO9 = boot LED / BOOT
// button) for sensor signals and reuse the on-board BOOT button for "identify".
// ---------------------------------------------------------------------------
#ifndef PIN_I2C_SDA
#define PIN_I2C_SDA 6
#endif
#ifndef PIN_I2C_SCL
#define PIN_I2C_SCL 7
#endif

// I2C bus clock. Default to the conservative 100 kHz "standard mode" for
// breadboard bring-up: the ESP32-C6's internal pull-ups are weak (~45 kΩ), so
// at 400 kHz over long jumper wires (or without the recommended external
// 4.7 kΩ pull-ups) the rising edges are too slow and the IDF 5.x I2C-NG driver
// reports i2c_master_transmit_receive failures (ESP_ERR_INVALID_STATE / 259).
// Once the bus is wired with proper pull-ups, raise this to 400000 for faster
// accelerometer sampling: -DI2C_CLOCK_HZ=400000.
#ifndef I2C_CLOCK_HZ
#define I2C_CLOCK_HZ 100000
#endif

// INMP441 I2S (SD has a 100k pull-down on most breakouts; L/R tied to GND = left)
#ifndef PIN_I2S_BCLK
#define PIN_I2S_BCLK 2
#endif
#ifndef PIN_I2S_WS
#define PIN_I2S_WS 3
#endif
#ifndef PIN_I2S_SD
#define PIN_I2S_SD 4
#endif

// On-board BOOT button (active-low). Short press = publish now / connectable.
#ifndef PIN_BUTTON
#define PIN_BUTTON 9
#endif

// On-board LED (active-low on most SuperMini clones). Used as a heartbeat.
#ifndef PIN_LED
#define PIN_LED 8
#endif
#ifndef LED_ACTIVE_LOW
#define LED_ACTIVE_LOW 1
#endif

// ---------------------------------------------------------------------------
// Battery monitoring (optional). See docs — the ESP32-C6 SuperMini has NO
// on-board charger or battery-sense divider on its BAT pads, so monitoring
// needs an external resistor divider from BAT+ to an ADC1 GPIO.
// ---------------------------------------------------------------------------
#ifndef ENABLE_BATTERY
#define ENABLE_BATTERY 1
#endif

// ADC1-capable GPIO the external divider feeds (GPIO0..GPIO6 on the C6).
#ifndef PIN_VBAT_ADC
#define PIN_VBAT_ADC 1
#endif

// Divider ratio Vbat/Vadc. 2.0 = two equal resistors (e.g. 100k/100k).
#ifndef VBAT_DIVIDER
#define VBAT_DIVIDER 2.0f
#endif

// Single-cell LiPo curve endpoints for the rough percentage estimate.
#ifndef VBAT_FULL_V
#define VBAT_FULL_V 4.20f
#endif
#ifndef VBAT_EMPTY_V
#define VBAT_EMPTY_V 3.30f
#endif

// ---------------------------------------------------------------------------
// SHT40 temperature + humidity
// ---------------------------------------------------------------------------
#ifndef ENABLE_SHT40
#define ENABLE_SHT40 1
#endif
// 0x44 is the fixed address for SHT40-AD1B; the -BD1B variant is 0x45.
#ifndef SHT40_ADDR
#define SHT40_ADDR 0x44
#endif

// ---------------------------------------------------------------------------
// LIS3DH accelerometer
// ---------------------------------------------------------------------------
#ifndef ENABLE_ACCEL
#define ENABLE_ACCEL 1
#endif

// 0x18 (SDO/SA0 -> GND, default on the breakout) or 0x19 (SDO/SA0 -> VCC).
#ifndef LIS3DH_ADDR
#define LIS3DH_ADDR 0x18
#endif
#ifndef LIS3DH_ODR_HZ
#define LIS3DH_ODR_HZ 400
#endif
#ifndef LIS3DH_RANGE_G
#define LIS3DH_RANGE_G 2
#endif
// Samples fed into the vibration FFT (power of two). 1024 @ 400 Hz ≈ 2.5 s,
// 0.39 Hz/bin — fine resolution for the 8–30 Hz swarm band.
#ifndef LIS3DH_SAMPLE_COUNT
#define LIS3DH_SAMPLE_COUNT 1024
#endif

// Vibration analysis bands (Hz), identical to HiveScale so a value means the
// same thing across the ecosystem (see HiveScale firmware/include/accel.h).
#define ACC_BAND_SWARM_LO 8     //  8–30 Hz  Ramsey et al. 2020 pre-swarm signal
#define ACC_BAND_SWARM_HI 30
#define ACC_BAND_FANNING_LO 30  // 30–100 Hz ventilation / fanning
#define ACC_BAND_FANNING_HI 100
#define ACC_BAND_ACTIVITY_LO 100 // 100–200 Hz general worker activity
#define ACC_BAND_ACTIVITY_HI 200

// ---------------------------------------------------------------------------
// INMP441 microphone + acoustic FFT
// ---------------------------------------------------------------------------
#ifndef ENABLE_MIC
#define ENABLE_MIC 1
#endif

#ifndef MIC_SAMPLE_RATE
#define MIC_SAMPLE_RATE 16000
#endif
// Frames captured for RMS/peak each cycle (0.5 s at 16 kHz).
#ifndef MIC_SAMPLE_FRAMES
#define MIC_SAMPLE_FRAMES 8000
#endif
// Samples fed into the acoustic FFT (power of two). 2048 @ 16 kHz ≈ 7.8 Hz/bin.
#ifndef MIC_FFT_SAMPLE_COUNT
#define MIC_FFT_SAMPLE_COUNT 2048
#endif
#ifndef MIC_I2S_PORT
#define MIC_I2S_PORT I2S_NUM_0
#endif

// Acoustic FFT bands (Hz) — kept aligned with HiveScale insights.
#define MIC_BAND_SUBBASS_LO 50   //   50–150 Hz structural / low rumble
#define MIC_BAND_SUBBASS_HI 150
#define MIC_BAND_HUM_LO 150      //  150–300 Hz normal colony hum
#define MIC_BAND_HUM_HI 300
#define MIC_BAND_PIPING_LO 300   //  300–550 Hz queen piping / tooting
#define MIC_BAND_PIPING_HI 550
#define MIC_BAND_STRESS_LO 550   //  550–1500 Hz agitated / robbing
#define MIC_BAND_STRESS_HI 1500
#define MIC_BAND_HIGH_LO 1500    // 1500–3000 Hz harmonic overtones
#define MIC_BAND_HIGH_HI 3000

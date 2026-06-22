// config.h — HiveInside XIAO ESP32-C6 prototype: pins, sensor settings, BLE mode.
//
// This is a *prototype* firmware that brings the HiveInside sensor suite up on
// cheap, widely-available parts before the XIAO nRF52840 final board:
//
//   * Seeed XIAO ESP32-C6  — MCU + BLE 5 (Bluetooth LE) radio, USB-C, LiPo charger
//   * LIS3DH               — 3-axis accelerometer (I2C)        -> swarm vibration
//   * SHT40                — temperature + humidity (I2C)
//   * INMP441              — I2S MEMS microphone               -> acoustic FFT
//
// Default pins use the XIAO's labelled header pads (D4/D5 = I2C, D6-D8 = I2S,
// A0 = battery sense). All values can be overridden from platformio.ini
// build_flags so you can wire the breakouts to whatever pins are convenient.
#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Identity / timing
// ---------------------------------------------------------------------------
#ifndef HIVEINSIDE_FW_VERSION
#define HIVEINSIDE_FW_VERSION "0.3.4"
#endif

// ---------------------------------------------------------------------------
// Firmware-over-BLE (OTA). HiveScale (the WiFi node) downloads a new HiveInside
// image and streams it into this device's OTA GATT characteristics; the device
// writes it to the inactive OTA slot, verifies an end-to-end CRC-32 and reboots
// into it. Requires a dual-OTA partition table (board_build.partitions in
// platformio.ini). See docs/ota-over-ble.md.
//
// While an OTA is RECEIVING (or a verified image is pending reboot) the device
// suppresses deep sleep so the transfer can complete.
// ---------------------------------------------------------------------------
#ifndef HIVEINSIDE_OTA_ENABLED
#define HIVEINSIDE_OTA_ENABLED 1
#endif

// BLE local name — what nRF Connect / a central shows. Keep it short so it fits
// in the connectable advertisement alongside the service UUID.
#ifndef BLE_DEVICE_NAME
#define BLE_DEVICE_NAME "HiveInside"
#endif

// How often to wake, sample all sensors and (re)publish over BLE.
#ifndef MEASURE_INTERVAL_MS
#define MEASURE_INTERVAL_MS (5UL * 60UL * 1000UL) // 5 minutes
#endif

// ---------------------------------------------------------------------------
// BLE transport — connectable GATT server.
//
// The device stays advertising as connectable; a central (HiveScale / phone /
// laptop / gateway) connects and reads or subscribes to characteristics. It
// exposes the standard Battery (0x180F) + Environmental-Sensing (0x181A)
// services for generic clients, plus a custom HiveInside service whose JSON
// characteristic carries the complete measurement (every FFT band, RMS, peak).
// ---------------------------------------------------------------------------

// Connectable awake window per cycle (ms): the post-publish awake window before
// deep sleep when DEEP_SLEEP_ENABLED=1 — long enough for a central to discover
// and connect (HiveScale's 6 s scan overlaps with this). When wake
// synchronisation is active this is superseded by SYNC_LISTEN_MS.
#ifndef CONNECT_WINDOW_MS
#define CONNECT_WINDOW_MS 8000  // 8 s: safely covers HiveScale's 6 s scan window
#endif

// ---------------------------------------------------------------------------
// Deep sleep (timer-based wake). DISABLED by default.
//
// When enabled, the device wakes, runs one measurement cycle, stays connectable
// for CONNECT_WINDOW_MS, then sleeps for MEASURE_INTERVAL_MS. Total awake time
// per cycle ≈ sensor-read time (~3–5 s) + CONNECT_WINDOW_MS.
//
// IMPORTANT — GPIO wake-from-sleep on ESP32-C6:
//   Only LP IO pins (GPIO0–7) can wake the chip from deep sleep via ext1.
//   GPIO9 (the on-board BOOT button) is NOT an LP IO pin and CANNOT be used
//   as a wake source. To enable button-from-sleep, wire a separate button to
//   an LP-capable pad — on the XIAO that's D0–D2 (GPIO0–2) — and set
//   PIN_WAKE_BUTTON to that GPIO number.
//   PIN_WAKE_BUTTON = -1 (default) disables button-from-sleep wakeup.
// ---------------------------------------------------------------------------
#ifndef DEEP_SLEEP_ENABLED
#define DEEP_SLEEP_ENABLED 0
#endif

// LP-capable GPIO for deep-sleep button wake (GPIO0–7 on ESP32-C6 only).
// -1 = disabled (timer-only wake).
#ifndef PIN_WAKE_BUTTON
#define PIN_WAKE_BUTTON (-1)
#endif

// ---------------------------------------------------------------------------
// Pairing mode — activated by a long button press (PAIRING_LONG_PRESS_MS).
//
// During the pairing window the device suppresses deep sleep so HiveScale's
// provisioning portal can scan, discover its MAC address and complete the sync.
// The device is already advertising as connectable; the pairing window just
// keeps it awake. A fast LED blink indicates the window is open.
//
// 15 minutes gives HiveScale ample time to scan, connect, read the first
// measurement and write the initial wake-sync schedule.
// ---------------------------------------------------------------------------
#ifndef PAIRING_WINDOW_MS
#define PAIRING_WINDOW_MS (15UL * 60UL * 1000UL)   // 15 min visible for pairing
#endif

#ifndef PAIRING_LONG_PRESS_MS
#define PAIRING_LONG_PRESS_MS 3000           // hold 3 s to enter pairing mode
#endif

// ---------------------------------------------------------------------------
// Wake synchronisation (HiveScale is the schedule master).
//
// When HIVEINSIDE_SYNC_ENABLED, HiveScale writes a uint32 LE
// "sleep this many seconds" value to a writable characteristic during the
// connection it already makes each cycle. On the next deep sleep HiveInside uses
// that value instead of MEASURE_INTERVAL_MS, so it wakes just before HiveScale's
// next scan instead of staying connectable continuously. If no value is written during a
// wake (e.g. HiveScale missed the connection), it falls back to
// MEASURE_INTERVAL_MS so the device never sleeps indefinitely.
//
// Because the deep-sleep timer drifts (±5–10% on the internal RC oscillator) and
// HiveScale's hint already subtracts a lead, the device must stay awake and
// connectable for SYNC_LISTEN_MS after each wake to be sure it overlaps
// HiveScale's scan + connect. This replaces CONNECT_WINDOW_MS as the awake window
// when sync is active; once the central connects, writes the next hint and
// disconnects, the device sleeps immediately without waiting out the window.
#ifndef HIVEINSIDE_SYNC_ENABLED
#define HIVEINSIDE_SYNC_ENABLED 1
#endif

#ifndef SYNC_LISTEN_MS
// 150 s awake/connectable window per synced wake. Sized — together with
// HiveScale's HIVEINSIDE_SYNC_LEAD_S (60 s) — to absorb one interval of
// RC-oscillator drift between the two free-running deep-sleep timers (the
// internal ~150 kHz RC clock drifts several percent with temperature, so a
// 10-min interval can slip tens of seconds per cycle).
//
// IMPORTANT — this window is only fully consumed on a MISSED rendezvous: in the
// normal case the device sleeps the instant HiveScale connects, writes the next
// hint and disconnects (see main.cpp: windowMs = 0). So a longer window costs
// extra awake time only on the cycles it actually rescues. Tune from the
// "[SYNC] ... awake Nms" logs once you have field drift numbers.
#define SYNC_LISTEN_MS 150000UL
#endif

// Guard rails on a received sync value, in case of a bad/garbage write.
#ifndef SYNC_MIN_SLEEP_MS
#define SYNC_MIN_SLEEP_MS (30UL * 1000UL)
#endif
#ifndef SYNC_MAX_SLEEP_MS
#define SYNC_MAX_SLEEP_MS (24UL * 60UL * 60UL * 1000UL)
#endif

// ---------------------------------------------------------------------------
// Pin map — Seeed XIAO ESP32-C6 (override any of these in platformio.ini).
//
// Defaults use the XIAO's labelled header pads. ADC1 lives on GPIO0..GPIO6, so
// battery sense uses A0 (GPIO0); I2C and I2S route to any GPIO via the GPIO
// matrix. GPIO3/GPIO14 drive the XIAO's internal RF switch and are NOT on the
// header, so they are avoided. The on-board BOOT button (GPIO9) is reused for
// "identify"; the user LED is GPIO15.
// ---------------------------------------------------------------------------
#ifndef PIN_I2C_SDA
#define PIN_I2C_SDA 22   // D4 (XIAO default SDA)
#endif
#ifndef PIN_I2C_SCL
#define PIN_I2C_SCL 23   // D5 (XIAO default SCL)
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
#define PIN_I2S_BCLK 16  // D6
#endif
#ifndef PIN_I2S_WS
#define PIN_I2S_WS 17    // D7
#endif
#ifndef PIN_I2S_SD
#define PIN_I2S_SD 19    // D8
#endif

// On-board BOOT button (active-low). Short press = publish now / connectable.
#ifndef PIN_BUTTON
#define PIN_BUTTON 9
#endif

// On-board user LED (GPIO15 on the XIAO ESP32-C6, active-low). Used as a heartbeat.
#ifndef PIN_LED
#define PIN_LED 15
#endif
#ifndef LED_ACTIVE_LOW
#define LED_ACTIVE_LOW 1
#endif

// ---------------------------------------------------------------------------
// Battery monitoring (optional). The XIAO ESP32-C6 charges a LiPo over USB-C
// (on-board charger) but has NO battery-sense divider, so reading the cell
// needs an external resistor divider from BAT+ to an ADC1 pad. See docs.
// ---------------------------------------------------------------------------
#ifndef ENABLE_BATTERY
#define ENABLE_BATTERY 1
#endif

// ADC1-capable pad the external divider feeds. A0 (GPIO0) is the conventional
// XIAO battery-sense pad (ADC1 spans GPIO0..GPIO6).
#ifndef PIN_VBAT_ADC
#define PIN_VBAT_ADC 0
#endif

// Divider ratio Vbat/Vadc. 2.0 = two equal resistors (e.g. 220k/220k).
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

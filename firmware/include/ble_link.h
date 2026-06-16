// ble_link.h — BLE transport with a compile-time advertising / GATT switch.
//
// The mode is chosen by BLE_MODE in config.h (override with -DBLE_MODE in
// platformio.ini):
//
//   BLE_MODE_ADVERTISING — connectionless BTHome v2 broadcast (Home Assistant
//       native) for temp/humidity/battery, plus a compact manufacturer-data
//       blob in the scan response carrying the full vibration + acoustic
//       summary (all FFT bands, RMS, peak). Radio on only ADV_BURST_MS/cycle.
//
//   BLE_MODE_GATT — connectable GATT server. Standard Battery (0x180F) and
//       Environmental Sensing (0x181A) services for generic clients, plus a
//       custom HiveInside service whose JSON characteristic carries the entire
//       Measurement (every band, RMS and peak). Read or subscribe-to-notify.
#pragma once

#include "measurement.h"

namespace ble {
// Initialise the radio for the configured BLE_MODE. Call once in setup().
void begin();
// Publish a fresh measurement: re-advertise (advertising mode) or update the
// characteristics and notify subscribers (GATT mode).
void publish(const Measurement& m);
// Human-readable name of the active mode, for logging.
const char* modeName();
} // namespace ble

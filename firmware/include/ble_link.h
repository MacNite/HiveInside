// ble_link.h — BLE transport: a connectable GATT server.
//
// The device exposes the standard Battery (0x180F) and Environmental Sensing
// (0x181A) services for generic clients, plus a custom HiveInside service whose
// JSON characteristic carries the entire Measurement (every band, RMS and
// peak). Read or subscribe-to-notify.
#pragma once

#include "measurement.h"

namespace ble {
// Initialise the radio and start the GATT server. Call once in setup().
void begin();
// Publish a fresh measurement: re-advertise (advertising mode) or update the
// characteristics and notify subscribers (GATT mode).
void publish(const Measurement& m);
// Deinit the BLE stack cleanly (call before deep sleep).
void shutdown();
// Enter pairing mode: keeps the device visible for PAIRING_WINDOW_MS so
// HiveScale's provisioning portal can discover the MAC. The device is already
// advertising as connectable; pairing mode mainly suppresses sleep.
void enterPairingMode();
// Returns true while the pairing window is open.
bool isPairingActive();
// Human-readable name of the active transport, for logging.
const char* modeName();

#if HIVEINSIDE_SYNC_ENABLED
// Wake synchronisation: HiveScale writes the seconds to sleep before the next
// connection into a writable characteristic. These let main.cpp decide
// the deep-sleep duration and when the listen window can end early.

// If HiveScale wrote a wake-sync value during this awake period, store the
// requested sleep (clamped to SYNC_MIN/MAX_SLEEP_MS) in *outMs and return true.
bool syncWakeMs(uint64_t* outMs);
// True while a central is connected (so the listen window waits for the write).
bool isCentralConnected();
#endif
} // namespace ble

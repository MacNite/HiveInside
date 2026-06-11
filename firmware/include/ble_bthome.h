// ble_bthome.h — builds and broadcasts a BTHome v2 BLE advertisement.
//
// BTHome (https://bthome.io) is an open advertising format natively supported by
// Home Assistant. We broadcast unencrypted by default (no pairing needed); a
// future encrypted mode is gated behind the pairing button.
#pragma once

#include "measurement.h"

// Initialise the BLE stack and base advertising parameters.
void bleInit();

// Encode `m` into a BTHome v2 service-data payload and advertise it for
// ADV_DURATION_MS, then stop advertising (caller goes back to sleep).
void bleAdvertise(const Measurement& m);

// Put the radio/stack into the lowest-power state before deep sleep.
void bleStop();

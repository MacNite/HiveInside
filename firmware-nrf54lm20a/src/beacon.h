/* beacon.h — BLE measurement beacon (data transfer to HiveHub).
 *
 * Broadcasts the full measurement as a 26-byte manufacturer-data advertisement
 * that HiveHub picks up with a passive scan and bridges to the backend. The
 * frame layout is defined in hive_config.h and decoded verbatim by HiveHub
 * firmware/src/ble_sensor.cpp::parseHiveInside().
 *
 * The advertiser is non-connectable and runs continuously; only the payload is
 * refreshed each measurement cycle. See beacon.c and the README for the
 * radio-sleep rationale.
 */
#pragma once

#include "measurement.h"

/* Bring up the BLE stack and start advertising an (initially empty) frame.
 * Prints the device's BLE address — the MAC to pair in the HiveHub portal.
 * Returns 0 on success, or a negative errno when the stack failed to start
 * (the caller keeps running the serial readout in that case). */
int beacon_init(void);

/* Encode `m` into the advertisement so every subsequent advertising event
 * carries the fresh reading. A group whose *_ok flag is false is marked absent
 * (its flag bit cleared) so HiveHub reports "not present" rather than raw zeros.
 * A no-op when beacon_init() did not succeed. */
void beacon_update(const struct measurement *m);

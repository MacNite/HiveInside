/* beacon.h — BLE transport: a continuously advertising measurement beacon.
 *
 * The full measurement rides in a manufacturer-specific AD structure that
 * HiveHub decodes with a passive scan (see beacon.c for the byte layout).
 * Advertising is connectable so HiveHub can additionally make a one-off
 * GATT connection for firmware-version/board discovery and, later, OTA
 * (see gatt_hive.c) — but no connection is ever needed for the data path.
 */
#pragma once

#include "measurement.h"

/* Start advertising with the given measurement encoded in the frame.
 * Call once after bt_enable(). Returns 0 on success. */
int beacon_start(const struct measurement *m);

/* Re-encode the frame from a fresh measurement. Takes effect immediately
 * while advertising; while a central is connected the advertiser is
 * stopped, and the new data is picked up by the automatic restart on
 * disconnect. */
void beacon_update(const struct measurement *m);

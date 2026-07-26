/*
 * gatt_hive.h — connectable HiveInside GATT service + Firmware-over-BLE (OTA).
 *
 * This module adds the connectable side of the radio: a GATT server exposing
 * the custom HiveInside OTA characteristics that a HiveHub relay drives to
 * flash a new image, backed by MCUboot dual-slot DFU. It is deliberately
 * separate from beacon.c, which owns the non-connectable measurement
 * advertisement; the two run as independent advertising sets so the passive
 * measurement beacon is never interrupted by an OTA session.
 *
 * When OTA is compiled out (HIVEINSIDE_OTA_ENABLED=0) every entry point below
 * degrades to a no-op and no GATT server or connectable advertising is started.
 */
#pragma once

#include <stdbool.h>

/*
 * Register the custom HiveInside GATT service and start a connectable
 * advertising set so a relay can open a GATT session for OTA. Must be called
 * after Bluetooth is enabled (beacon_init() calls bt_enable()). Returns 0 on
 * success or a negative errno; a failure here leaves the measurement beacon
 * fully working.
 */
int gatt_hive_init(void);

/*
 * True while a firmware transfer is in progress (BEGIN accepted, not yet
 * finished or aborted) or a verified image is staged and a reboot is pending.
 * main.c uses this as the isOtaActive() gate: while it is true the main loop
 * suppresses deep sleep and skips measurement so the transfer is not disturbed.
 */
bool gatt_hive_ota_active(void);

/*
 * Drive deferred OTA work — currently the delayed reboot after END verifies an
 * image, so the central has a chance to read the terminal "done" status first.
 * Call once per main-loop iteration; a no-op when nothing is pending.
 */
void gatt_hive_poll(void);

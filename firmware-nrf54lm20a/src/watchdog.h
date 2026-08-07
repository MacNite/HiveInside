/* watchdog.h — hardware watchdog for unattended in-hive operation.
 *
 * A sealed node has no console and nobody watching it. Without a watchdog a
 * hang inside a sensor driver — a stuck I²C transfer, a dmic_read() that never
 * returns — leaves the firmware alive enough to keep advertising the last good
 * measurement forever, so the failure looks like a hive that simply stopped
 * changing. The watchdog turns that into a reset, and MCUboot brings the node
 * back on the same image.
 */
#pragma once

#include <stdbool.h>

/* Install and start the watchdog. Call once at boot, before the main loop.
 * After this returns successfully the firmware must call hive_watchdog_feed()
 * at least every HIVE_WDT_TIMEOUT_MS or the SoC resets.
 *
 * Compiles to a no-op when the devicetree has no enabled watchdog node, so the
 * build still works on a board tree without one; hive_watchdog_active() then
 * reports false and nothing else changes. */
void hive_watchdog_init(void);

/* Restart the timeout. Safe to call before hive_watchdog_init() or when no
 * watchdog was installed — it is a no-op in both cases, so callers never need
 * to guard it. */
void hive_watchdog_feed(void);

/* True once a watchdog is installed and running. Only useful for reporting;
 * hive_watchdog_feed() is safe regardless. */
bool hive_watchdog_active(void);

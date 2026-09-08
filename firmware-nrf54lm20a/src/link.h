/* Shared ownership of the single peripheral BLE connection. */
#pragma once

#include <stdbool.h>
#include <zephyr/bluetooth/conn.h>

enum link_owner {
	LINK_OWNER_NONE,
	LINK_OWNER_OTA,
	LINK_OWNER_AUDIO,
	LINK_OWNER_THROUGHPUT,
};

struct bt_conn *link_conn(void);
bool link_claim(enum link_owner owner);
void link_release(enum link_owner owner);
bool link_is_owner(enum link_owner owner);
bool link_is_busy(void);
/* Hold around the complete powered sensor cycle.  This waits in watchdog-sized
 * slices, while link_claim() remains non-blocking for GATT callers. */
bool link_measurement_begin(void);
void link_measurement_end(void);

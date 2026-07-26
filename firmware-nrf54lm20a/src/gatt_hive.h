/* HiveInside Firmware-over-BLE GATT service. */
#pragma once

#include <stdbool.h>

/* Start the connectable OTA advertising set after Bluetooth is enabled. */
int gatt_hive_init(void);

/* True while a transfer is receiving data or waiting for its verified reboot. */
bool gatt_hive_ota_active(void);

/* HiveHub-compatible, connectionless BLE measurement transport. */
#pragma once

#include "measurement.h"

/* Enable Bluetooth. Advertising starts with the first published reading. */
int beacon_init(void);

/* Encode and publish one measurement. The controller repeats it autonomously. */
int beacon_publish(const struct measurement *measurement);


/* battery.h — LiPo state from the XIAO's on-board nPM1300 PMIC. */
#pragma once

#include "measurement.h"

/* Read the cell voltage from the nPM1300 charger/fuel-gauge and derive a
 * rough percentage (sets m->battery_ok). A no-op that leaves battery_ok
 * false when the devicetree has no enabled nPM1300 charger node. */
void battery_read(struct measurement *m);

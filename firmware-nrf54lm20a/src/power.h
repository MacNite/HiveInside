/* power.h — board power-rail bring-up (nPM1300 LDO1 → sensor rail). */
#pragma once

/* Raise the nPM1300's LDO1 to 3.3 V and enable it. On the XIAO nRF54LM20A
 * Sense LDO1 powers the on-board IMU and PDM microphone, but the stock board
 * definition leaves it at 1.8 V — too low for either part. Call once at boot,
 * before any sensor is probed. Harmless no-op when the devicetree has no
 * nPM1300 regulator (the sensors then simply fail their probes). */
void power_init(void);

/* Enable/disable the switched IMU and microphone supply around a measurement.
 * Keeping LDO1 off during the long interval between captures avoids paying the
 * sensors' standby current. The upstream board power gate remains enabled so
 * the PMIC stays reachable. Both calls are harmless no-ops on boards without
 * a controllable nPM1300 LDO1. */
int power_sensor_rail_enable(void);
int power_sensor_rail_disable(void);

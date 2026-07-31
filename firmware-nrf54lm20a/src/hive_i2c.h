/*
 * hive_i2c.h — the set of I²C buses the sensor probes may live on.
 *
 * The XIAO nRF54LM20A Sense has two relevant TWIM buses: the XIAO header bus
 * (external SHT40 on D4/D5) and the internal bus wired to the on-board
 * LSM6DS3TR-C IMU and nPM1300 PMIC. Rather than hard-coding board node
 * labels, this helper collects every enabled `nordic,nrf-twim` controller
 * from the devicetree, and the sensor modules probe their fixed addresses on
 * each — so the firmware keeps working whichever bus a sensor sits on, and an
 * external LIS3DH breakout wired to the header is found too.
 */
#pragma once

#include <stddef.h>
#include <zephyr/device.h>

/* Fill out[] with up to max ready I²C bus devices; returns the count. */
size_t hive_i2c_buses(const struct device **out, size_t max);

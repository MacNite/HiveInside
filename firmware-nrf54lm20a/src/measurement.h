/*
 * measurement.h — one full sensor snapshot, shared by every module.
 *
 * The sensor modules fill this struct each cycle; main.c prints it to the USB
 * serial console. A group's *_ok flag is false when the sensor is missing or
 * its read failed this cycle, and main.c then prints that group as "n/a"
 * rather than showing stale or zero values.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

struct measurement {
	/* ── Climate (SHT40) ── */
	bool sht_ok;
	float temp_c;
	float humidity_pct;

	/* ── Vibration (on-board LSM6DS3TR-C IMU, or external LIS3DH) ── */
	bool accel_ok;
	float accel_x_mg; /* per-axis acceleration, milli-g */
	float accel_y_mg;
	float accel_z_mg;
	float accel_mag_mg; /* magnitude |a| */

	/* ── Acoustics (PDM microphone) ── */
	bool mic_ok;
	uint32_t mic_frames; /* PCM frames analysed */
	float mic_rms_dbfs;  /* AC RMS level, dBFS re 16-bit full scale */
	float mic_peak_dbfs; /* peak deviation, dBFS */

	/* ── Power (nPM1300 fuel gauge) ── */
	bool battery_ok;
	float battery_v;
	uint8_t battery_pct;
};

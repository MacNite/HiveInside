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
	float accel_x_mg; /* mean (static/orientation) acceleration, milli-g */
	float accel_y_mg;
	float accel_z_mg;
	float accel_mag_mg; /* magnitude of the mean vector */
	uint16_t accel_sample_rate_hz;
	uint16_t accel_sample_count;
	float accel_rms_mg;  /* broadband AC RMS of |a| */
	float accel_peak_mg; /* peak |a − mean| */
	float accel_band_swarm_mg;    /*   8–30 Hz */
	float accel_band_fanning_mg;  /*  30–100 Hz */
	float accel_band_activity_mg; /* 100–200 Hz */

	/* ── Acoustics (PDM microphone), dBFS re 16-bit full scale ── */
	bool mic_ok;
	uint32_t mic_frames; /* PCM frames analysed */
	float mic_rms_dbfs;  /* AC RMS level */
	float mic_peak_dbfs; /* peak deviation */
	float mic_band_sub_bass_dbfs; /*   50–150 Hz */
	float mic_band_hum_dbfs;      /*  150–300 Hz */
	float mic_band_piping_dbfs;   /*  300–550 Hz */
	float mic_band_stress_dbfs;   /*  550–1500 Hz */
	float mic_band_high_dbfs;     /* 1500–3000 Hz */

	/* ── Power (nPM1300 fuel gauge) ── */
	bool battery_ok;
	float battery_v;
	uint8_t battery_pct;
};

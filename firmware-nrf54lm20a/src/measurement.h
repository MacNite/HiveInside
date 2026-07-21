/*
 * measurement.h — one full sensor snapshot, shared by every module.
 *
 * The sensor modules fill this struct; beacon.c serialises it into the
 * 26-byte manufacturer-data frame. Keeping one plain struct means the
 * capture/FFT code is completely independent of the BLE transport, exactly
 * as in the ESP32-C6 prototype.
 *
 * A group's *_ok flag mirrors the beacon flags byte: when false the group's
 * values are absent (the sensor is missing or the read failed this cycle)
 * and HiveHub reports them as "not present" rather than zeros.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

struct measurement {
	/* ── Climate (SHT40) ── */
	bool sht_ok;
	float temp_c;
	float humidity_pct;

	/* ── Vibration (LSM6DS3TR-C on-board IMU, or external LIS3DH) ── */
	bool accel_ok;
	uint16_t accel_sample_rate_hz;
	uint16_t accel_sample_count;
	float accel_rms_mg;  /* broadband AC RMS of |a| */
	float accel_peak_mg; /* peak |a − mean| */
	float accel_band_swarm_mg;    /*   8–30 Hz */
	float accel_band_fanning_mg;  /*  30–100 Hz */
	float accel_band_activity_mg; /* 100–200 Hz */

	/* ── Acoustics (PDM microphone), dBFS re 16-bit full scale ── */
	bool mic_ok;
	float mic_rms_dbfs;
	float mic_peak_dbfs;
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

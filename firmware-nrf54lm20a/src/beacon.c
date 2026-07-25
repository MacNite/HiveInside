/*
 * HiveInside manufacturer-data beacon.
 *
 * This is the exact 26-byte format consumed by HiveHub's
 * blesensor::parseHiveInside().  Keeping the complete measurement in the
 * primary advertising packet lets HiveHub use a passive scan; no connection,
 * scan response, pairing window, or wake-time rendezvous is required.
 */
#include "beacon.h"
#include "hive_config.h"

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#define FRAME_SIZE 26U
#define FRAME_MAGIC 0x48U /* 'H' */
#define FRAME_VERSION 0x01U

#define FLAG_SHT   (1U << 0)
#define FLAG_ACCEL (1U << 1)
#define FLAG_MIC   (1U << 2)
#define FLAG_BATT  (1U << 3)

static uint8_t frame[FRAME_SIZE];
static bool bluetooth_ready;
static bool advertising;

static void put_u16(size_t offset, uint16_t value)
{
	frame[offset] = (uint8_t)value;
	frame[offset + 1U] = (uint8_t)(value >> 8);
}

static int16_t scaled_i16(float value, float scale)
{
	if (!isfinite(value)) {
		return 0;
	}
	long encoded = lroundf(value * scale);
	return (int16_t)CLAMP(encoded, INT16_MIN, INT16_MAX);
}

static uint16_t scaled_u16(float value, float scale)
{
	if (!isfinite(value) || value <= 0.0f) {
		return 0;
	}
	long encoded = lroundf(value * scale);
	return (uint16_t)MIN(encoded, UINT16_MAX);
}

static int8_t rounded_i8(float value)
{
	if (!isfinite(value)) {
		return 0;
	}
	long encoded = lroundf(value);
	return (int8_t)CLAMP(encoded, INT8_MIN, INT8_MAX);
}

static void encode(const struct measurement *m)
{
	memset(frame, 0, sizeof(frame));
	put_u16(0, HIVEINSIDE_COMPANY_ID);
	frame[2] = FRAME_MAGIC;
	frame[3] = FRAME_VERSION;

	if (m->sht_ok) {
		frame[4] |= FLAG_SHT;
		put_u16(5, (uint16_t)scaled_i16(m->temp_c, 10.0f));
		put_u16(7, scaled_u16(m->humidity_pct, 10.0f));
	}
	if (m->battery_ok) {
		frame[4] |= FLAG_BATT;
		put_u16(9, scaled_u16(m->battery_v, 1000.0f));
		frame[11] = MIN(m->battery_pct, 100U);
	}
	if (m->accel_ok) {
		frame[4] |= FLAG_ACCEL;
		put_u16(12, scaled_u16(m->accel_rms_mg, 10.0f));
		put_u16(14, scaled_u16(m->accel_band_swarm_mg, 10.0f));
		put_u16(16, scaled_u16(m->accel_band_fanning_mg, 10.0f));
		put_u16(18, scaled_u16(m->accel_band_activity_mg, 10.0f));
	}
	if (m->mic_ok) {
		frame[4] |= FLAG_MIC;
		frame[20] = (uint8_t)rounded_i8(m->mic_rms_dbfs);
		frame[21] = (uint8_t)rounded_i8(m->mic_band_sub_bass_dbfs);
		frame[22] = (uint8_t)rounded_i8(m->mic_band_hum_dbfs);
		frame[23] = (uint8_t)rounded_i8(m->mic_band_piping_dbfs);
		frame[24] = (uint8_t)rounded_i8(m->mic_band_stress_dbfs);
		frame[25] = (uint8_t)rounded_i8(m->mic_band_high_dbfs);
	}
}

int beacon_init(void)
{
	int err = bt_enable(NULL);

	if (err != 0) {
		printk("[BLE] init failed (%d)\n", err);
		return err;
	}
	printk("[BLE] ready; beacon interval %u ms\n", BLE_ADV_INTERVAL_MS);
	bluetooth_ready = true;
	return 0;
}

int beacon_publish(const struct measurement *m)
{
	if (m == NULL) {
		return -EINVAL;
	}
	if (!bluetooth_ready) {
		return -ENODEV;
	}

	encode(m);
	const struct bt_data ad[] = {
		BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_NO_BREDR),
		BT_DATA(BT_DATA_MANUFACTURER_DATA, frame, sizeof(frame)),
	};
	int err;

	if (!advertising) {
		const struct bt_le_adv_param param = BT_LE_ADV_PARAM_INIT(
			BT_LE_ADV_OPT_USE_IDENTITY,
			BLE_ADV_INTERVAL_UNITS, BLE_ADV_INTERVAL_UNITS, NULL);
		err = bt_le_adv_start(&param, ad, ARRAY_SIZE(ad), NULL, 0);
		if (err == 0) {
			advertising = true;
		}
	} else {
		err = bt_le_adv_update_data(ad, ARRAY_SIZE(ad), NULL, 0);
	}

	if (err != 0) {
		printk("[BLE] measurement publish failed (%d)\n", err);
	} else {
		printk("[BLE] measurement advertised (flags=0x%02x)\n", frame[4]);
	}
	return err;
}

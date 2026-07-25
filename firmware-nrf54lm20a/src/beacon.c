/*
 * HiveInside manufacturer-data beacon.
 *
 * Version 2 extends the 26-byte format consumed by existing HiveHubs with
 * acceleration and microphone peaks at bytes 26..28. Keeping the version-1
 * prefix unchanged preserves core-data compatibility, while the complete
 * measurement remains in the primary advertising packet for passive scans.
 * Active setup scans can additionally read the local name from the response.
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

#define FRAME_SIZE 29U
#define FRAME_MAGIC 0x48U /* 'H' */
#define FRAME_VERSION 0x02U

#define FLAG_SHT   (1U << 0)
#define FLAG_ACCEL (1U << 1)
#define FLAG_MIC   (1U << 2)
#define FLAG_BATT  (1U << 3)

static uint8_t frame[FRAME_SIZE];
static bool bluetooth_ready;
static bool advertising;
static bool scannable = true; /* cleared if the controller rejects the scan response */

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
	/* Version 2 appends the two broadband peak values. Keeping every version-1
	 * field at its original offset lets existing HiveHubs continue decoding the
	 * core measurement while newer decoders consume these trailing bytes. */
	if (m->accel_ok) {
		put_u16(26, scaled_u16(m->accel_peak_mg, 10.0f));
	}
	if (m->mic_ok) {
		frame[28] = (uint8_t)rounded_i8(m->mic_peak_dbfs);
	}
}

int beacon_init(void)
{
	int err = bt_enable(NULL);

	if (err != 0) {
		printk("[BLE] init failed (%d)\n", err);
		return err;
	}
	printk("[BLE] ready; name=%s manufacturer=%s id=0x%04x interval=%u ms\n",
	       HIVEINSIDE_DEVICE_NAME, HIVEINSIDE_MANUFACTURER_NAME,
	       HIVEINSIDE_COMPANY_ID, BLE_ADV_INTERVAL_MS);
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
	/* Flags are optional for a non-connectable LE-only broadcaster. Omitting
	 * that three-byte AD element leaves the complete 31-byte legacy payload for
	 * the manufacturer element: 29 data bytes plus its length and type. */
	const struct bt_data ad[] = {
		BT_DATA(BT_DATA_MANUFACTURER_DATA, frame, sizeof(frame)),
	};
	const struct bt_data scan_response[] = {
		BT_DATA(BT_DATA_NAME_COMPLETE, HIVEINSIDE_DEVICE_NAME,
			sizeof(HIVEINSIDE_DEVICE_NAME) - 1U),
	};
	int err;

	if (!advertising) {
		/* Legacy scannable undirected advertising (ADV_SCAN_IND): the
		 * measurement stays in the primary packet for HiveHub's passive
		 * scan while the "HiveInside" name rides in the scan response for
		 * active setup scans. Only the legacy PDU allows both payloads at
		 * once, so never let extended advertising be selected here. */
		struct bt_le_adv_param param = BT_LE_ADV_PARAM_INIT(
			BT_LE_ADV_OPT_USE_IDENTITY | BT_LE_ADV_OPT_SCANNABLE,
			BLE_ADV_INTERVAL_UNITS, BLE_ADV_INTERVAL_UNITS, NULL);
		err = bt_le_adv_start(&param, ad, ARRAY_SIZE(ad), scan_response,
				      ARRAY_SIZE(scan_response));
		if (err != 0) {
			/* The controller rejected the scannable beacon. Fall back
			 * to the known-good non-connectable advert so HiveHub keeps
			 * receiving measurements; the friendly name is best-effort. */
			printk("[BLE] scannable start failed (%d); "
			       "falling back to non-connectable\n", err);
			scannable = false;
			param.options = BT_LE_ADV_OPT_USE_IDENTITY;
			err = bt_le_adv_start(&param, ad, ARRAY_SIZE(ad), NULL, 0);
		}
		if (err == 0) {
			advertising = true;
		}
	} else if (scannable) {
		err = bt_le_adv_update_data(ad, ARRAY_SIZE(ad), scan_response,
					ARRAY_SIZE(scan_response));
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

/* beacon.c — measurement beacon: encoder + advertiser management.
 *
 * ── Manufacturer-data frame layout (the wire contract) ────────────────────
 *
 * One 26-byte manufacturer-specific AD payload carries the whole
 * measurement. The layout is mirrored byte-for-byte by HiveHub's
 * parseHiveInside() (HiveHub firmware/src/ble_sensor.cpp, the HI_*
 * constants) — change either side only together. The ESP32-C6 prototype
 * emitted this same frame, so existing HiveHubs decode the nRF54 node
 * unchanged.
 *
 *   off 0..1  : company id (LE)         == BEACON_COMPANY_ID (0x02E5)
 *   off 2     : magic                    == 0x48 ('H')
 *   off 3     : version                  (currently 0x01)
 *   off 4     : flags  bit0 sht  bit1 accel  bit2 mic  bit3 batt
 *   off 5..6  : temperature   int16  LE, 0.1 °C   (valid only if bit0 set)
 *   off 7..8  : humidity      uint16 LE, 0.1 %RH  (valid only if bit0 set)
 *   off 9..10 : battery       uint16 LE, millivolt (valid only if bit3 set)
 *   off 11    : battery percent (uint8)            (valid only if bit3 set)
 *   off 12..13: accel RMS           uint16 LE, 0.1 mg
 *   off 14..15: accel band swarm    uint16 LE, 0.1 mg   (8–30 Hz)
 *   off 16..17: accel band fanning  uint16 LE, 0.1 mg   (30–100 Hz)
 *   off 18..19: accel band activity uint16 LE, 0.1 mg   (100–200 Hz)
 *   off 20    : mic RMS        int8, dBFS
 *   off 21    : mic sub-bass   int8, dBFS   (50–150 Hz)
 *   off 22    : mic hum        int8, dBFS   (150–300 Hz)
 *   off 23    : mic piping     int8, dBFS   (300–550 Hz)
 *   off 24    : mic stress     int8, dBFS   (550–1500 Hz)
 *   off 25    : mic high       int8, dBFS   (1500–3000 Hz)
 *
 * A group whose flags bit is clear leaves its fields zeroed; decoders must
 * treat them as absent, not as physical zeros.
 *
 * Payload budget: flags AD (3 B) + manufacturer AD (2 B header + 26 B)
 * = 31 B — exactly the legacy advertising payload. The device name rides in
 * the scan response (HiveHub scans actively, so it still sees the name).
 *
 * ── Addressing ────────────────────────────────────────────────────────────
 * HiveHub pairs beacons by MAC, so the advertising address must be stable:
 * privacy is off (no CONFIG_BT_PRIVACY) and the Zephyr host uses the nRF's
 * factory static-random address, which is constant across reboots and
 * firmware updates.
 */
#include "beacon.h"

#include "hive_config.h"

#include <math.h>
#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>

#define FRAME_LEN 26
#define FRAME_MAGIC 0x48 /* 'H' */
#define FRAME_VERSION 0x01

#define FLAG_SHT   BIT(0)
#define FLAG_ACCEL BIT(1)
#define FLAG_MIC   BIT(2)
#define FLAG_BATT  BIT(3)

/* The connectable-advertising option macro was renamed from
 * BT_LE_ADV_OPT_CONNECTABLE to BT_LE_ADV_OPT_CONN in Zephyr 3.7 / NCS 2.7.
 * Support either so the same source builds against the pinned Seeed
 * PlatformIO framework and a current upstream/NCS `west` workspace alike. */
#if !defined(BT_LE_ADV_OPT_CONN) && defined(BT_LE_ADV_OPT_CONNECTABLE)
#define BT_LE_ADV_OPT_CONN BT_LE_ADV_OPT_CONNECTABLE
#endif

static uint8_t frame[FRAME_LEN];

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA(BT_DATA_MANUFACTURER_DATA, frame, sizeof(frame)),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/* Connectable advertising at the power-efficient beacon rate configured in
 * hive_config.h (see the BEACON_ADV_INT_* rationale there). */
static const struct bt_le_adv_param adv_param = BT_LE_ADV_PARAM_INIT(
	BT_LE_ADV_OPT_CONN, BEACON_ADV_INT_MIN, BEACON_ADV_INT_MAX, NULL);

/* ── Field encoders: clamp + scale, NAN-safe (absent groups stay zero) ── */

static uint16_t enc_u16_x10(float v)
{
	if (isnan(v) || v <= 0.0f) {
		return 0;
	}
	float x = v * 10.0f + 0.5f;

	return x >= 65535.0f ? 65535 : (uint16_t)x;
}

static int16_t enc_i16_x10(float v)
{
	if (isnan(v)) {
		return 0;
	}
	float x = v * 10.0f;

	if (x >= 32767.0f) {
		return 32767;
	}
	if (x <= -32768.0f) {
		return -32768;
	}
	return (int16_t)lroundf(x);
}

static int8_t enc_i8(float v)
{
	if (isnan(v)) {
		return 0;
	}
	if (v >= 127.0f) {
		return 127;
	}
	if (v <= -128.0f) {
		return -128;
	}
	return (int8_t)lroundf(v);
}

static void encode(const struct measurement *m)
{
	uint8_t flags = 0;

	memset(frame, 0, sizeof(frame));
	sys_put_le16(BEACON_COMPANY_ID, &frame[0]);
	frame[2] = FRAME_MAGIC;
	frame[3] = FRAME_VERSION;

	if (m->sht_ok) {
		flags |= FLAG_SHT;
		sys_put_le16((uint16_t)enc_i16_x10(m->temp_c), &frame[5]);
		sys_put_le16(enc_u16_x10(m->humidity_pct), &frame[7]);
	}
	if (m->battery_ok) {
		flags |= FLAG_BATT;
		float mv = isnan(m->battery_v) ? 0.0f
					       : m->battery_v * 1000.0f + 0.5f;
		if (mv < 0.0f) {
			mv = 0.0f;
		}
		if (mv > 65535.0f) {
			mv = 65535.0f;
		}
		sys_put_le16((uint16_t)mv, &frame[9]);
		frame[11] = m->battery_pct;
	}
	if (m->accel_ok) {
		flags |= FLAG_ACCEL;
		sys_put_le16(enc_u16_x10(m->accel_rms_mg), &frame[12]);
		sys_put_le16(enc_u16_x10(m->accel_band_swarm_mg), &frame[14]);
		sys_put_le16(enc_u16_x10(m->accel_band_fanning_mg), &frame[16]);
		sys_put_le16(enc_u16_x10(m->accel_band_activity_mg), &frame[18]);
	}
	if (m->mic_ok) {
		flags |= FLAG_MIC;
		frame[20] = (uint8_t)enc_i8(m->mic_rms_dbfs);
		frame[21] = (uint8_t)enc_i8(m->mic_band_sub_bass_dbfs);
		frame[22] = (uint8_t)enc_i8(m->mic_band_hum_dbfs);
		frame[23] = (uint8_t)enc_i8(m->mic_band_piping_dbfs);
		frame[24] = (uint8_t)enc_i8(m->mic_band_stress_dbfs);
		frame[25] = (uint8_t)enc_i8(m->mic_band_high_dbfs);
	}
	frame[4] = flags;
}

/* ── Advertiser lifecycle ─────────────────────────────────────────────────
 * Legacy connectable advertising stops when a central connects (HiveHub's
 * version-discovery or a future OTA session) and is restarted from the
 * disconnect callback via the work item below, with a retry in case the
 * connection object is still being recycled. */

static void adv_restart(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(adv_restart_work, adv_restart);

static void adv_restart(struct k_work *work)
{
	int err = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), sd,
				  ARRAY_SIZE(sd));

	if (err == -EALREADY) {
		return;
	}
	if (err != 0) {
		printk("[BLE] adv restart failed (%d); retrying\n", err);
		k_work_schedule(&adv_restart_work, K_MSEC(500));
		return;
	}
	printk("[BLE] advertising resumed\n");
}

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	if (err == 0U) {
		printk("[BLE] central connected\n");
	}
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	printk("[BLE] central disconnected (reason %u); re-advertising\n",
	       reason);
	k_work_schedule(&adv_restart_work, K_MSEC(50));
}

BT_CONN_CB_DEFINE(beacon_conn_cb) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
};

int beacon_start(const struct measurement *m)
{
	encode(m);

	int err = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), sd,
				  ARRAY_SIZE(sd));
	if (err != 0) {
		printk("[BLE] adv start failed (%d)\n", err);
		return err;
	}

	bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
	size_t count = ARRAY_SIZE(addrs);
	char addr_str[BT_ADDR_LE_STR_LEN] = "?";

	bt_id_get(addrs, &count);
	if (count > 0) {
		bt_addr_le_to_str(&addrs[0], addr_str, sizeof(addr_str));
	}
	printk("[BLE] beacon advertising as %s every %u-%u ms (%u-byte frame)\n",
	       addr_str, (unsigned)(BEACON_ADV_INT_MIN * 625U / 1000U),
	       (unsigned)(BEACON_ADV_INT_MAX * 625U / 1000U),
	       (unsigned)sizeof(frame));
	return 0;
}

void beacon_update(const struct measurement *m)
{
	encode(m);

	/* The stack copies the AD payload on this call. -EAGAIN just means a
	 * central currently holds the advertiser; the disconnect restart
	 * re-reads frame[], which already holds the new bytes. */
	int err = bt_le_adv_update_data(ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

	if (err != 0 && err != -EAGAIN) {
		printk("[BLE] adv data update failed (%d)\n", err);
	}
}

/* beacon.c — BLE measurement beacon: the data-transfer path to HiveHub.
 *
 * ── Wire format (26 bytes, little-endian) ──────────────────────────────────
 * The advertisement's manufacturer-specific data carries one full measurement.
 * HiveHub firmware/src/ble_sensor.cpp::parseHiveInside() decodes these exact
 * bytes; the offsets/scales below MUST match it (and hive_config.h):
 *
 *   0..1  company id (LE)     == BEACON_COMPANY_ID
 *   2     magic               == 0x48 ('H')
 *   3     version             == 0x01
 *   4     flags   bit0 sht  bit1 accel  bit2 mic  bit3 batt
 *   5..6  temperature   int16 LE, 0.1 °C     (valid iff flag sht)
 *   7..8  humidity      uint16 LE, 0.1 %RH   (valid iff flag sht)
 *   9..10 battery       uint16 LE, milli-volt (valid iff flag batt)
 *   11    battery percent uint8              (valid iff flag batt)
 *   12..13 accel RMS         uint16 LE, 0.1 mg   (valid iff flag accel)
 *   14..15 accel band swarm    uint16 LE, 0.1 mg   (8–30 Hz)
 *   16..17 accel band fanning  uint16 LE, 0.1 mg   (30–100 Hz)
 *   18..19 accel band activity uint16 LE, 0.1 mg   (100–200 Hz)
 *   20    mic RMS       int8 dBFS            (valid iff flag mic)
 *   21    mic sub-bass  int8 dBFS   (50–150 Hz)
 *   22    mic hum       int8 dBFS   (150–300 Hz)
 *   23    mic piping    int8 dBFS   (300–550 Hz)
 *   24    mic stress    int8 dBFS   (550–1500 Hz)
 *   25    mic high      int8 dBFS   (1500–3000 Hz)
 *
 * A cleared flag bit means that sensor was absent or failed this cycle; the
 * group's bytes stay zero and HiveHub reports the group as "not present" rather
 * than reading the zeros as real values (a dead SHT40 must not look like 0.0 °C).
 *
 * ── Radio sleep ────────────────────────────────────────────────────────────
 * The advertiser is non-connectable and runs continuously: a beacon has no way
 * to learn when HiveHub's short passive scan lands, so it must always be on air.
 * That is nearly free — the controller emits one short advertising event per
 * BEACON_ADV_INTERVAL (<1 ms of radio time) while the CPU sleeps, so the radio
 * is active well under 0.1 % of the time. Only the payload is refreshed each
 * measurement cycle via bt_le_adv_update_data(); the encode is cheap and the
 * sensor rail — not the radio — is what dominates the energy budget.
 */
#include "beacon.h"

#include "hive_config.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <string.h>

#if defined(CONFIG_BT)

#include <zephyr/bluetooth/bluetooth.h>
#include <math.h>

/* Frame layout — mirrors HiveHub's HI_OFF_* / HI_FLAG_* constants. */
#define FRAME_LEN       26
#define OFF_COMPANY      0
#define OFF_MAGIC        2
#define OFF_VERSION      3
#define OFF_FLAGS        4
#define OFF_TEMP         5
#define OFF_HUMID        7
#define OFF_BATT_MV      9
#define OFF_BATT_PCT    11
#define OFF_ACC_RMS     12
#define OFF_ACC_SWARM   14
#define OFF_ACC_FANNING 16
#define OFF_ACC_ACTIVITY 18
#define OFF_MIC_RMS     20
#define OFF_MIC_SUB     21
#define OFF_MIC_HUM     22
#define OFF_MIC_PIPING  23
#define OFF_MIC_STRESS  24
#define OFF_MIC_HIGH    25

#define FLAG_SHT   (1 << 0)
#define FLAG_ACCEL (1 << 1)
#define FLAG_MIC   (1 << 2)
#define FLAG_BATT  (1 << 3)

/* 0.1-unit fixed point for temperature, humidity and the accel mg fields. */
#define SCALE_DECI 10.0f

/* Advertising interval in 0.625 ms units (BLE spec granularity). */
#define ADV_INT_UNITS(ms) ((uint32_t)((ms) * 8 / 5))

/* The live advertisement payload. Seeded with the constant header; the sensor
 * fields are filled by beacon_update() and re-read by bt_le_adv_update_data(). */
static uint8_t s_frame[FRAME_LEN];
static bool    s_ready;      /* advertising started successfully */
static bool    s_scannable;  /* scan response (name) is in use */

static const struct bt_data s_ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_NO_BREDR),
	BT_DATA(BT_DATA_MANUFACTURER_DATA, s_frame, sizeof(s_frame)),
};
static const struct bt_data s_sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, BEACON_NAME, sizeof(BEACON_NAME) - 1),
};

/* Non-connectable, undirected. USE_IDENTITY advertises the stable identity
 * (static random) address so the MAC the operator pairs in the HiveHub portal
 * does not change. Providing a scan response makes it a scannable beacon. */
static const struct bt_le_adv_param s_adv_param = BT_LE_ADV_PARAM_INIT(
	BT_LE_ADV_OPT_USE_IDENTITY,
	ADV_INT_UNITS(BEACON_ADV_INTERVAL_MIN_MS),
	ADV_INT_UNITS(BEACON_ADV_INTERVAL_MAX_MS),
	NULL);

static void put_u16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xFF);
	p[1] = (uint8_t)(v >> 8);
}

static uint16_t u16_clamp(float x)
{
	long v = lroundf(x);

	if (v < 0) {
		v = 0;
	}
	if (v > 65535) {
		v = 65535;
	}
	return (uint16_t)v;
}

static int16_t s16_clamp(float x)
{
	long v = lroundf(x);

	if (v < INT16_MIN) {
		v = INT16_MIN;
	}
	if (v > INT16_MAX) {
		v = INT16_MAX;
	}
	return (int16_t)v;
}

static int8_t s8_clamp(float x)
{
	long v = lroundf(x);

	if (v < INT8_MIN) {
		v = INT8_MIN;
	}
	if (v > INT8_MAX) {
		v = INT8_MAX;
	}
	return (int8_t)v;
}

static void print_identity(void)
{
	bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
	size_t count = ARRAY_SIZE(addrs);
	char str[BT_ADDR_LE_STR_LEN];

	bt_id_get(addrs, &count);
	if (count == 0) {
		return;
	}
	bt_addr_le_to_str(&addrs[0], str, sizeof(str));
	printk("[BLE] beacon address %s — pair this MAC in HiveHub\n", str);
}

int beacon_init(void)
{
	int err;

	/* Constant header; sensor fields start cleared (all groups "absent"). */
	memset(s_frame, 0, sizeof(s_frame));
	put_u16(&s_frame[OFF_COMPANY], (uint16_t)BEACON_COMPANY_ID);
	s_frame[OFF_MAGIC]   = BEACON_MAGIC;
	s_frame[OFF_VERSION] = BEACON_VERSION;

	err = bt_enable(NULL);
	if (err) {
		printk("[BLE] bt_enable failed (%d); running readout only\n", err);
		return err;
	}

	/* Prefer a scannable beacon so the friendly name rides in the scan
	 * response. If a Zephyr build rejects a legacy scan response on a custom
	 * non-connectable param, fall back to advertising the measurement frame
	 * alone — HiveHub pairs by MAC, so only the display name is lost. */
	err = bt_le_adv_start(&s_adv_param, s_ad, ARRAY_SIZE(s_ad),
			      s_sd, ARRAY_SIZE(s_sd));
	if (err == 0) {
		s_scannable = true;
	} else {
		printk("[BLE] scannable adv rejected (%d); retrying without name\n", err);
		err = bt_le_adv_start(&s_adv_param, s_ad, ARRAY_SIZE(s_ad), NULL, 0);
		if (err) {
			printk("[BLE] bt_le_adv_start failed (%d)\n", err);
			return err;
		}
	}

	s_ready = true;
	printk("[BLE] beacon advertising every %u–%u ms (non-connectable)\n",
	       (unsigned)BEACON_ADV_INTERVAL_MIN_MS,
	       (unsigned)BEACON_ADV_INTERVAL_MAX_MS);
	print_identity();
	return 0;
}

void beacon_update(const struct measurement *m)
{
	int err;

	if (!s_ready) {
		return;
	}

	uint8_t flags = 0;

	if (m->sht_ok) {
		flags |= FLAG_SHT;
		put_u16(&s_frame[OFF_TEMP],  (uint16_t)s16_clamp(m->temp_c * SCALE_DECI));
		put_u16(&s_frame[OFF_HUMID], u16_clamp(m->humidity_pct * SCALE_DECI));
	} else {
		put_u16(&s_frame[OFF_TEMP], 0);
		put_u16(&s_frame[OFF_HUMID], 0);
	}

	if (m->battery_ok) {
		flags |= FLAG_BATT;
		put_u16(&s_frame[OFF_BATT_MV], u16_clamp(m->battery_v * 1000.0f));
		s_frame[OFF_BATT_PCT] = m->battery_pct;
	} else {
		put_u16(&s_frame[OFF_BATT_MV], 0);
		s_frame[OFF_BATT_PCT] = 0;
	}

	if (m->accel_ok) {
		flags |= FLAG_ACCEL;
		put_u16(&s_frame[OFF_ACC_RMS],      u16_clamp(m->accel_rms_mg * SCALE_DECI));
		put_u16(&s_frame[OFF_ACC_SWARM],    u16_clamp(m->accel_band_swarm_mg * SCALE_DECI));
		put_u16(&s_frame[OFF_ACC_FANNING],  u16_clamp(m->accel_band_fanning_mg * SCALE_DECI));
		put_u16(&s_frame[OFF_ACC_ACTIVITY], u16_clamp(m->accel_band_activity_mg * SCALE_DECI));
	} else {
		put_u16(&s_frame[OFF_ACC_RMS], 0);
		put_u16(&s_frame[OFF_ACC_SWARM], 0);
		put_u16(&s_frame[OFF_ACC_FANNING], 0);
		put_u16(&s_frame[OFF_ACC_ACTIVITY], 0);
	}

	if (m->mic_ok) {
		flags |= FLAG_MIC;
		s_frame[OFF_MIC_RMS]    = (uint8_t)s8_clamp(m->mic_rms_dbfs);
		s_frame[OFF_MIC_SUB]    = (uint8_t)s8_clamp(m->mic_band_sub_bass_dbfs);
		s_frame[OFF_MIC_HUM]    = (uint8_t)s8_clamp(m->mic_band_hum_dbfs);
		s_frame[OFF_MIC_PIPING] = (uint8_t)s8_clamp(m->mic_band_piping_dbfs);
		s_frame[OFF_MIC_STRESS] = (uint8_t)s8_clamp(m->mic_band_stress_dbfs);
		s_frame[OFF_MIC_HIGH]   = (uint8_t)s8_clamp(m->mic_band_high_dbfs);
	} else {
		s_frame[OFF_MIC_RMS] = 0;
		s_frame[OFF_MIC_SUB] = 0;
		s_frame[OFF_MIC_HUM] = 0;
		s_frame[OFF_MIC_PIPING] = 0;
		s_frame[OFF_MIC_STRESS] = 0;
		s_frame[OFF_MIC_HIGH] = 0;
	}

	s_frame[OFF_FLAGS] = flags;

	/* Re-publish the payload; the running advertiser picks it up for its next
	 * event. s_ad points at s_frame, so no copy back into s_ad is needed. */
	err = bt_le_adv_update_data(s_ad, ARRAY_SIZE(s_ad),
				    s_scannable ? s_sd : NULL,
				    s_scannable ? ARRAY_SIZE(s_sd) : 0);
	if (err) {
		printk("[BLE] adv update failed (%d)\n", err);
	}
}

#else /* !CONFIG_BT */

int beacon_init(void)
{
	printk("[BLE] disabled at build time (CONFIG_BT=n)\n");
	return -ENOTSUP;
}

void beacon_update(const struct measurement *m)
{
	ARG_UNUSED(m);
}

#endif /* CONFIG_BT */

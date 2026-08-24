/*
 * HiveInside manufacturer-data beacon.
 *
 * Version 2 extends the 26-byte format consumed by existing HiveHubs with
 * acceleration and microphone peaks at bytes 26..28. Keeping the version-1
 * prefix unchanged preserves core-data compatibility, while the complete
 * measurement remains in the primary advertising packet for passive scans.
 * Active setup scans can additionally read the local name and the compact
 * board/firmware identity record from the response. The advertised name
 * carries the last two bytes of the device's own BLE address (see
 * device_name_init() below), so several nodes in range stay distinguishable
 * in a scanner list without any per-unit build or provisioning step.
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
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#define FRAME_SIZE 29U
#define FRAME_MAGIC 0x48U /* 'H' */
#define FRAME_VERSION 0x02U

/* Compact board/firmware record for active scanners.  It is deliberately
 * constant and kept out of the console formatting path.  PR #44 changed both
 * paths together, which made it impossible to isolate the reported console
 * regression. */
#define IDENTITY_MAGIC 0x49U /* 'I' */
#define IDENTITY_VERSION 0x01U
#define IDENTITY_SIZE 8U

static const uint8_t identity[IDENTITY_SIZE] = {
	(uint8_t)(HIVEINSIDE_COMPANY_ID & 0xffU),
	(uint8_t)(HIVEINSIDE_COMPANY_ID >> 8),
	IDENTITY_MAGIC,
	IDENTITY_VERSION,
	HIVEINSIDE_BOARD_ID,
	HIVEINSIDE_FW_VERSION_MAJOR,
	HIVEINSIDE_FW_VERSION_MINOR,
	HIVEINSIDE_FW_VERSION_PATCH,
};

/* The identity record carries each version component in a single byte, so a
 * version bump past 255 would silently truncate on the air — the same class of
 * failure as the version never changing at all. */
BUILD_ASSERT(HIVEINSIDE_FW_VERSION_MAJOR >= 0 && HIVEINSIDE_FW_VERSION_MAJOR <= UINT8_MAX &&
	     HIVEINSIDE_FW_VERSION_MINOR >= 0 && HIVEINSIDE_FW_VERSION_MINOR <= UINT8_MAX &&
	     HIVEINSIDE_FW_VERSION_PATCH >= 0 && HIVEINSIDE_FW_VERSION_PATCH <= UINT8_MAX,
	     "each firmware version component must fit the one-byte identity field");

/* The advertised local name is HIVEINSIDE_DEVICE_NAME with the last two bytes
 * of the device's BLE address appended as "-AB:12", i.e. six extra characters.
 * The buffer is filled once at init and then referenced by every scan-response
 * update, so it is sized here rather than on the stack. */
#define DEVICE_NAME_SUFFIX_LEN 6U

static char device_name[sizeof(HIVEINSIDE_DEVICE_NAME) + DEVICE_NAME_SUFFIX_LEN];
static uint8_t device_name_len;

/* A legacy scan response is limited to 31 bytes. Each AD structure adds a
 * length and type byte, hence the two +2 terms below. The name is checked at
 * its longest — the full suffixed form — because that is what actually goes on
 * the air; a name that only fits without its address suffix would still be
 * rejected at runtime by the controller. */
BUILD_ASSERT((sizeof(device_name) - 1U + 2U) +
	     (sizeof(identity) + 2U) <= 31U,
	     "name and identity exceed legacy scan-response capacity");

#define FLAG_SHT   (1U << 0)
#define FLAG_ACCEL (1U << 1)
#define FLAG_MIC   (1U << 2)
#define FLAG_BATT  (1U << 3)

static uint8_t frame[FRAME_SIZE];
static bool bluetooth_ready;
static bool advertising;

/* Restarting connectable advertising out of the disconnected callback.
 *
 * With legacy (non-extended) connectable advertising the host reserves a
 * connection object the moment advertising starts — le_adv_start_add_conn()
 * calls bt_conn_add_le() and returns -ENOMEM when the pool is empty. This
 * firmware sets CONFIG_BT_MAX_CONN=1, and inside the disconnected callback the
 * stack is still holding its own reference to the connection that just went
 * away: it is released only after every callback has returned. Calling
 * bt_le_adv_start() there therefore fails with -ENOMEM every single time —
 * "[BLE] advertising restart failed (-12)" after each OTA attempt — and the
 * node stays off the air until the next measurement cycle happens to retry it,
 * up to MEASURE_INTERVAL_MS later. For a hive that is five minutes of looking
 * exactly like a flat battery.
 *
 * Deferring the restart to the system workqueue lets the callback return and
 * the connection object be freed first. The short delay is what makes that
 * ordering reliable rather than a race against the Bluetooth RX thread, and
 * the bounded retry covers the case where the pool is still busy — a slot in
 * which the old advertising-reserved object has not been recycled yet.
 */
#define ADV_RESTART_DELAY K_MSEC(50)
#define ADV_RESTART_RETRY K_MSEC(250)
#define ADV_RESTART_ATTEMPTS 5U

static void adv_restart_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(adv_restart_work, adv_restart_handler);
static uint8_t adv_restart_attempts;

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

/* Start legacy connectable undirected advertising (ADV_IND) carrying the last
 * encoded measurement: the reading stays in the primary packet for HiveHub's
 * passive scan while the "HiveInside" name rides in the scan response for
 * active setup scans. Only the legacy PDU allows both payloads at once, so
 * never let extended advertising be selected here.
 *
 * The Flags AD element is omitted: dropping those three bytes leaves the
 * complete 31-byte legacy payload for the manufacturer element (29 data bytes
 * plus its length and type). Without Flags the device is formally
 * non-discoverable — a scanner filtering on the discoverable bits may not list
 * it — but HiveHub's passive scan and a connect by address, which is how the
 * OTA relay reaches it, are unaffected.
 */
static int advertising_start(void)
{
	const struct bt_data ad[] = {
		BT_DATA(BT_DATA_MANUFACTURER_DATA, frame, sizeof(frame)),
	};
	const struct bt_data scan_response[] = {
		BT_DATA(BT_DATA_NAME_COMPLETE, device_name, device_name_len),
		BT_DATA(BT_DATA_MANUFACTURER_DATA, identity, sizeof(identity)),
	};
	struct bt_le_adv_param param = BT_LE_ADV_PARAM_INIT(
		BT_LE_ADV_OPT_USE_IDENTITY | BT_LE_ADV_OPT_CONN,
		BLE_ADV_INTERVAL_UNITS, BLE_ADV_INTERVAL_UNITS, NULL);
	int err = bt_le_adv_start(&param, ad, ARRAY_SIZE(ad), scan_response,
				  ARRAY_SIZE(scan_response));

	if (err == 0 || err == -EALREADY) {
		advertising = true;
		return 0;
	}
	return err;
}

static void adv_restart_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!bluetooth_ready || advertising) {
		return;
	}

	int err = advertising_start();

	if (err == 0) {
		return;
	}
	if (++adv_restart_attempts < ADV_RESTART_ATTEMPTS) {
		(void)k_work_reschedule(&adv_restart_work, ADV_RESTART_RETRY);
		return;
	}
	/* Out of retries. The next beacon_publish() still restarts advertising,
	 * so this is a delay rather than a permanent silence — but it is the
	 * point at which it stops being a transient. */
	printk("[BLE] advertising restart failed (%d) after %u attempts\n", err,
	       (unsigned)adv_restart_attempts);
}

/* Build the advertised name once, after bt_enable() has established the
 * identity address.
 *
 * The suffix is the last two bytes of that address, formatted the way a
 * scanner prints them, so "HiveInside-AB:12" is literally the tail of the
 * address shown next to it in the list. bt_addr_le_t stores the address
 * little-endian (val[5] is the leading byte a scanner prints), hence val[1]
 * and val[0] here.
 *
 * That the two agree is a property of how this firmware advertises:
 * advertising_start() passes BT_LE_ADV_OPT_USE_IDENTITY, so the packets carry
 * the identity address itself rather than a rotating resolvable private
 * address. On the nRF54 the identity address is derived from the factory
 * FICR.DEVICEADDR, which makes the suffix stable across reboots and reflashes
 * and unique per unit without any provisioning step.
 *
 * If the identity cannot be read the plain product name is used: an unsuffixed
 * name is a far better failure than no advertising at all.
 *
 * The two bytes are formatted by hand rather than with snprintk() on purpose.
 * The low-power deployment profile sets CONFIG_PRINTK=n, under which Zephyr
 * replaces snprintk() with a stub that writes nothing and returns 0 — the name
 * would silently become empty in exactly the build that ships to hives, while
 * the console-enabled bringup build looked fine.
 */
static void device_name_init(void)
{
	static const char hex_digits[] = "0123456789ABCDEF";
	bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
	size_t count = ARRAY_SIZE(addrs);
	size_t len = sizeof(HIVEINSIDE_DEVICE_NAME) - 1U;

	memcpy(device_name, HIVEINSIDE_DEVICE_NAME, len);
	bt_id_get(addrs, &count);

	if (count > 0U) {
		const uint8_t *val = addrs[0].a.val;

		device_name[len++] = '-';
		device_name[len++] = hex_digits[val[1] >> 4];
		device_name[len++] = hex_digits[val[1] & 0x0fU];
		device_name[len++] = ':';
		device_name[len++] = hex_digits[val[0] >> 4];
		device_name[len++] = hex_digits[val[0] & 0x0fU];
	} else {
		printk("[BLE] no identity address; advertising unsuffixed name\n");
	}

	device_name[len] = '\0';
	device_name_len = (uint8_t)len;

	/* Keep the GAP Device Name characteristic in step with the advertised
	 * one, so a client that connects and reads it (rather than trusting the
	 * scan response) sees the same identity. */
	int err = bt_set_name(device_name);

	if (err != 0) {
		printk("[BLE] GAP name not updated (%d)\n", err);
	}
}

int beacon_init(void)
{
	int err = bt_enable(NULL);

	if (err != 0) {
		printk("[BLE] init failed (%d)\n", err);
		return err;
	}
	device_name_init();
	printk("[BLE] ready; name=%s manufacturer=%s id=0x%04x interval=%u ms\n",
	       device_name, HIVEINSIDE_MANUFACTURER_NAME,
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

	int err;

	if (!advertising) {
		/* A pending deferred restart would only race with this one. */
		(void)k_work_cancel_delayable(&adv_restart_work);
		err = advertising_start();
	} else {
		const struct bt_data ad[] = {
			BT_DATA(BT_DATA_MANUFACTURER_DATA, frame, sizeof(frame)),
		};
		const struct bt_data scan_response[] = {
			BT_DATA(BT_DATA_NAME_COMPLETE, device_name,
				device_name_len),
			BT_DATA(BT_DATA_MANUFACTURER_DATA, identity,
				sizeof(identity)),
		};

		err = bt_le_adv_update_data(ad, ARRAY_SIZE(ad), scan_response,
					ARRAY_SIZE(scan_response));
	}

	if (err != 0) {
		printk("[BLE] measurement publish failed (%d)\n", err);
	} else {
		printk("[BLE] measurement advertised (flags=0x%02x)\n", frame[4]);
	}
	return err;
}

void beacon_connected(void)
{
	advertising = false;
	(void)k_work_cancel_delayable(&adv_restart_work);
}

void beacon_disconnected(void)
{
	/* Deliberately does not call bt_le_adv_start() here — see the comment on
	 * adv_restart_work above for why that can only ever return -ENOMEM. */
	adv_restart_attempts = 0;
	(void)k_work_reschedule(&adv_restart_work, ADV_RESTART_DELAY);
}

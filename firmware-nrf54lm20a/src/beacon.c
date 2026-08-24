/*
 * HiveInside manufacturer-data beacon.
 *
 * Version 2 extends the 26-byte format consumed by existing HiveHubs with
 * acceleration and microphone peaks at bytes 26..28. Keeping the version-1
 * prefix unchanged preserves core-data compatibility, while the complete
 * measurement remains in the primary advertising packet for passive scans.
 * Active setup scans can additionally read the local name — which carries the
 * last two bytes of the node's address, so several nodes in one yard are
 * distinguishable — and the compact board/firmware identity record from the
 * response.
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

/* A legacy scan response is limited to 31 bytes. Each AD structure adds a
 * length and type byte, hence the two +2 terms below. The name is measured at
 * its full length — prefix plus the "-XXXX" address suffix beacon_init() adds —
 * because that is what actually goes on the air. */
BUILD_ASSERT((HIVEINSIDE_DEVICE_NAME_MAX + 2U) + (sizeof(identity) + 2U) <= 31U,
	     "name and identity exceed legacy scan-response capacity");

/* bt_set_name() copies into a buffer sized by CONFIG_BT_DEVICE_NAME_MAX; if
 * that is trimmed below the name this firmware builds, the GAP characteristic
 * silently disagrees with the scan response. */
BUILD_ASSERT(CONFIG_BT_DEVICE_NAME_MAX >= HIVEINSIDE_DEVICE_NAME_MAX,
	     "CONFIG_BT_DEVICE_NAME_MAX is too small for the advertised name");

/* The advertised local name, built once in beacon_init(): HIVEINSIDE_DEVICE_NAME
 * with a hyphen and the last two bytes of the identity address appended, e.g.
 * "HiveInside-8A3F". It is a runtime buffer rather than a literal because the
 * address is only known after bt_enable() has brought the identity up, and it is
 * kept in one place so the scan response, the GAP name a connected OTA client
 * reads, and the boot banner cannot drift apart.
 *
 * Until the buffer is filled it holds the bare prefix, so a node whose address
 * could not be read still advertises a usable — if not unique — name instead of
 * an empty one.
 */
static char adv_name[HIVEINSIDE_DEVICE_NAME_MAX + 1U] = HIVEINSIDE_DEVICE_NAME;
static size_t adv_name_len = sizeof(HIVEINSIDE_DEVICE_NAME) - 1U;

/* Append "-XXXX" from the identity address. bt_id_get() reports the addresses
 * the stack created during bt_enable(), so this must run after it. The two
 * bytes used are val[1] and val[0] — a bt_addr_t is little-endian, so those are
 * the last two of the address as it is printed and as HiveHub shows it, which
 * is what makes the suffix something a beekeeper can match against a label.
 */
static void build_adv_name(void)
{
	bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
	size_t count = ARRAY_SIZE(addrs);

	bt_id_get(addrs, &count);
	if (count == 0U) {
		printk("[BLE] no identity address; advertising as %s\n", adv_name);
		return;
	}

	size_t room = sizeof(adv_name) - adv_name_len;
	int written = snprintk(adv_name + adv_name_len, room, "-%02X%02X",
			       addrs[0].a.val[1], addrs[0].a.val[0]);

	/* snprintk returns what it *would* have written, so a truncated suffix
	 * reports a length at or past the room it was given. Either way the
	 * buffer now holds a partial suffix, which is worse on the air than none
	 * at all — a name that looks unique but is not. */
	if (written <= 0 || (size_t)written >= room) {
		adv_name[adv_name_len] = '\0';
		printk("[BLE] address suffix did not fit; advertising as %s\n",
		       adv_name);
		return;
	}
	adv_name_len += (size_t)written;

	/* Keep the GAP Device Name characteristic in step with the scan
	 * response. A HiveHub OTA relay connects by address and never reads it,
	 * but every generic BLE app a beekeeper might debug with does, and a
	 * connected node calling itself something other than what it advertised
	 * is exactly the kind of mismatch that costs an afternoon. */
	int err = bt_set_name(adv_name);

	if (err != 0) {
		printk("[BLE] setting GAP name failed (%d)\n", err);
	}
}

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
 * passive scan while the "HiveInside-XXXX" name rides in the scan response for
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
		BT_DATA(BT_DATA_NAME_COMPLETE, adv_name, adv_name_len),
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

int beacon_init(void)
{
	int err = bt_enable(NULL);

	if (err != 0) {
		printk("[BLE] init failed (%d)\n", err);
		return err;
	}
	build_adv_name();
	printk("[BLE] ready; name=%s manufacturer=%s id=0x%04x interval=%u ms\n",
	       adv_name, HIVEINSIDE_MANUFACTURER_NAME,
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
			BT_DATA(BT_DATA_NAME_COMPLETE, adv_name, adv_name_len),
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

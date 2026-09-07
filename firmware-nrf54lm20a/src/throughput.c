/* throughput.c — see throughput.h. TEMPORARY; delete once phase 0e is answered.
 *
 * Protocol (same UUID family as the OTA service, 0x8e8b00fx so it cannot
 * collide with a future audio service):
 *
 *   CTRL   write  0x01 duration_s payload_len [interval_units]  start a run
 *                 0x02                                          stop early
 *   DATA   notify seq(4 LE) + filler, `payload_len` bytes total
 *   STATUS read/notify  state(1) bytes(4 LE) ms(4 LE) notifs(4 LE)
 *                       payload_len(1) mtu(2 LE)
 *
 * `interval_units` is optional and in the BLE 1.25 ms unit, so the client can
 * sweep the connection interval without a reflash — that and
 * CONFIG_BT_BUF_ACL_TX_COUNT are the two knobs most likely to move the number.
 *
 * The sequence number at the head of every notification is what lets the
 * receiver tell "the link is slow" from "the link is dropping packets": the
 * device reports what it handed to the controller, the client reports what
 * arrived, and a gap between the two is loss rather than throughput.
 */
#include "throughput.h"

#include "hive_config.h"

#if ENABLE_THROUGHPUT_SPIKE

#include "ota.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/bluetooth/att.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

enum { TP_OP_START = 0x01, TP_OP_STOP = 0x02 };
enum {
	TP_IDLE = 0x00, TP_RUNNING = 0x01, TP_DONE = 0x02,
	TP_ERR_PARAM = 0x10, TP_ERR_NO_LINK = 0x11, TP_ERR_NO_SUB = 0x12,
};

/* Credits mirror the controller's TX buffers exactly: one notification in
 * flight per buffer, released by the completion callback. Sweeping
 * CONFIG_BT_BUF_ACL_TX_COUNT therefore sweeps how many notifications can be
 * queued into a single connection event, which is the whole point of the
 * experiment. */
#define TP_CREDITS CONFIG_BT_BUF_ACL_TX_COUNT

/* Largest notification this build will send: one ATT MTU (247) minus the 3-byte
 * notification header, which is exactly what gatt_ota.cpp already negotiates for
 * OTA writes. */
#define TP_PAYLOAD_MAX 244

static struct bt_conn *tp_conn;
static uint8_t state;
static uint32_t bytes_sent, notifs_sent, elapsed_ms;
static uint16_t payload_len;
static uint8_t duration_s;
static bool subscribed;

/* Not `static`: the K_*_DEFINE macros declare section-iterable objects, and
 * ota.c's K_WORK_DELAYABLE_DEFINE uses the same plain form. */
K_SEM_DEFINE(tp_start_sem, 0, 1);
K_SEM_DEFINE(tp_credits, TP_CREDITS, TP_CREDITS);

static ssize_t ctrl_write(struct bt_conn *, const struct bt_gatt_attr *,
			  const void *, uint16_t, uint16_t, uint8_t);
static ssize_t status_read(struct bt_conn *, const struct bt_gatt_attr *,
			   void *, uint16_t, uint16_t);
static void data_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);

#define UUID128(n) BT_UUID_128_ENCODE(n, 0x7a1c, 0x4b9e, 0x9a2f, 0x1d6e0b9c1a01)
static struct bt_uuid_128 service_uuid = BT_UUID_INIT_128(UUID128(0x8e8b00f0));
static struct bt_uuid_128 ctrl_uuid = BT_UUID_INIT_128(UUID128(0x8e8b00f1));
static struct bt_uuid_128 data_uuid = BT_UUID_INIT_128(UUID128(0x8e8b00f2));
static struct bt_uuid_128 status_uuid = BT_UUID_INIT_128(UUID128(0x8e8b00f3));

BT_GATT_SERVICE_DEFINE(tp_service,
	BT_GATT_PRIMARY_SERVICE(&service_uuid.uuid),
	BT_GATT_CHARACTERISTIC(&ctrl_uuid.uuid, BT_GATT_CHRC_WRITE,
		BT_GATT_PERM_WRITE, NULL, ctrl_write, NULL),
	BT_GATT_CHARACTERISTIC(&data_uuid.uuid, BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(data_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(&status_uuid.uuid,
		BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_READ, status_read, NULL, NULL),
	BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE));

/* Attribute indices into the table above. Named rather than spelled inline so a
 * later edit to the table cannot silently notify the wrong characteristic. */
#define TP_ATTR_DATA   4
#define TP_ATTR_STATUS 7

#define TP_STATUS_LEN 16

static void fill_status(uint8_t v[TP_STATUS_LEN])
{
	v[0] = state;
	sys_put_le32(bytes_sent, &v[1]);
	sys_put_le32(elapsed_ms, &v[5]);
	sys_put_le32(notifs_sent, &v[9]);
	v[13] = (uint8_t)MIN(payload_len, 255U);
	sys_put_le16(tp_conn ? bt_gatt_get_mtu(tp_conn) : 0, &v[14]);
}

static void publish_status(void)
{
	uint8_t v[TP_STATUS_LEN];

	fill_status(v);
	(void)bt_gatt_notify(NULL, &tp_service.attrs[TP_ATTR_STATUS], v, sizeof(v));
}

static void data_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	subscribed = (value == BT_GATT_CCC_NOTIFY);
	printk("[TP] DATA notifications %s\n", subscribed ? "enabled" : "disabled");
}

static void sent_cb(struct bt_conn *conn, void *user_data)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(user_data);
	k_sem_give(&tp_credits);
}

static void tp_thread_fn(void *a, void *b, void *c)
{
	/* Static, not on the stack: the payload is up to one full ATT MTU and
	 * this thread's stack is sized for the Bluetooth call chain, not for a
	 * buffer. */
	static uint8_t buf[TP_PAYLOAD_MAX];

	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	memset(buf, 0xA5, sizeof(buf));

	for (;;) {
		k_sem_take(&tp_start_sem, K_FOREVER);

		/* Reset the credit pool for this run: a previous run that ended
		 * with notifications still in flight leaves the semaphore short,
		 * and the next run would then be measuring the leftover instead
		 * of the link. */
		k_sem_reset(&tp_credits);
		for (int i = 0; i < TP_CREDITS; i++) {
			k_sem_give(&tp_credits);
		}

		bytes_sent = 0;
		notifs_sent = 0;
		elapsed_ms = 0;

		const int64_t start = k_uptime_get();
		const int64_t deadline = start + (int64_t)duration_s * 1000;
		uint32_t seq = 0;

		printk("[TP] run start: %u s, %u B payload, %u credits\n",
		       (unsigned)duration_s, (unsigned)payload_len,
		       (unsigned)TP_CREDITS);

		while (state == TP_RUNNING && tp_conn != NULL &&
		       k_uptime_get() < deadline) {
			if (k_sem_take(&tp_credits, K_MSEC(500)) != 0) {
				/* Half a second without a single completion is not
				 * slowness, it is a link that has stopped moving. */
				printk("[TP] no TX completion for 500 ms\n");
				break;
			}

			sys_put_le32(seq, buf);

			struct bt_gatt_notify_params params = {
				.attr = &tp_service.attrs[TP_ATTR_DATA],
				.data = buf,
				.len = payload_len,
				.func = sent_cb,
			};
			int err = bt_gatt_notify_cb(tp_conn, &params);

			if (err != 0) {
				k_sem_give(&tp_credits);
				if (err == -ENOMEM) {
					/* Controller buffers momentarily gone
					 * despite the credit; yield and retry
					 * rather than counting it as an error. */
					k_yield();
					continue;
				}
				printk("[TP] notify failed (%d) after %u B\n", err,
				       (unsigned)bytes_sent);
				break;
			}

			bytes_sent += payload_len;
			notifs_sent++;
			seq++;
		}

		elapsed_ms = (uint32_t)(k_uptime_get() - start);
		state = TP_DONE;

		/* Integer arithmetic only: this build may or may not have the
		 * cbprintf float formatter, and a throughput figure does not
		 * need a fraction. */
		uint32_t bps = elapsed_ms ? (bytes_sent * 1000U) / elapsed_ms : 0U;

		printk("[TP] run done: %u B in %u ms = %u B/s (%u notifications of %u B)\n",
		       (unsigned)bytes_sent, (unsigned)elapsed_ms, (unsigned)bps,
		       (unsigned)notifs_sent, (unsigned)payload_len);
		publish_status();
	}
}

K_THREAD_DEFINE(tp_thread, 2048, tp_thread_fn, NULL, NULL, NULL, 7, 0, 0);

static ssize_t ctrl_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  const void *buf, uint16_t len, uint16_t offset,
			  uint8_t flags)
{
	const uint8_t *p = buf;

	ARG_UNUSED(attr); ARG_UNUSED(flags);
	if (offset || !len) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	switch (p[0]) {
	case TP_OP_START: {
		if (len < 3 || state == TP_RUNNING) {
			state = TP_ERR_PARAM;
			publish_status();
			return len;
		}
		if (!tp_conn) {
			state = TP_ERR_NO_LINK;
			publish_status();
			return len;
		}
		if (!subscribed) {
			/* Notifying an unsubscribed characteristic silently does
			 * nothing, which would read as "0 B/s" and send someone
			 * hunting a radio problem that does not exist. */
			state = TP_ERR_NO_SUB;
			publish_status();
			printk("[TP] START refused: client has not subscribed to DATA\n");
			return len;
		}

		duration_s = CLAMP(p[1], 1, 60);

		uint16_t mtu = bt_gatt_get_mtu(conn);
		uint16_t max_payload = (mtu > 3) ? (uint16_t)(mtu - 3) : 20;

		if (max_payload > 244) {
			max_payload = 244;
		}
		payload_len = CLAMP(p[2], 20, max_payload);

		/* Optional fourth byte: connection interval in 1.25 ms units, so
		 * the interval can be swept from the client. The supervision
		 * timeout is the OTA service's two seconds, which comfortably
		 * clears every interval this accepts. */
		if (len >= 4 && p[3] != 0) {
			struct bt_le_conn_param param = {
				.interval_min = p[3],
				.interval_max = p[3],
				.latency = 0,
				.timeout = 200,
			};
			int err = bt_conn_le_param_update(conn, &param);

			printk("[TP] requested interval %u units (%u.%02u ms): %d\n",
			       (unsigned)p[3], (unsigned)(p[3] * 5U / 4U),
			       (unsigned)((p[3] * 500U / 4U) % 100U), err);
		}

#if defined(CONFIG_BT_USER_DATA_LEN_UPDATE)
		(void)bt_conn_le_data_len_update(conn, BT_LE_DATA_LEN_PARAM_MAX);
#endif
#if defined(CONFIG_BT_USER_PHY_UPDATE)
		(void)bt_conn_le_phy_update(conn, BT_CONN_LE_PHY_PARAM_2M);
#endif

		/* ota.c disconnects any central that has not sent BEGIN within
		 * six seconds. That guard is right for the OTA service and fatal
		 * for a twenty-second measurement, so stand it down for this
		 * link. */
		ota_release_arm_timeout();

		state = TP_RUNNING;
		publish_status();
		k_sem_give(&tp_start_sem);
		return len;
	}
	case TP_OP_STOP:
		if (state == TP_RUNNING) {
			state = TP_DONE;
		}
		return len;
	default:
		state = TP_ERR_PARAM;
		publish_status();
		return len;
	}
}

static ssize_t status_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			   void *buf, uint16_t len, uint16_t offset)
{
	uint8_t v[TP_STATUS_LEN];

	fill_status(v);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, v, sizeof(v));
}

/* ── Link parameter logging (phase 0b) ─────────────────────────────────────
 *
 * A second set of connection callbacks alongside ota.c's. Zephyr calls every
 * registered set, and these deliberately do NOT touch the beacon: advertising
 * is ota.c's to restart, and calling beacon_disconnected() from two places
 * would schedule the restart work twice.
 */
static void tp_connected(struct bt_conn *conn, uint8_t conn_err)
{
	struct bt_conn_info info;

	if (conn_err) {
		return;
	}
	tp_conn = bt_conn_ref(conn);
	subscribed = false;
	state = TP_IDLE;

	if (bt_conn_get_info(conn, &info) == 0 && info.type == BT_CONN_TYPE_LE) {
		/* interval_us, not the 1.25 ms `interval` field: that one is
		 * deprecated (it cannot represent the shorter intervals
		 * CONFIG_BT_SHORTER_CONNECTION_INTERVALS allows) and NCS builds
		 * deprecation warnings as errors. `latency` and `timeout` are
		 * unaffected. */
		printk("[TP] connected: interval=%u us (%u.%02u ms) latency=%u timeout=%u ms\n",
		       (unsigned)info.le.interval_us,
		       (unsigned)(info.le.interval_us / 1000U),
		       (unsigned)((info.le.interval_us % 1000U) / 10U),
		       (unsigned)info.le.latency,
		       (unsigned)info.le.timeout * 10U);
	}
}

static void tp_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	if (state == TP_RUNNING) {
		state = TP_IDLE;
	}
	subscribed = false;
	if (tp_conn) {
		bt_conn_unref(tp_conn);
		tp_conn = NULL;
	}
	printk("[TP] disconnected (0x%02x)\n", reason);
}

static void tp_param_updated(struct bt_conn *conn, uint16_t interval,
			     uint16_t latency, uint16_t timeout)
{
	ARG_UNUSED(conn);
	printk("[TP] interval now %u units (%u.%02u ms) latency=%u timeout=%u ms\n",
	       (unsigned)interval, (unsigned)(interval * 5U / 4U),
	       (unsigned)((interval * 500U / 4U) % 100U),
	       (unsigned)latency, (unsigned)timeout * 10U);
}

#if defined(CONFIG_BT_USER_PHY_UPDATE)
static void tp_phy_updated(struct bt_conn *conn,
			   struct bt_conn_le_phy_info *param)
{
	ARG_UNUSED(conn);
	/* 1 = 1M, 2 = 2M, 4 = Coded. 2M on both directions is what the 2 Mbit
	 * symbol rate needs; a run that reports 1M here is answering a different
	 * question than the one being asked. */
	printk("[TP] PHY now tx=%u rx=%u\n", (unsigned)param->tx_phy,
	       (unsigned)param->rx_phy);
}
#endif

#if defined(CONFIG_BT_USER_DATA_LEN_UPDATE)
static void tp_data_len_updated(struct bt_conn *conn,
				struct bt_conn_le_data_len_info *info)
{
	ARG_UNUSED(conn);
	/* tx_max_len 27 means DLE did not take, whatever the Kconfig says, and
	 * every notification is being fragmented across link-layer packets. */
	printk("[TP] data length now tx=%u B/%u us rx=%u B/%u us\n",
	       (unsigned)info->tx_max_len, (unsigned)info->tx_max_time,
	       (unsigned)info->rx_max_len, (unsigned)info->rx_max_time);
}
#endif

BT_CONN_CB_DEFINE(tp_conn_callbacks) = {
	.connected = tp_connected,
	.disconnected = tp_disconnected,
	.le_param_updated = tp_param_updated,
#if defined(CONFIG_BT_USER_PHY_UPDATE)
	.le_phy_updated = tp_phy_updated,
#endif
#if defined(CONFIG_BT_USER_DATA_LEN_UPDATE)
	.le_data_len_updated = tp_data_len_updated,
#endif
};

/* A banner rather than an init call, so main.c needs no #if of its own. It also
 * answers the first question any confusing result raises: is the board actually
 * running the spike image? */
static int tp_banner(void)
{
	printk("[TP] BLE throughput spike compiled in — measurement build, NOT for deployment\n");
	return 0;
}

SYS_INIT(tp_banner, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* ENABLE_THROUGHPUT_SPIKE */

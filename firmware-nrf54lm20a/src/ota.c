/* MCUboot firmware-over-BLE target. ATT responses are returned only after each
 * flash write completes, providing the relay's stream flow control. */
#include "ota.h"
#include "beacon.h"
#include "hive_config.h"

#include <zephyr/bluetooth/att.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>

#define OTA_ARM_TIMEOUT K_SECONDS(6)
#define OTA_STALL_TIMEOUT K_MSEC(HIVE_OTA_STALL_TIMEOUT_MS)
#define OTA_REBOOT_DELAY K_MSEC(1500)
#define OTA_SUPERVISION_TIMEOUT 200U /* 2 seconds, units of 10 ms */

enum { OTA_OP_BEGIN = 0x01, OTA_OP_END = 0x03, OTA_OP_ABORT = 0x04 };
enum {
	OTA_IDLE = 0x00, OTA_RECEIVING = 0x01, OTA_DONE = 0x02,
	OTA_ERR_BEGIN = 0x10, OTA_ERR_SEQ = 0x11, OTA_ERR_WRITE = 0x12,
	OTA_ERR_CRC = 0x13, OTA_ERR_SIZE = 0x14, OTA_ERR_END = 0x15,
};

static struct flash_img_context flash_ctx;
static struct bt_conn *active_conn;
static uint32_t expected_size, expected_crc, received, running_crc;
static uint8_t state, error;

static void arm_timeout_handler(struct k_work *work);
static void reboot_handler(struct k_work *work);
static void stall_timeout_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(arm_timeout_work, arm_timeout_handler);
K_WORK_DELAYABLE_DEFINE(reboot_work, reboot_handler);
K_WORK_DELAYABLE_DEFINE(stall_timeout_work, stall_timeout_handler);

static ssize_t ctrl_write(struct bt_conn *, const struct bt_gatt_attr *,
			  const void *, uint16_t, uint16_t, uint8_t);
static ssize_t data_write(struct bt_conn *, const struct bt_gatt_attr *,
			  const void *, uint16_t, uint16_t, uint8_t);
static ssize_t status_read(struct bt_conn *, const struct bt_gatt_attr *,
			   void *, uint16_t, uint16_t);

#define UUID128(n) BT_UUID_128_ENCODE(n, 0x7a1c, 0x4b9e, 0x9a2f, 0x1d6e0b9c1a01)
static struct bt_uuid_128 service_uuid = BT_UUID_INIT_128(UUID128(0x8e8b0001));
static struct bt_uuid_128 ctrl_uuid = BT_UUID_INIT_128(UUID128(0x8e8b0010));
static struct bt_uuid_128 data_uuid = BT_UUID_INIT_128(UUID128(0x8e8b0011));
static struct bt_uuid_128 status_uuid = BT_UUID_INIT_128(UUID128(0x8e8b0013));

BT_GATT_SERVICE_DEFINE(ota_service,
	BT_GATT_PRIMARY_SERVICE(&service_uuid.uuid),
	BT_GATT_CHARACTERISTIC(&ctrl_uuid.uuid, BT_GATT_CHRC_WRITE,
		BT_GATT_PERM_WRITE, NULL, ctrl_write, NULL),
	BT_GATT_CHARACTERISTIC(&data_uuid.uuid,
		BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
		BT_GATT_PERM_WRITE, NULL, data_write, NULL),
	BT_GATT_CHARACTERISTIC(&status_uuid.uuid,
		BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_READ, status_read, NULL, NULL),
	BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE));

static void publish_status(void)
{
	uint8_t value[6] = { state, 0, 0, 0, 0, error };
	sys_put_le32(received, &value[1]);
	(void)bt_gatt_notify(NULL, &ota_service.attrs[6], value, sizeof(value));
}

static void set_state(uint8_t value)
{
	state = value;
	error = value >= OTA_ERR_BEGIN ? value : 0;
	publish_status();
}

static void fail(uint8_t value)
{
	set_state(value);
}

static ssize_t ctrl_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  const void *buf, uint16_t len, uint16_t offset,
			  uint8_t flags)
{
	const uint8_t *p = buf;
	int rc;

	ARG_UNUSED(conn); ARG_UNUSED(attr); ARG_UNUSED(flags);
	if (offset || !len) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	switch (p[0]) {
	case OTA_OP_BEGIN: {
		const struct flash_area *slot;
		if (len != 9 || state == OTA_RECEIVING) {
			fail(OTA_ERR_BEGIN); return len;
		}
		expected_size = sys_get_le32(&p[1]);
		expected_crc = sys_get_le32(&p[5]);
		if (!expected_size ||
		    flash_area_open(FIXED_PARTITION_ID(slot1_partition), &slot)) {
			fail(OTA_ERR_BEGIN); return len;
		}
		printk("[OTA] slot 1 offset=0x%lx size=%lu\n",
		       (unsigned long)slot->fa_off, (unsigned long)slot->fa_size);
		if (expected_size > slot->fa_size) {
			flash_area_close(slot); fail(OTA_ERR_BEGIN); return len;
		}
		flash_area_close(slot);
		rc = flash_img_init(&flash_ctx);
		printk("[OTA] flash_img_init rc=%d\n", rc);
		if (rc) {
			fail(OTA_ERR_BEGIN); return len;
		}
		received = 0;
		/* crc32_ieee_update uses zlib's pre/post complement internally. */
		running_crc = 0;
		state = OTA_RECEIVING; error = 0;
		(void)k_work_cancel_delayable(&arm_timeout_work);
		/* The arm timeout is done once BEGIN arrives, so from here the stall
		 * timeout is the only thing bounding the session. */
		(void)k_work_reschedule(&stall_timeout_work, OTA_STALL_TIMEOUT);
		printk("[OTA] BEGIN size=%u crc=0x%08x\n", expected_size, expected_crc);
		publish_status();
		return len;
	}
	case OTA_OP_END:
		(void)k_work_cancel_delayable(&stall_timeout_work);
		if (len != 1 || state != OTA_RECEIVING) {
			fail(OTA_ERR_END); return len;
		}
		if (received != expected_size) { fail(OTA_ERR_SIZE); return len; }
		if (running_crc != expected_crc) { fail(OTA_ERR_CRC); return len; }
		rc = flash_img_buffered_write(&flash_ctx, NULL, 0, true);
		if (rc) {
			printk("[OTA] END flash flush failed offset=%u rc=%d\n",
			       received, rc);
			fail(OTA_ERR_END); return len;
		}
		if (flash_img_bytes_written(&flash_ctx) != received) {
			fail(OTA_ERR_END); return len;
		}
		if (boot_request_upgrade(BOOT_UPGRADE_TEST)) {
			fail(OTA_ERR_END); return len;
		}
		printk("[OTA] image verified; test upgrade requested\n");
		set_state(OTA_DONE);
		(void)k_work_reschedule(&reboot_work, OTA_REBOOT_DELAY);
		return len;
	case OTA_OP_ABORT:
		if (len != 1) return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		(void)k_work_cancel_delayable(&reboot_work);
		(void)k_work_cancel_delayable(&stall_timeout_work);
		expected_size = expected_crc = received = running_crc = 0;
		set_state(OTA_IDLE);
		return len;
	default:
		fail(OTA_ERR_SEQ); return len;
	}
}

static ssize_t data_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  const void *buf, uint16_t len, uint16_t offset,
			  uint8_t flags)
{
	int rc;
	ARG_UNUSED(conn); ARG_UNUSED(attr); ARG_UNUSED(flags);
	if (offset || state != OTA_RECEIVING || !len) {
		fail(OTA_ERR_SEQ); return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}
	if (received > expected_size || len > expected_size - received) {
		fail(OTA_ERR_SIZE); return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	/* Synchronous: the write response cannot precede this flash operation. */
	rc = flash_img_buffered_write(&flash_ctx, buf, len, false);
	if (rc) {
		printk("[OTA] flash write failed offset=%u len=%u rc=%d\n",
		       received, len, rc);
		fail(OTA_ERR_WRITE); return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
	}
	running_crc = crc32_ieee_update(running_crc, buf, len);
	received += len;
	(void)k_work_reschedule(&stall_timeout_work, OTA_STALL_TIMEOUT);
	return len;
}

static ssize_t status_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			   void *buf, uint16_t len, uint16_t offset)
{
	uint8_t value[6] = { state, 0, 0, 0, 0, error };
	sys_put_le32(received, &value[1]);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(value));
}

static void arm_timeout_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	if (active_conn && state != OTA_RECEIVING && state != OTA_DONE) {
		printk("[OTA] connection not armed; disconnecting\n");
		(void)bt_conn_disconnect(active_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
}

static void stall_timeout_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	if (state != OTA_RECEIVING) {
		return;
	}
	printk("[OTA] no data for %u ms; abandoning transfer\n",
	       (unsigned)HIVE_OTA_STALL_TIMEOUT_MS);
	/* Publish the error before dropping the link so a central that is merely
	 * slow, rather than gone, can still read why its session ended. Leaving
	 * OTA_RECEIVING is what releases the sensor loop; the disconnect then
	 * clears active_conn and restarts advertising. */
	fail(OTA_ERR_SEQ);
	if (active_conn) {
		(void)bt_conn_disconnect(active_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
}

static void reboot_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	printk("[OTA] rebooting into test image\n");
	sys_reboot(SYS_REBOOT_COLD);
}

static void connected(struct bt_conn *conn, uint8_t conn_err)
{
	struct bt_le_conn_param param = { .interval_min = 24, .interval_max = 40,
		.latency = 0, .timeout = OTA_SUPERVISION_TIMEOUT };
	if (conn_err) return;
	beacon_connected();
	active_conn = bt_conn_ref(conn);
	(void)bt_conn_le_param_update(conn, &param);
	(void)k_work_reschedule(&arm_timeout_work, OTA_ARM_TIMEOUT);
	printk("[OTA] central connected; waiting for BEGIN\n");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	(void)k_work_cancel_delayable(&arm_timeout_work);
	(void)k_work_cancel_delayable(&stall_timeout_work);
	if (state == OTA_RECEIVING) { state = error = OTA_ERR_SEQ; }
	if (active_conn) { bt_conn_unref(active_conn); active_conn = NULL; }
	printk("[OTA] central disconnected (0x%02x)\n", reason);
	beacon_disconnected();
}

BT_CONN_CB_DEFINE(ota_conn_callbacks) = { .connected = connected,
	.disconnected = disconnected };

void ota_init(void)
{
	state = OTA_IDLE; error = 0;
	/* TODO: use a filter-accept-list keyed to HiveHub after HiveHub supports
	 * bonding. That stronger guardrail requires an out-of-scope relay change. */
}

bool ota_is_active(void)
{
	return active_conn || state == OTA_RECEIVING || state == OTA_DONE;
}

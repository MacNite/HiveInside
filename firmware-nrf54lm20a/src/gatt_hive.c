/* HiveInside Firmware-over-BLE, streamed directly to MCUboot's secondary slot. */
#include "gatt_hive.h"
#include "hive_config.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/byteorder.h>

#if HIVEINSIDE_OTA_ENABLED

#define OTA_OP_BEGIN 0x01U
#define OTA_OP_END   0x03U
#define OTA_OP_ABORT 0x04U

#define OTA_IDLE      0x00U
#define OTA_RECEIVING 0x01U
#define OTA_DONE      0x02U
#define OTA_ERR_BEGIN 0x10U
#define OTA_ERR_SEQ   0x11U
#define OTA_ERR_WRITE 0x12U
#define OTA_ERR_CRC   0x13U
#define OTA_ERR_SIZE  0x14U
#define OTA_ERR_END   0x15U

#define HIVE_UUID(value) BT_UUID_128_ENCODE(value, 0x7a1c, 0x4b9e, 0x9a2f, 0x1d6e0b9c1a01)
static struct bt_uuid_128 service_uuid = BT_UUID_INIT_128(HIVE_UUID(0x8e8b0001));
static struct bt_uuid_128 ctrl_uuid = BT_UUID_INIT_128(HIVE_UUID(0x8e8b0010));
static struct bt_uuid_128 data_uuid = BT_UUID_INIT_128(HIVE_UUID(0x8e8b0011));
static struct bt_uuid_128 status_uuid = BT_UUID_INIT_128(HIVE_UUID(0x8e8b0013));

static struct flash_img_context flash_ctx;
static struct bt_le_ext_adv *connectable_adv;
static uint8_t state;
static uint8_t error;
static uint32_t expected_size;
static uint32_t expected_crc;
static uint32_t received;
static uint32_t running_crc;
static uint8_t status_value[6];
extern const struct bt_gatt_service_static hive_service;

static uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
	for (size_t i = 0; i < len; ++i) {
		crc ^= data[i];
		for (int bit = 0; bit < 8; ++bit) {
			crc = (crc >> 1) ^ (0xedb88320U & (uint32_t)-(int32_t)(crc & 1U));
		}
	}
	return crc;
}

static void publish_status(void)
{
	status_value[0] = state;
	sys_put_le32(received, &status_value[1]);
	status_value[5] = error;
	(void)bt_gatt_notify(NULL, &hive_service.attrs[6], status_value, sizeof(status_value));
}

static void fail(uint8_t failure)
{
	state = failure;
	error = failure;
	publish_status();
}

static ssize_t read_status(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			void *buf, uint16_t len, uint16_t offset)
{
	ARG_UNUSED(attr);
	status_value[0] = state;
	sys_put_le32(received, &status_value[1]);
	status_value[5] = error;
	return bt_gatt_attr_read(conn, attr, buf, len, offset, status_value,
				 sizeof(status_value));
}

static void reboot_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	printk("[OTA] rebooting into test image\n");
	sys_reboot(SYS_REBOOT_COLD);
}
K_WORK_DELAYABLE_DEFINE(reboot_work, reboot_work_handler);

static ssize_t write_control(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			     const void *buf, uint16_t len, uint16_t offset,
			     uint8_t flags)
{
	ARG_UNUSED(conn); ARG_UNUSED(attr); ARG_UNUSED(flags);
	const uint8_t *p = buf;
	if (offset != 0U || len == 0U) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	switch (p[0]) {
	case OTA_OP_BEGIN:
		if (len != 9U || state == OTA_RECEIVING) {
			fail(OTA_ERR_BEGIN);
			break;
		}
		expected_size = get_le32(&p[1]);
		expected_crc = get_le32(&p[5]);
		if (expected_size == 0U || expected_size > FIXED_PARTITION_SIZE(slot1_partition)) {
			fail(OTA_ERR_BEGIN);
			break;
		}
		if (flash_img_init(&flash_ctx) != 0) {
			fail(OTA_ERR_BEGIN);
			break;
		}
		received = 0U;
		running_crc = 0xffffffffU;
		error = 0U;
		state = OTA_RECEIVING;
		printk("[OTA] BEGIN size=%u crc=0x%08x\n", expected_size, expected_crc);
		publish_status();
		break;
	case OTA_OP_END:
		if (len != 1U || state != OTA_RECEIVING) {
			fail(OTA_ERR_END);
			break;
		}
		if (received != expected_size) {
			fail(OTA_ERR_SIZE);
			break;
		}
		if ((running_crc ^ 0xffffffffU) != expected_crc) {
			fail(OTA_ERR_CRC);
			break;
		}
		if (boot_request_upgrade(BOOT_UPGRADE_TEST) != 0) {
			fail(OTA_ERR_END);
			break;
		}
		state = OTA_DONE;
		error = 0U;
		publish_status();
		printk("[OTA] END verified; reboot scheduled\n");
		(void)k_work_reschedule(&reboot_work, K_MSEC(1500));
		break;
	case OTA_OP_ABORT:
		state = OTA_IDLE;
		error = 0U;
		received = 0U;
		publish_status();
		printk("[OTA] ABORT\n");
		break;
	default:
		fail(OTA_ERR_SEQ);
		break;
	}
	return len;
}

static ssize_t write_data(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  const void *buf, uint16_t len, uint16_t offset,
			  uint8_t flags)
{
	ARG_UNUSED(conn); ARG_UNUSED(attr); ARG_UNUSED(flags);
	if (offset != 0U || state != OTA_RECEIVING) {
		fail(OTA_ERR_SEQ);
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}
	if (len == 0U) {
		return 0;
	}
	if (received > expected_size || len > expected_size - received) {
		fail(OTA_ERR_SIZE);
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	if (flash_img_buffered_write(&flash_ctx, buf, len,
				      received + len == expected_size) != 0) {
		fail(OTA_ERR_WRITE);
		return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
	}
	running_crc = crc32_update(running_crc, buf, len);
	received += len;
	return len;
}

BT_GATT_SERVICE_DEFINE(hive_service,
	BT_GATT_PRIMARY_SERVICE(&service_uuid),
	BT_GATT_CHARACTERISTIC(&ctrl_uuid.uuid, BT_GATT_CHRC_WRITE,
		BT_GATT_PERM_WRITE, NULL, write_control, NULL),
	BT_GATT_CHARACTERISTIC(&data_uuid.uuid, BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
		BT_GATT_PERM_WRITE, NULL, write_data, NULL),
	BT_GATT_CHARACTERISTIC(&status_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_READ, read_status, NULL, status_value),
	BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE));

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn); ARG_UNUSED(reason);
	if (state == OTA_RECEIVING) {
		fail(OTA_ERR_SEQ);
	}
	if (connectable_adv != NULL) {
		(void)bt_le_ext_adv_start(connectable_adv, BT_LE_EXT_ADV_START_DEFAULT);
	}
}
BT_CONN_CB_DEFINE(conn_callbacks) = { .disconnected = disconnected };

int gatt_hive_init(void)
{
	const struct bt_data ad[] = {
		BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
		BT_DATA_BYTES(BT_DATA_UUID128_ALL, HIVE_UUID(0x8e8b0001)),
	};
	struct bt_le_adv_param param = BT_LE_ADV_PARAM_INIT(
		BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_USE_IDENTITY,
		BT_GAP_ADV_FAST_INT_MIN_2, BT_GAP_ADV_FAST_INT_MAX_2, NULL);
	int err = bt_le_ext_adv_create(&param, NULL, &connectable_adv);
	if (err == 0) err = bt_le_ext_adv_set_data(connectable_adv, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err == 0) err = bt_le_ext_adv_start(connectable_adv, BT_LE_EXT_ADV_START_DEFAULT);
	printk("[OTA] connectable GATT advertising %s (%d)\n", err ? "failed" : "ready", err);
	return err;
}

bool gatt_hive_ota_active(void) { return state == OTA_RECEIVING || state == OTA_DONE; }

#else
int gatt_hive_init(void) { return 0; }
bool gatt_hive_ota_active(void) { return false; }
#endif

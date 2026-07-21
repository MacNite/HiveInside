/* gatt_hive.c — the HiveInside custom GATT service.
 *
 * The beacon (beacon.c) is the data path; this service exists for the two
 * things a broadcast cannot do:
 *
 *  1. Version/board discovery. A beacon frame has no room for firmware
 *     metadata, so HiveHub makes ONE cached GATT connection per node per
 *     session and reads the version characteristic, which answers with
 *     {"fw":"...","board":"nrf54lm20a"}. The UUID is the same one the
 *     ESP32-C6 prototype used for its measurement JSON — HiveHub's reader
 *     treats every measurement field as optional, so a version-only blob
 *     satisfies both code paths (see HiveHub ble_sensor.cpp,
 *     discoverHiVersion / gattReadHiveInside).
 *
 *  2. Firmware-over-BLE (OTA) — PLACEHOLDER ONLY. The characteristics
 *     exist with the ecosystem UUIDs and framing so the service shape is
 *     final, but MCUboot/DFU is not integrated yet: BEGIN is rejected at
 *     the ATT level, which makes HiveHub's relay fail fast ("OTA BEGIN
 *     write failed") *before* it downloads and streams a full image, and
 *     the status characteristic reports OTA_ERR_BEGIN for status-pollers.
 *     A future implementation keeps these UUIDs/opcodes and swaps the
 *     handlers for real MCUboot slot writes (see docs/ota-over-ble.md).
 *
 * UUIDs must stay in sync with HiveHub firmware/src/ble_sensor.cpp
 * (HIVEINSIDE_GATT_* / HI_OTA_*) and the ESP32-C6 prototype's ble_link.cpp.
 */
#include "hive_config.h"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>

/* 8e8bXXXX-7a1c-4b9e-9a2f-1d6e0b9c1a01 */
#define HIVE_UUID(w)                                                           \
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x8e8b0000u | (w), 0x7a1c, 0x4b9e, \
					    0x9a2f, 0x1d6e0b9c1a01ULL))

/* Non-const: BT_GATT_PRIMARY_SERVICE stores it through a void* user_data. */
static struct bt_uuid_128 uuid_svc              = HIVE_UUID(0x0001);
static const struct bt_uuid_128 uuid_version    = HIVE_UUID(0x0002);
static const struct bt_uuid_128 uuid_ota_ctrl   = HIVE_UUID(0x0010);
static const struct bt_uuid_128 uuid_ota_data   = HIVE_UUID(0x0011);
static const struct bt_uuid_128 uuid_ota_status = HIVE_UUID(0x0013);

/* Control opcodes (first byte of a CTRL write) — ecosystem contract. */
#define OTA_OP_BEGIN 0x01 /* + size(4 LE) + crc32(4 LE) */
#define OTA_OP_END   0x03
#define OTA_OP_ABORT 0x04

/* Status state byte. >= 0x10 is a terminal error. */
#define OTA_IDLE      0x00
#define OTA_ERR_BEGIN 0x10

/* Status value: state(1) + received(4 LE) + error(1). */
static uint8_t ota_status[6];

static const char version_json[] =
	"{\"fw\":\"" HIVEINSIDE_FW_VERSION "\",\"board\":\"" HIVEINSIDE_BOARD
	"\"}";

static ssize_t read_version(struct bt_conn *conn,
			    const struct bt_gatt_attr *attr, void *buf,
			    uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, version_json,
				 sizeof(version_json) - 1);
}

static ssize_t read_ota_status(struct bt_conn *conn,
			       const struct bt_gatt_attr *attr, void *buf,
			       uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, ota_status,
				 sizeof(ota_status));
}

static void ota_status_ccc_changed(const struct bt_gatt_attr *attr,
				   uint16_t value)
{
	ARG_UNUSED(attr);
	ARG_UNUSED(value);
}

static ssize_t write_ota_ctrl(struct bt_conn *conn,
			      const struct bt_gatt_attr *attr, const void *buf,
			      uint16_t len, uint16_t offset, uint8_t flags);
static ssize_t write_ota_data(struct bt_conn *conn,
			      const struct bt_gatt_attr *attr, const void *buf,
			      uint16_t len, uint16_t offset, uint8_t flags);

BT_GATT_SERVICE_DEFINE(hive_svc,
	BT_GATT_PRIMARY_SERVICE(&uuid_svc),
	BT_GATT_CHARACTERISTIC(&uuid_version.uuid, BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ, read_version, NULL, NULL),
	BT_GATT_CHARACTERISTIC(&uuid_ota_ctrl.uuid, BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE, NULL, write_ota_ctrl, NULL),
	BT_GATT_CHARACTERISTIC(&uuid_ota_data.uuid,
			       BT_GATT_CHRC_WRITE |
				       BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, write_ota_data, NULL),
	BT_GATT_CHARACTERISTIC(&uuid_ota_status.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, read_ota_status, NULL, NULL),
	BT_GATT_CCC(ota_status_ccc_changed,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

/* Index of the OTA status *value* attribute inside hive_svc.attrs[]:
 * [0] service, [1]+[2] version, [3]+[4] ctrl, [5]+[6] data,
 * [7]+[8] status (decl + value), [9] CCC. */
#define OTA_STATUS_ATTR (&hive_svc.attrs[8])

static void ota_publish_status(uint8_t state, uint8_t err)
{
	ota_status[0] = state;
	sys_put_le32(0, &ota_status[1]);
	ota_status[5] = err;
	/* Best-effort: no-op unless a subscribed central is connected. */
	bt_gatt_notify(NULL, OTA_STATUS_ATTR, ota_status, sizeof(ota_status));
}

static ssize_t write_ota_ctrl(struct bt_conn *conn,
			      const struct bt_gatt_attr *attr, const void *buf,
			      uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(offset);
	ARG_UNUSED(flags);

	const uint8_t *p = buf;

	if (len < 1) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	switch (p[0]) {
	case OTA_OP_BEGIN:
		/* Placeholder: refuse the transfer up front so the relay
		 * fails before streaming an image (see file header). */
		printk("[OTA] BEGIN rejected: firmware update not implemented on this target yet\n");
		ota_publish_status(OTA_ERR_BEGIN, OTA_ERR_BEGIN);
		return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
	case OTA_OP_ABORT:
		ota_publish_status(OTA_IDLE, 0);
		return len;
	case OTA_OP_END:
	default:
		/* No transfer can be in progress — nothing to do. */
		return len;
	}
}

static ssize_t write_ota_data(struct bt_conn *conn,
			      const struct bt_gatt_attr *attr, const void *buf,
			      uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(buf);
	ARG_UNUSED(len);
	ARG_UNUSED(offset);
	ARG_UNUSED(flags);

	/* Placeholder: no transfer is ever active, so payload writes are
	 * protocol errors. */
	return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
}

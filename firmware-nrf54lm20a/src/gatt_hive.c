/*
 * gatt_hive.c — connectable HiveInside GATT service + Firmware-over-BLE (OTA).
 *
 * A HiveHub relay opens a GATT session and streams a new firmware image into
 * the OTA characteristics; each DATA write is flashed straight into MCUboot's
 * secondary slot (image-1) via Zephyr's DFU image API, with a running CRC-32.
 * Nothing is committed until END verifies size + CRC, so a dropped or corrupted
 * transfer always leaves the device on its old image. On a good END the image
 * is marked for upgrade (boot_request_upgrade) and the node reboots; MCUboot
 * swaps image-1 into the active slot on the next boot.
 *
 * Wire contract — MUST match docs/ota-over-ble.md exactly (these bytes are also
 * decoded by HiveHub and must not drift):
 *   service  8e8b0001-7a1c-4b9e-9a2f-1d6e0b9c1a01
 *   ctrl     8e8b0010-…  Write        first byte = opcode
 *   data     8e8b0011-…  Write/WriteNR raw firmware bytes, in order
 *   status   8e8b0013-…  Read/Notify  state(1) + received(4 LE) + error(1)
 *
 * Backed by MCUboot dual-slot DFU via Zephyr's DFU image API — not an
 * in-place flash-writer. The board DTS already provides the
 * mcuboot/slot0/slot1/storage partitions in RRAM, so this module only streams
 * into the secondary slot and requests the swap.
 *
 * ── The connectable-vs-connectionless split ──
 * beacon.c owns a NON-connectable, scannable legacy advertisement carrying the
 * measurement; this module owns a SECOND, connectable legacy advertising set
 * for OTA. Both are legacy PDUs (the extended-advertising API is used only to
 * host two sets at once), so HiveHub's passive scanner keeps decoding the
 * measurement beacon unchanged while a relay can still connect for OTA.
 *
 * Compiled out when HIVEINSIDE_OTA_ENABLED=0 (see the stubs at the bottom).
 */
#include "gatt_hive.h"
#include "hive_config.h"

#if HIVEINSIDE_OTA_ENABLED

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/bluetooth/att.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

/* ── 128-bit UUIDs (all share the HiveInside base) ─────────────────────────
 * BT_UUID_128_ENCODE emits the 16 bytes in little-endian wire order. */
#define HIVE_UUID(w32) BT_UUID_128_ENCODE((w32), 0x7a1c, 0x4b9e, 0x9a2f, 0x1d6e0b9c1a01)

static const struct bt_uuid_128 hive_svc_uuid   = BT_UUID_INIT_128(HIVE_UUID(0x8e8b0001));
static const struct bt_uuid_128 ota_ctrl_uuid   = BT_UUID_INIT_128(HIVE_UUID(0x8e8b0010));
static const struct bt_uuid_128 ota_data_uuid   = BT_UUID_INIT_128(HIVE_UUID(0x8e8b0011));
static const struct bt_uuid_128 ota_status_uuid = BT_UUID_INIT_128(HIVE_UUID(0x8e8b0013));

/* ── Control opcodes (first byte of a CTRL write) — must match the contract ── */
#define OTA_OP_BEGIN 0x01U /* + size(4 LE) + crc32(4 LE) */
#define OTA_OP_END   0x03U /* verify size + CRC, request upgrade, reboot */
#define OTA_OP_ABORT 0x04U /* cancel, stay on current image */

/* ── Status state byte. 0x10.. is a terminal error, matching the contract. ── */
#define OTA_IDLE      0x00U
#define OTA_RECEIVING 0x01U
#define OTA_DONE      0x02U
#define OTA_ERR_BEGIN 0x10U
#define OTA_ERR_SEQ   0x11U
#define OTA_ERR_WRITE 0x12U
#define OTA_ERR_CRC   0x13U
#define OTA_ERR_SIZE  0x14U
#define OTA_ERR_END   0x15U

/* Give the central time to read the terminal "done" status before we reboot. */
#define OTA_REBOOT_DELAY_MS 1500

/* ── OTA transfer state (touched from the BT RX thread in the GATT write
 * callbacks, which the stack serialises; the reboot deadline is also read from
 * the main thread in gatt_hive_poll(), hence volatile). ── */
static struct flash_img_context s_img;         /* streams into slot1 (image-1) */
static volatile uint8_t  s_state = OTA_IDLE;
static volatile uint8_t  s_err;
static uint32_t          s_size;               /* expected image size from BEGIN */
static volatile uint32_t s_recv;               /* bytes flashed so far */
static uint32_t          s_expected_crc;       /* end-to-end CRC from BEGIN */
static uint32_t          s_running_crc = 0xFFFFFFFFUL;
static volatile int64_t  s_reboot_at;          /* uptime deadline; 0 = none */

/*
 * CRC-32 (IEEE 802.3, reflected poly 0xEDB88320, init/final 0xFFFFFFFF) —
 * identical to zlib.crc32 on the backend and the value HiveHub relays, so an
 * image verified here matches byte-for-byte end to end.
 */
static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		crc ^= p[i];
		for (int k = 0; k < 8; k++) {
			uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));

			crc = (crc >> 1) ^ (0xEDB88320UL & mask);
		}
	}
	return crc;
}

/* ── GATT service ──────────────────────────────────────────────────────────
 * Attribute layout used by OTA_STATUS_ATTR below:
 *   [0] primary service
 *   [1] OTA control declaration   [2] OTA control value
 *   [3] OTA data    declaration   [4] OTA data value
 *   [5] OTA status  declaration   [6] OTA status value   <- notify target
 *   [7] OTA status  CCC
 */
static ssize_t ota_ctrl_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			      const void *buf, uint16_t len, uint16_t offset, uint8_t flags);
static ssize_t ota_data_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			      const void *buf, uint16_t len, uint16_t offset, uint8_t flags);
static ssize_t ota_status_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			       void *buf, uint16_t len, uint16_t offset);

BT_GATT_SERVICE_DEFINE(hive_svc,
	BT_GATT_PRIMARY_SERVICE(&hive_svc_uuid),
	BT_GATT_CHARACTERISTIC(&ota_ctrl_uuid.uuid, BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE, NULL, ota_ctrl_write, NULL),
	BT_GATT_CHARACTERISTIC(&ota_data_uuid.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, ota_data_write, NULL),
	BT_GATT_CHARACTERISTIC(&ota_status_uuid.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, ota_status_read, NULL, NULL),
	BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

#define OTA_STATUS_ATTR (&hive_svc.attrs[6])

/* Serialise status into the 6-byte wire layout: state(1) recv(4 LE) error(1). */
static void ota_status_bytes(uint8_t out[6])
{
	uint32_t r = s_recv;

	out[0] = s_state;
	out[1] = (uint8_t)r;
	out[2] = (uint8_t)(r >> 8);
	out[3] = (uint8_t)(r >> 16);
	out[4] = (uint8_t)(r >> 24);
	out[5] = s_err;
}

static void ota_notify_status(void)
{
	uint8_t b[6];

	ota_status_bytes(b);
	/* NULL conn: notify every subscribed central; -ENOTCONN when none. */
	(void)bt_gatt_notify(NULL, OTA_STATUS_ATTR, b, sizeof(b));
}

static ssize_t ota_status_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			       void *buf, uint16_t len, uint16_t offset)
{
	uint8_t b[6];

	ota_status_bytes(b);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, b, sizeof(b));
}

/* Enter a terminal error state, abandoning any partially written slot. */
static void ota_fail(uint8_t state)
{
	s_state = state;
	s_err = state;
	s_reboot_at = 0;
	ota_notify_status();
}

static void ota_reset(uint8_t state)
{
	s_state = state;
	s_err = 0;
	s_recv = 0;
	s_reboot_at = 0;
	ota_notify_status();
}

static ssize_t ota_ctrl_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			      const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	const uint8_t *p = buf;

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len < 1U) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	switch (p[0]) {
	case OTA_OP_BEGIN: {
		if (len < 9U) {
			ota_fail(OTA_ERR_BEGIN);
			break;
		}
		uint32_t size = (uint32_t)p[1] | ((uint32_t)p[2] << 8) |
				((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 24);
		uint32_t crc = (uint32_t)p[5] | ((uint32_t)p[6] << 8) |
			       ((uint32_t)p[7] << 16) | ((uint32_t)p[8] << 24);

		/* Point the DFU stream at MCUboot's secondary slot (image-1) and
		 * erase-as-you-write; nothing is committed until END. */
		int rc = flash_img_init(&s_img);

		if (rc != 0) {
			printk("[OTA] flash_img_init failed (%d)\n", rc);
			ota_fail(OTA_ERR_BEGIN);
			break;
		}
		s_size = size;
		s_expected_crc = crc;
		s_recv = 0;
		s_running_crc = 0xFFFFFFFFUL;
		s_err = 0;
		s_reboot_at = 0;
		s_state = OTA_RECEIVING;
		printk("[OTA] BEGIN size=%u crc=0x%08x\n", (unsigned)size, (unsigned)crc);
		ota_notify_status();
		break;
	}
	case OTA_OP_END: {
		if (s_state != OTA_RECEIVING) {
			break;
		}
		if (s_recv != s_size) {
			printk("[OTA] size mismatch recv=%u expected=%u\n",
			       (unsigned)s_recv, (unsigned)s_size);
			ota_fail(OTA_ERR_SIZE);
			break;
		}
		uint32_t final_crc = s_running_crc ^ 0xFFFFFFFFUL;

		if (s_expected_crc != 0U && final_crc != s_expected_crc) {
			printk("[OTA] CRC mismatch got=0x%08x expected=0x%08x\n",
			       (unsigned)final_crc, (unsigned)s_expected_crc);
			ota_fail(OTA_ERR_CRC);
			break;
		}
		/* Flush the last partial flash-write-block still buffered by the
		 * DFU image writer, then hand the slot to MCUboot. Pass a non-NULL
		 * pointer with len 0 so no writer implementation can dereference a
		 * NULL data pointer while flushing. */
		uint8_t flush_marker = 0;
		int rc = flash_img_buffered_write(&s_img, &flush_marker, 0, true);

		if (rc != 0) {
			printk("[OTA] flush failed (%d)\n", rc);
			ota_fail(OTA_ERR_END);
			break;
		}
		rc = boot_request_upgrade(BOOT_UPGRADE_PERMANENT);
		if (rc != 0) {
			printk("[OTA] boot_request_upgrade failed (%d)\n", rc);
			ota_fail(OTA_ERR_END);
			break;
		}
		printk("[OTA] END ok — image staged, rebooting into MCUboot swap\n");
		s_state = OTA_DONE;
		s_err = 0;
		ota_notify_status();
		/* Defer the reset so the central can read DONE; poll() reboots. */
		s_reboot_at = k_uptime_get() + OTA_REBOOT_DELAY_MS;
		break;
	}
	case OTA_OP_ABORT:
		printk("[OTA] ABORT\n");
		ota_reset(OTA_IDLE);
		break;
	default:
		break;
	}

	return len;
}

static ssize_t ota_data_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			      const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	const uint8_t *p = buf;

	if (s_state != OTA_RECEIVING) {
		ota_fail(OTA_ERR_SEQ);
		return len;
	}
	if (len == 0U) {
		return len;
	}
	/* Overrun guard: never write past the image the BEGIN announced. */
	if ((uint64_t)s_recv + len > s_size) {
		printk("[OTA] overrun recv=%u+%u > size=%u\n",
		       (unsigned)s_recv, (unsigned)len, (unsigned)s_size);
		ota_fail(OTA_ERR_SIZE);
		return len;
	}
	/* Stream straight into slot1; the write returns (and, for a plain Write,
	 * the ATT response follows) only after the chunk is flashed — the flow
	 * control the wire contract relies on. */
	int rc = flash_img_buffered_write(&s_img, p, len, false);

	if (rc != 0) {
		printk("[OTA] write failed at %u (%d)\n", (unsigned)s_recv, rc);
		ota_fail(OTA_ERR_WRITE);
		return len;
	}
	s_running_crc = crc32_update(s_running_crc, p, len);
	s_recv += len;
	return len;
}

/* ── Connectable advertising set (independent of the measurement beacon) ──── */
static struct bt_le_ext_adv *conn_adv;

static const struct bt_data conn_ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, HIVE_UUID(0x8e8b0001)),
};

static const struct bt_data conn_sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, HIVEINSIDE_DEVICE_NAME,
		sizeof(HIVEINSIDE_DEVICE_NAME) - 1U),
};

/* Legacy (no BT_LE_ADV_OPT_EXT_ADV) connectable ADV_IND from the stable
 * identity address — the same address the measurement beacon uses, so a relay
 * connects to the node it scanned. */
static const struct bt_le_adv_param conn_param = BT_LE_ADV_PARAM_INIT(
	BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_USE_IDENTITY,
	BT_GAP_ADV_FAST_INT_MIN_2, BT_GAP_ADV_FAST_INT_MAX_2, NULL);

static void conn_adv_start(void)
{
	int err = bt_le_ext_adv_start(conn_adv, BT_LE_EXT_ADV_START_DEFAULT);

	if (err != 0 && err != -EALREADY) {
		printk("[OTA] connectable adv start failed (%d)\n", err);
	}
}

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	if (err != 0) {
		conn_adv_start(); /* connection attempt failed; keep advertising */
		return;
	}
	printk("[OTA] central connected (OTA session open)\n");
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	printk("[OTA] central disconnected (reason 0x%02x)\n", reason);
	/* A transfer interrupted mid-flight leaves slot1 without a valid MCUboot
	 * trailer, so the old image keeps running; just reset OTA state. */
	if (s_state == OTA_RECEIVING) {
		ota_reset(OTA_IDLE);
	}
	/* A connectable set stops when a connection is established; restart it so
	 * the relay can reconnect (e.g. to retry a transfer). */
	conn_adv_start();
}

BT_CONN_CB_DEFINE(hive_conn_cb) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
};

int gatt_hive_init(void)
{
	/* The GATT service is registered at boot by BT_GATT_SERVICE_DEFINE; here
	 * we only bring up the connectable advertising set. Bluetooth must
	 * already be enabled (beacon_init() calls bt_enable()). */
	int err = bt_le_ext_adv_create(&conn_param, NULL, &conn_adv);

	if (err != 0) {
		printk("[OTA] connectable adv create failed (%d)\n", err);
		return err;
	}
	err = bt_le_ext_adv_set_data(conn_adv, conn_ad, ARRAY_SIZE(conn_ad),
				     conn_sd, ARRAY_SIZE(conn_sd));
	if (err != 0) {
		printk("[OTA] connectable adv set_data failed (%d)\n", err);
		return err;
	}
	conn_adv_start();
	printk("[OTA] ready; connectable GATT OTA service advertising\n");
	return 0;
}

bool gatt_hive_ota_active(void)
{
	return s_state == OTA_RECEIVING ||
	       (s_state == OTA_DONE && s_reboot_at != 0);
}

void gatt_hive_poll(void)
{
	if (s_state == OTA_DONE && s_reboot_at != 0 &&
	    k_uptime_get() >= s_reboot_at) {
		printk("[OTA] rebooting into new image\n");
		s_reboot_at = 0;
		sys_reboot(SYS_REBOOT_WARM);
	}
}

#else /* !HIVEINSIDE_OTA_ENABLED — OTA compiled out */

int gatt_hive_init(void)
{
	return 0;
}

bool gatt_hive_ota_active(void)
{
	return false;
}

void gatt_hive_poll(void)
{
}

#endif /* HIVEINSIDE_OTA_ENABLED */

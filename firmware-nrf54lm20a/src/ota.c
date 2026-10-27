/* MCUboot firmware-over-BLE target. ATT responses are returned only after each
 * flash write completes, providing the relay's stream flow control. */
#include "ota.h"
#include "link.h"
#include "hive_config.h"

#include <errno.h>
#include <stdint.h>

#include <zephyr/bluetooth/att.h>
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
#include <zephyr/sys/util.h>

#define OTA_STALL_TIMEOUT K_MSEC(HIVE_OTA_STALL_TIMEOUT_MS)
#define OTA_SESSION_TIMEOUT K_MSEC(HIVE_OTA_SESSION_TIMEOUT_MS)
#define OTA_REBOOT_DELAY K_MSEC(1500)
#define OTA_SUPERVISION_TIMEOUT 200U /* 2 seconds, units of 10 ms */

enum { OTA_OP_BEGIN = 0x01, OTA_OP_END = 0x03, OTA_OP_ABORT = 0x04 };
enum {
	OTA_IDLE = 0x00, OTA_RECEIVING = 0x01, OTA_DONE = 0x02,
	OTA_ERR_BEGIN = 0x10, OTA_ERR_SEQ = 0x11, OTA_ERR_WRITE = 0x12,
	OTA_ERR_CRC = 0x13, OTA_ERR_SIZE = 0x14, OTA_ERR_END = 0x15,
};

static struct flash_img_context flash_ctx;
static uint32_t expected_size, expected_crc, received, running_crc;
static uint8_t state, error;
/* Errno of the last failed flash operation, reported as the seventh STATUS
 * byte. The low-power deployment image has no console at all (low-power.conf
 * turns CONFIG_PRINTK off), so without this the only thing a field failure can
 * say is "flash write failed" — which does not distinguish a slot that is too
 * small (-EINVAL) from a radio timeslot that never arrived (-ETIMEDOUT). Zero
 * means "no error recorded". */
static int8_t op_errno;

/* Zephyr errnos are small negatives, but clamp rather than trust that: a value
 * that wrapped in the cast would read as a completely different fault. */
static void record_errno(int rc)
{
	op_errno = (int8_t)CLAMP(rc, INT8_MIN, INT8_MAX);
}

/* GATT writes arrive on the Bluetooth RX thread, the timeouts on the system
 * work queue and link events from either; every state transition holds this. */
K_MUTEX_DEFINE(ota_lock);

/* Failed transfers since the last successful END, and when the cooldown that
 * HIVE_OTA_MAX_FAILED_SESSIONS of them start ends. */
static uint8_t failed_sessions;
static int64_t cooldown_until_ms;

static void reboot_handler(struct k_work *work);
static void stall_timeout_handler(struct k_work *work);
static void session_timeout_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(reboot_work, reboot_handler);
K_WORK_DELAYABLE_DEFINE(stall_timeout_work, stall_timeout_handler);
K_WORK_DELAYABLE_DEFINE(session_timeout_work, session_timeout_handler);


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

/* state(1) + received(4 LE) + error(1) + errno(1). The first six bytes are the
 * original layout and keep their meaning; relays that only know about those
 * read a short value and are unaffected. */
#define OTA_STATUS_LEN 7

static void fill_status(uint8_t value[OTA_STATUS_LEN])
{
	value[0] = state;
	sys_put_le32(received, &value[1]);
	value[5] = error;
	value[6] = (uint8_t)op_errno;
}

static void publish_status(void)
{
	uint8_t value[OTA_STATUS_LEN];

	fill_status(value);
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

/* Report a refused request without disturbing the session it arrived in: a
 * duplicate BEGIN must not kill the transfer in progress, and nothing may move
 * the node out of DONE once the upgrade has been requested. */
static void refuse(uint8_t value)
{
	error = value;
	publish_status();
}

/* The single exit for a transfer that holds the link: stop its timers, forget
 * its progress, report why it ended and hand the link back. Releasing the claim
 * re-arms the link's arm timeout, so the central keeps a short window to read
 * STATUS or BEGIN again, and is disconnected if it does neither. */
static void end_session(uint8_t value)
{
	(void)k_work_cancel_delayable(&stall_timeout_work);
	(void)k_work_cancel_delayable(&session_timeout_work);
	expected_size = expected_crc = received = running_crc = 0;
	if (value != OTA_IDLE && ++failed_sessions >= HIVE_OTA_MAX_FAILED_SESSIONS) {
		cooldown_until_ms = k_uptime_get() + HIVE_OTA_FAILURE_COOLDOWN_MS;
		printk("[OTA] %u failed sessions; refusing BEGIN for %u ms\n",
		       (unsigned)failed_sessions,
		       (unsigned)HIVE_OTA_FAILURE_COOLDOWN_MS);
	}
	set_state(value);
	link_release(LINK_OWNER_OTA);
}

static bool cooling_down(void)
{
	if (failed_sessions < HIVE_OTA_MAX_FAILED_SESSIONS) {
		return false;
	}
	if (k_uptime_get() < cooldown_until_ms) {
		return true;
	}
	failed_sessions = 0;
	return false;
}

static ssize_t ctrl_begin(struct bt_conn *conn, const uint8_t *p, uint16_t len)
{
	const struct flash_area *slot;
	int rc;

	if (state == OTA_RECEIVING || state == OTA_DONE) {
		refuse(OTA_ERR_BEGIN);
		return len;
	}
	if (len != 9) {
		fail(OTA_ERR_BEGIN);
		return len;
	}
	op_errno = 0;
	if (cooling_down()) {
		record_errno(-EAGAIN);
		fail(OTA_ERR_BEGIN);
		return len;
	}
	expected_size = sys_get_le32(&p[1]);
	expected_crc = sys_get_le32(&p[5]);
	if (!expected_size ||
	    flash_area_open(FIXED_PARTITION_ID(slot1_partition), &slot)) {
		fail(OTA_ERR_BEGIN);
		return len;
	}
	printk("[OTA] slot 1 offset=0x%lx size=%lu\n",
	       (unsigned long)slot->fa_off, (unsigned long)slot->fa_size);
	if (expected_size > slot->fa_size) {
		flash_area_close(slot);
		fail(OTA_ERR_BEGIN);
		return len;
	}
	flash_area_close(slot);
	rc = flash_img_init(&flash_ctx);
	printk("[OTA] flash_img_init rc=%d\n", rc);
	if (rc) {
		record_errno(rc);
		fail(OTA_ERR_BEGIN);
		return len;
	}
	if (!link_claim(LINK_OWNER_OTA)) {
		fail(OTA_ERR_BEGIN);
		return len;
	}
	/* Leaving OTA at the generic 30--50 ms link interval made every
	 * write-with-response wait needlessly: its ATT response is the flash
	 * flow-control boundary. Match audio's faster link setup after claim. */
	struct bt_le_conn_param cp = {
		.interval_min = HIVE_AUDIO_CONN_INTERVAL_UNITS,
		.interval_max = HIVE_AUDIO_CONN_INTERVAL_UNITS,
		.latency = 0,
		.timeout = OTA_SUPERVISION_TIMEOUT,
	};
	(void)bt_conn_le_param_update(conn, &cp);
	(void)bt_conn_le_data_len_update(conn, BT_LE_DATA_LEN_PARAM_MAX);
	(void)bt_conn_le_phy_update(conn, BT_CONN_LE_PHY_PARAM_2M);
	received = 0;
	/* crc32_ieee_update uses zlib's pre/post complement internally. */
	running_crc = 0;
	state = OTA_RECEIVING; error = 0;
	/* The arm timeout is done once BEGIN arrives. From here the stall
	 * timeout bounds silence and the session timeout bounds the whole
	 * transfer, however steadily it trickles in. */
	(void)k_work_reschedule(&stall_timeout_work, OTA_STALL_TIMEOUT);
	(void)k_work_reschedule(&session_timeout_work, OTA_SESSION_TIMEOUT);
	printk("[OTA] BEGIN size=%u crc=0x%08x\n", expected_size, expected_crc);
	publish_status();
	return len;
}

static int sem_ver_cmp(const struct mcuboot_img_sem_ver *a,
		       const struct mcuboot_img_sem_ver *b)
{
	if (a->major != b->major) {
		return a->major < b->major ? -1 : 1;
	}
	if (a->minor != b->minor) {
		return a->minor < b->minor ? -1 : 1;
	}
	if (a->revision != b->revision) {
		return a->revision < b->revision ? -1 : 1;
	}
	return 0;
}

/* Downgrade policy: refuse an image whose signed header version is older than
 * the running image's. The version in the header is covered by the image
 * signature, so a validly signed old release cannot be re-badged; a forged
 * header only reaches MCUboot, which rejects its signature. Equal versions are
 * accepted so a node can switch between the bring-up and low-power variants of
 * one release. MCUboot's own test-swap rollback is unaffected: it reverts
 * without going through this path. */
static int check_image_version(void)
{
	struct mcuboot_img_header incoming, running;
	int rc;

	rc = boot_read_bank_header(FIXED_PARTITION_ID(slot1_partition), &incoming,
				   sizeof(incoming));
	if (rc != 0 || incoming.mcuboot_version != 1) {
		printk("[OTA] uploaded image has no readable MCUboot header (%d)\n", rc);
		return -EBADMSG;
	}
	rc = boot_read_bank_header(FIXED_PARTITION_ID(slot0_partition), &running,
				   sizeof(running));
	if (rc != 0 || running.mcuboot_version != 1) {
		/* Not expected for an image MCUboot booted. Let the signature
		 * check decide rather than make the node un-updatable. */
		printk("[OTA] running image header unreadable (%d); skipping version check\n",
		       rc);
		return 0;
	}
	if (sem_ver_cmp(&incoming.h.v1.sem_ver, &running.h.v1.sem_ver) < 0) {
		printk("[OTA] refusing downgrade %u.%u.%u -> %u.%u.%u\n",
		       running.h.v1.sem_ver.major, running.h.v1.sem_ver.minor,
		       running.h.v1.sem_ver.revision, incoming.h.v1.sem_ver.major,
		       incoming.h.v1.sem_ver.minor, incoming.h.v1.sem_ver.revision);
		return -EPERM;
	}
	return 0;
}

static ssize_t ctrl_end(uint16_t len)
{
	int rc;

	if (state == OTA_DONE) {
		/* A repeated END changes nothing: the upgrade is already requested. */
		return len;
	}
	if (state != OTA_RECEIVING) {
		fail(OTA_ERR_END);
		return len;
	}
	if (len != 1) { end_session(OTA_ERR_END); return len; }
	if (received != expected_size) { end_session(OTA_ERR_SIZE); return len; }
	if (running_crc != expected_crc) { end_session(OTA_ERR_CRC); return len; }
	rc = flash_img_buffered_write(&flash_ctx, NULL, 0, true);
	if (rc) {
		printk("[OTA] END flash flush failed offset=%u rc=%d\n",
		       received, rc);
		record_errno(rc);
		end_session(OTA_ERR_END);
		return len;
	}
	if (flash_img_bytes_written(&flash_ctx) != received) {
		end_session(OTA_ERR_END);
		return len;
	}
	rc = check_image_version();
	if (rc) {
		record_errno(rc);
		end_session(OTA_ERR_END);
		return len;
	}
	rc = boot_request_upgrade(BOOT_UPGRADE_TEST);
	if (rc) {
		record_errno(rc);
		end_session(OTA_ERR_END);
		return len;
	}
	/* END is the commit point. The pending-upgrade marker is now in flash,
	 * so the next reset of any kind boots the new image; the node keeps the
	 * link claimed and reboots on schedule rather than pretend otherwise. */
	(void)k_work_cancel_delayable(&stall_timeout_work);
	(void)k_work_cancel_delayable(&session_timeout_work);
	failed_sessions = 0;
	printk("[OTA] image verified; test upgrade requested\n");
	set_state(OTA_DONE);
	(void)k_work_reschedule(&reboot_work, OTA_REBOOT_DELAY);
	return len;
}

static ssize_t ctrl_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  const void *buf, uint16_t len, uint16_t offset,
			  uint8_t flags)
{
	const uint8_t *p = buf;
	ssize_t result = len;

	ARG_UNUSED(attr); ARG_UNUSED(flags);
	if (offset || !len) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	k_mutex_lock(&ota_lock, K_FOREVER);
	switch (p[0]) {
	case OTA_OP_BEGIN:
		result = ctrl_begin(conn, p, len);
		break;
	case OTA_OP_END:
		result = ctrl_end(len);
		break;
	case OTA_OP_ABORT:
		if (len != 1) {
			result = BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		} else if (state == OTA_DONE) {
			/* Too late: cancelling the reboot would not withdraw the
			 * pending upgrade, only postpone it to an arbitrary later
			 * reset. Leave DONE as reported and reboot as announced. */
			refuse(OTA_ERR_SEQ);
		} else if (state == OTA_RECEIVING) {
			end_session(OTA_IDLE);
		} else {
			set_state(OTA_IDLE);
		}
		break;
	default:
		if (state == OTA_RECEIVING) {
			end_session(OTA_ERR_SEQ);
		} else if (state == OTA_DONE) {
			refuse(OTA_ERR_SEQ);
		} else {
			fail(OTA_ERR_SEQ);
		}
		break;
	}
	k_mutex_unlock(&ota_lock);
	return result;
}

static ssize_t data_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  const void *buf, uint16_t len, uint16_t offset,
			  uint8_t flags)
{
	ssize_t result = len;
	int rc;

	ARG_UNUSED(conn); ARG_UNUSED(attr); ARG_UNUSED(flags);
	k_mutex_lock(&ota_lock, K_FOREVER);
	if (state != OTA_RECEIVING) {
		/* Writes still queued behind a failed transfer must not overwrite
		 * the error that ended it. */
		if (state == OTA_IDLE) {
			fail(OTA_ERR_SEQ);
		}
		result = BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	} else if (offset || !len) {
		end_session(OTA_ERR_SEQ);
		result = BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	} else if (received > expected_size || len > expected_size - received) {
		end_session(OTA_ERR_SIZE);
		result = BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	} else {
		/* Synchronous: the write response cannot precede this flash
		 * operation. */
		rc = flash_img_buffered_write(&flash_ctx, buf, len, false);
		if (rc) {
			printk("[OTA] flash write failed offset=%u len=%u rc=%d\n",
			       received, len, rc);
			record_errno(rc);
			end_session(OTA_ERR_WRITE);
			result = BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
		} else {
			running_crc = crc32_ieee_update(running_crc, buf, len);
			received += len;
			(void)k_work_reschedule(&stall_timeout_work,
						OTA_STALL_TIMEOUT);
		}
	}
	k_mutex_unlock(&ota_lock);
	return result;
}

static ssize_t status_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			   void *buf, uint16_t len, uint16_t offset)
{
	uint8_t value[OTA_STATUS_LEN];

	fill_status(value);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(value));
}

static void abandon(const char *why, int rc)
{
	k_mutex_lock(&ota_lock, K_FOREVER);
	if (state == OTA_RECEIVING) {
		printk("[OTA] %s; abandoning transfer\n", why);
		/* Publish the error before dropping the link so a central that is
		 * merely slow, rather than gone, can still read why its session
		 * ended. */
		record_errno(rc);
		end_session(OTA_ERR_SEQ);
		link_disconnect();
	}
	k_mutex_unlock(&ota_lock);
}

static void stall_timeout_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	abandon("no DATA write within the stall timeout", -ETIMEDOUT);
}

static void session_timeout_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	abandon("transfer exceeded the session timeout", -ETIME);
}

static void reboot_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	printk("[OTA] rebooting into test image\n");
	sys_reboot(SYS_REBOOT_COLD);
}

void ota_link_connected(void)
{
	k_mutex_lock(&ota_lock, K_FOREVER);
	/* A pending reboot keeps DONE visible to whoever connects next. */
	if (state != OTA_DONE) {
		state = OTA_IDLE;
		error = 0;
		op_errno = 0;
	}
	k_mutex_unlock(&ota_lock);
}

void ota_link_disconnected(void)
{
	k_mutex_lock(&ota_lock, K_FOREVER);
	if (state == OTA_RECEIVING) {
		end_session(OTA_ERR_SEQ);
	} else {
		/* DONE keeps its claim until the reboot; drop it with the link. */
		link_release(LINK_OWNER_OTA);
	}
	k_mutex_unlock(&ota_lock);
}

void ota_init(void)
{
	state = OTA_IDLE; error = 0; op_errno = 0;
	/* TODO: use a filter-accept-list keyed to HiveHub after HiveHub supports
	 * bonding. That stronger guardrail requires an out-of-scope relay change. */
}

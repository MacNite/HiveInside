/* OTA session lifecycle against the real link.c and ota.c. */
#include "shim_test.h"

#include "link.c"
#include "ota.c"

void beacon_connected(void) { }
void beacon_disconnected(void) { }
void hive_watchdog_feed(void) { }
void audio_link_connected(void) { }
void audio_link_disconnected(void) { }

#define CTRL (&ota_service_attrs[2])
#define DATA (&ota_service_attrs[4])
#define STATUS (&ota_service_attrs[6])

enum { S_IDLE = 0x00, S_RECEIVING = 0x01, S_DONE = 0x02, E_BEGIN = 0x10,
       E_SEQ = 0x11, E_WRITE = 0x12, E_CRC = 0x13, E_SIZE = 0x14, E_END = 0x15 };

static struct bt_conn *conn;
static uint8_t image[4096];

static ssize_t write_attr(const struct bt_gatt_attr *attr, const void *buf, uint16_t len)
{
	return attr->write(conn, attr, buf, len, 0, 0);
}

static void begin(uint32_t size, uint32_t crc)
{
	uint8_t frame[9] = { 0x01 };

	sys_put_le32(size, &frame[1]);
	sys_put_le32(crc, &frame[5]);
	write_attr(CTRL, frame, sizeof(frame));
}

static ssize_t data(size_t off, size_t len)
{
	return write_attr(DATA, &image[off], (uint16_t)len);
}

static void end(void) { uint8_t op = 0x03; write_attr(CTRL, &op, 1); }
static void abort_transfer(void) { uint8_t op = 0x04; write_attr(CTRL, &op, 1); }

static uint8_t status_byte(int i)
{
	uint8_t v[OTA_STATUS_LEN];

	STATUS->read(conn, STATUS, v, sizeof(v), 0);
	return v[i];
}

static void connect(void)
{
	conn = shim_conn_new(247);
	link_callbacks.connected(conn, 0);
}

static void disconnect(void)
{
	link_callbacks.disconnected(conn, 0x13);
	shim_conn_drop(conn);
}

/* Fresh node: no connection, idle OTA, gate free, timers stopped. */
static void setup(void)
{
	if (link_conn() != NULL) {
		disconnect();
	}
	shim_bt_reset();
	shim_flash_reset();
	k_work_cancel_delayable(&reboot_work);
	failed_sessions = 0;
	cooldown_until_ms = 0;
	ota_init();
	for (size_t i = 0; i < sizeof(image); i++) {
		image[i] = (uint8_t)(i * 7 + 3);
	}
	connect();
}

/* The link is free again and the arm timeout will drop an idle central. */
static void check_released(void)
{
	CHECK(!link_is_owner(LINK_OWNER_OTA));
	CHECK(shim_work_pending(&arm_timeout_work, NULL));
	CHECK(!shim_work_pending(&stall_timeout_work, NULL));
	CHECK(!shim_work_pending(&session_timeout_work, NULL));
	unsigned int before = shim_disconnect_requests();
	shim_advance_ms(HIVE_LINK_ARM_TIMEOUT_MS);
	CHECK_EQ(shim_disconnect_requests(), before + 1);
}

static void test_successful_transfer(void)
{
	setup();
	begin(sizeof(image), crc32_ieee(image, sizeof(image)));
	CHECK_EQ(status_byte(0), S_RECEIVING);
	CHECK(link_is_owner(LINK_OWNER_OTA));
	for (size_t off = 0; off < sizeof(image); off += 512) {
		CHECK_EQ(data(off, 512), 512);
	}
	end();
	CHECK_EQ(status_byte(0), S_DONE);
	CHECK(shim_upgrade_pending());
	shim_advance_ms(1500);
	CHECK_EQ(shim_reboots(), 1);
}

/* HI-02: a duplicate BEGIN must neither strand the link nor kill the transfer. */
static void test_duplicate_begin_keeps_transfer(void)
{
	setup();
	begin(sizeof(image), crc32_ieee(image, sizeof(image)));
	data(0, 1024);
	begin(sizeof(image), crc32_ieee(image, sizeof(image)));
	CHECK_EQ(status_byte(0), S_RECEIVING);
	CHECK_EQ(status_byte(5), E_BEGIN);
	CHECK(link_is_owner(LINK_OWNER_OTA));
	CHECK_EQ(data(1024, sizeof(image) - 1024), sizeof(image) - 1024);
	end();
	CHECK_EQ(status_byte(0), S_DONE);
}

/* HI-02: every error path hands the link back under the arm timeout. */
static void test_crc_mismatch_releases_link(void)
{
	setup();
	begin(sizeof(image), 0xdeadbeef);
	data(0, sizeof(image));
	end();
	CHECK_EQ(status_byte(0), E_CRC);
	CHECK(!shim_upgrade_pending());
	check_released();
}

static void test_early_end_releases_link(void)
{
	setup();
	begin(sizeof(image), crc32_ieee(image, sizeof(image)));
	data(0, 100);
	end();
	CHECK_EQ(status_byte(0), E_SIZE);
	check_released();
}

static void test_flash_error_releases_link(void)
{
	setup();
	begin(sizeof(image), crc32_ieee(image, sizeof(image)));
	shim_flash_fail_write(2, -EIO);
	data(0, 512);
	CHECK(data(512, 512) < 0);
	CHECK_EQ(status_byte(0), E_WRITE);
	CHECK_EQ((int8_t)status_byte(6), -EIO);
	/* Writes still in flight must not overwrite the error. */
	CHECK(data(1024, 512) < 0);
	CHECK_EQ(status_byte(0), E_WRITE);
	check_released();
}

static void test_oversized_data_releases_link(void)
{
	setup();
	begin(100, 0);
	CHECK(data(0, 200) < 0);
	CHECK_EQ(status_byte(0), E_SIZE);
	check_released();
}

static void test_unknown_opcode_releases_link(void)
{
	setup();
	begin(sizeof(image), 0);
	uint8_t op = 0x7f;
	write_attr(CTRL, &op, 1);
	CHECK_EQ(status_byte(0), E_SEQ);
	check_released();
}

/* A failed transfer can be retried on the same connection. */
static void test_retry_after_error(void)
{
	setup();
	begin(sizeof(image), 0xdeadbeef);
	data(0, sizeof(image));
	end();
	begin(sizeof(image), crc32_ieee(image, sizeof(image)));
	CHECK_EQ(status_byte(0), S_RECEIVING);
	data(0, sizeof(image));
	end();
	CHECK_EQ(status_byte(0), S_DONE);
}

/* HI-26: END is the commit point; ABORT afterwards must not cancel the reboot
 * while leaving the upgrade pending. */
static void test_abort_after_end_still_reboots(void)
{
	setup();
	begin(sizeof(image), crc32_ieee(image, sizeof(image)));
	data(0, sizeof(image));
	end();
	abort_transfer();
	CHECK_EQ(status_byte(0), S_DONE);
	CHECK(shim_upgrade_pending());
	shim_advance_ms(1500);
	CHECK_EQ(shim_reboots(), 1);
}

static void test_abort_during_transfer(void)
{
	setup();
	begin(sizeof(image), crc32_ieee(image, sizeof(image)));
	data(0, 512);
	abort_transfer();
	CHECK_EQ(status_byte(0), S_IDLE);
	check_released();
	/* An abort is not a failure for the cooldown count. */
	CHECK_EQ(failed_sessions, 0);
}

static void test_stall_timeout(void)
{
	setup();
	begin(sizeof(image), crc32_ieee(image, sizeof(image)));
	data(0, 512);
	shim_advance_ms(HIVE_OTA_STALL_TIMEOUT_MS);
	CHECK_EQ(status_byte(0), E_SEQ);
	CHECK_EQ((int8_t)status_byte(6), -ETIMEDOUT);
	CHECK(!link_is_owner(LINK_OWNER_OTA));
	CHECK(shim_disconnect_requests() >= 1);
}

/* HI-05: a transfer that keeps trickling cannot hold the node forever. */
static void test_session_timeout(void)
{
	setup();
	begin(SHIM_SLOT_SIZE, 0);
	size_t off = 0;
	for (uint32_t t = 0; t < HIVE_OTA_SESSION_TIMEOUT_MS; t += 20000) {
		data(off % sizeof(image), 1);
		off++;
		shim_advance_ms(20000);
	}
	CHECK_EQ(status_byte(0), E_SEQ);
	CHECK_EQ((int8_t)status_byte(6), -ETIME);
	CHECK(!link_is_owner(LINK_OWNER_OTA));
}

/* HI-05: repeated failures close BEGIN for a cooldown. */
static void test_failure_cooldown(void)
{
	setup();
	for (unsigned int i = 0; i < HIVE_OTA_MAX_FAILED_SESSIONS; i++) {
		begin(sizeof(image), 0xdeadbeef);
		data(0, sizeof(image));
		end();
		CHECK_EQ(status_byte(0), E_CRC);
	}
	begin(sizeof(image), crc32_ieee(image, sizeof(image)));
	CHECK_EQ(status_byte(0), E_BEGIN);
	CHECK_EQ((int8_t)status_byte(6), -EAGAIN);
	CHECK(!link_is_owner(LINK_OWNER_OTA));

	/* The cooldown outlives a reconnect, then lapses. */
	disconnect();
	connect();
	begin(sizeof(image), crc32_ieee(image, sizeof(image)));
	CHECK_EQ(status_byte(0), E_BEGIN);
	shim_advance_ms(HIVE_OTA_FAILURE_COOLDOWN_MS);
	disconnect();
	connect();
	begin(sizeof(image), crc32_ieee(image, sizeof(image)));
	CHECK_EQ(status_byte(0), S_RECEIVING);
	data(0, sizeof(image));
	end();
	CHECK_EQ(status_byte(0), S_DONE);
	CHECK_EQ(failed_sessions, 0);
}

/* HI-07: an older signed image is refused at END; equal and newer pass. */
static void test_downgrade_refused(void)
{
	setup();
	shim_set_image_version(0, true, 0, 7, 0);
	shim_set_image_version(1, true, 0, 6, 9);
	begin(sizeof(image), crc32_ieee(image, sizeof(image)));
	data(0, sizeof(image));
	end();
	CHECK_EQ(status_byte(0), E_END);
	CHECK_EQ((int8_t)status_byte(6), -EPERM);
	CHECK(!shim_upgrade_pending());
	check_released();
}

static void test_same_version_accepted(void)
{
	setup();
	shim_set_image_version(0, true, 0, 7, 0);
	shim_set_image_version(1, true, 0, 7, 0);
	begin(sizeof(image), crc32_ieee(image, sizeof(image)));
	data(0, sizeof(image));
	end();
	CHECK_EQ(status_byte(0), S_DONE);
}

static void test_unreadable_header_refused(void)
{
	setup();
	shim_set_image_version(1, false, 0, 0, 0);
	begin(sizeof(image), crc32_ieee(image, sizeof(image)));
	data(0, sizeof(image));
	end();
	CHECK_EQ(status_byte(0), E_END);
	CHECK_EQ((int8_t)status_byte(6), -EBADMSG);
	CHECK(!shim_upgrade_pending());
}

static void test_disconnect_mid_transfer(void)
{
	setup();
	begin(sizeof(image), crc32_ieee(image, sizeof(image)));
	data(0, 512);
	disconnect();
	CHECK(!link_is_owner(LINK_OWNER_OTA));
	CHECK(!link_is_busy());
	CHECK(!shim_work_pending(&stall_timeout_work, NULL));
	CHECK_EQ(failed_sessions, 1);
	/* The measurement loop can take the gate again. */
	CHECK(link_measurement_begin());
	link_measurement_end();
	connect();
}

static void test_unclaimed_connection_is_dropped(void)
{
	setup();
	unsigned int before = shim_disconnect_requests();
	shim_advance_ms(HIVE_LINK_ARM_TIMEOUT_MS - 1);
	CHECK_EQ(shim_disconnect_requests(), before);
	shim_advance_ms(1);
	CHECK_EQ(shim_disconnect_requests(), before + 1);
}

int main(void)
{
	RUN(test_successful_transfer);
	RUN(test_duplicate_begin_keeps_transfer);
	RUN(test_crc_mismatch_releases_link);
	RUN(test_early_end_releases_link);
	RUN(test_flash_error_releases_link);
	RUN(test_oversized_data_releases_link);
	RUN(test_unknown_opcode_releases_link);
	RUN(test_retry_after_error);
	RUN(test_abort_after_end_still_reboots);
	RUN(test_abort_during_transfer);
	RUN(test_stall_timeout);
	RUN(test_session_timeout);
	RUN(test_failure_cooldown);
	RUN(test_downgrade_refused);
	RUN(test_same_version_accepted);
	RUN(test_unreadable_header_refused);
	RUN(test_disconnect_mid_transfer);
	RUN(test_unclaimed_connection_is_dropped);
	return shim_failures != 0;
}

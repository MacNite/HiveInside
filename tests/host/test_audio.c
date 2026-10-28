/* Audio session lifecycle against the real link.c and audio.c, with the
 * capture and TX worker threads running concurrently as on the device. */
#include "shim_test.h"

#include "link.c"
#include "audio.c"

void beacon_connected(void) { }
void beacon_disconnected(void) { }
void hive_watchdog_feed(void) { }
void ota_link_connected(void) { }
void ota_link_disconnected(void) { }

/* ── fakes: sensor rail and PDM microphone ──────────────────────────────── */

static atomic_t rail_users, rail_underflows, rail_enable_delay_ms;

int power_sensor_rail_enable(void)
{
	k_msleep((int32_t)atomic_get(&rail_enable_delay_ms));
	__atomic_add_fetch(&rail_users, 1, __ATOMIC_SEQ_CST);
	return 0;
}

int power_sensor_rail_disable(void)
{
	if (__atomic_sub_fetch(&rail_users, 1, __ATOMIC_SEQ_CST) < 0) {
		__atomic_add_fetch(&rail_users, 1, __ATOMIC_SEQ_CST);
		__atomic_add_fetch(&rail_underflows, 1, __ATOMIC_SEQ_CST);
	}
	return 0;
}

#define BLOCK_BYTES (MIC_BLOCK_SAMPLES * sizeof(int16_t))
static int16_t pdm_block[MIC_BLOCK_SAMPLES];
static atomic_t mic_running, mic_overlaps, mic_blocks, mic_outstanding;

int mic_stream_start(void)
{
	if (atomic_set(&mic_running, 1)) {
		__atomic_add_fetch(&mic_overlaps, 1, __ATOMIC_SEQ_CST);
	}
	return 0;
}

int mic_stream_read(void **buf, uint32_t *size, int32_t timeout_ms)
{
	if (!atomic_get(&mic_running)) {
		k_msleep(timeout_ms);
		return -EIO;
	}
	k_msleep(10);  /* a block every 10 ms: ten times real time */
	__atomic_add_fetch(&mic_blocks, 1, __ATOMIC_SEQ_CST);
	__atomic_add_fetch(&mic_outstanding, 1, __ATOMIC_SEQ_CST);
	*buf = pdm_block;
	*size = BLOCK_BYTES;
	return 0;
}

void mic_stream_release(void *buf)
{
	ARG_UNUSED(buf);
	__atomic_sub_fetch(&mic_outstanding, 1, __ATOMIC_SEQ_CST);
}

void mic_stream_stop(void)
{
	atomic_set(&mic_running, 0);
}

/* ── helpers ────────────────────────────────────────────────────────────── */

#define CTRL (&audio_service_attrs[2])
#define DATA (&audio_service_attrs[4])
#define DATA_CCC (&audio_service_attrs[5])
#define STATUS (&audio_service_attrs[7])

enum { S_ARMED = 1, S_STREAMING = 2, S_DONE = 3 };

static struct bt_conn *conn;

static void read_status(uint8_t v[AUDIO_STATUS_LEN])
{
	STATUS->read(conn, STATUS, v, AUDIO_STATUS_LEN, 0);
}

static uint8_t status_state(void)
{
	uint8_t v[AUDIO_STATUS_LEN];

	read_status(v);
	return v[0];
}

static uint32_t status_u32(int offset)
{
	uint8_t v[AUDIO_STATUS_LEN];

	read_status(v);
	return sys_get_le32(&v[offset]);
}

static void connect(bool subscribe)
{
	conn = shim_conn_new(247);
	link_callbacks.connected(conn, 0);
	if (subscribe) {
		shim_subscribe(conn, DATA, true);
		DATA_CCC->ccc_changed(DATA_CCC, BT_GATT_CCC_NOTIFY);
	}
}

static void disconnect(void)
{
	link_callbacks.disconnected(conn, 0x13);
	shim_conn_drop(conn);
}

static void send_start(uint16_t duration_ds, bool valid_mac)
{
	uint8_t v[AUDIO_STATUS_LEN], req[14] = { 0x01 }, mac[32];
	uint8_t msg[14];

	read_status(v);
	sys_put_le16(duration_ds, &req[1]);
	req[3] = 0; req[4] = 0; req[5] = 0;
	memcpy(msg, &v[2], 8);
	memcpy(&msg[8], req, 6);
	shim_hmac_sha256(hive_audio_psk, sizeof(hive_audio_psk), msg, sizeof(msg), mac);
	memcpy(&req[6], mac, 8);
	if (!valid_mac) {
		req[6] ^= 0xff;
	}
	CTRL->write(conn, CTRL, req, sizeof(req), 0, 0);
}

static void send_stop(void)
{
	uint8_t op = 0x02;

	CTRL->write(conn, CTRL, &op, 1, 0, 0);
}

/* Every captured byte is either counted as sent or as dropped. */
static void check_accounting(void)
{
	CHECK_EQ(status_u32(10) + status_u32(18), atomic_get(&mic_blocks) * BLOCK_BYTES);
}

/* The session has fully let go of the hardware and the link. */
static bool session_released(void)
{
	return !link_is_owner(LINK_OWNER_AUDIO) && atomic_get(&rail_users) == 0 &&
	       !atomic_get(&mic_running) && !atomic_get(&session_active);
}

static void setup(void)
{
	if (link_conn() != NULL) {
		disconnect();
	}
	CHECK(WAIT_FOR(session_released(), 3000));
	shim_bt_reset();
	atomic_set(&mic_blocks, 0);
	atomic_set(&rail_enable_delay_ms, 0);
	connect(true);
}

/* ── tests ──────────────────────────────────────────────────────────────── */

static void test_complete_session(void)
{
	setup();
	send_start(3, true);  /* 300 ms */
	CHECK(WAIT_FOR(status_state() == S_DONE, 3000));
	CHECK(WAIT_FOR(session_released(), 1000));
	CHECK(status_u32(10) > 0);
	/* No zero-drop assertion: on a loaded host the TX thread can be
	 * descheduled long enough for the ring to overrun, which is correct
	 * behaviour. What must hold is that every byte is accounted for. */
	check_accounting();
	/* DATA carries a 4-byte header per notification plus the PCM. */
	struct shim_notify_stats s = shim_notify_stats(conn, DATA);
	CHECK_EQ(s.bytes - 4u * s.count, status_u32(10));
	CHECK_EQ(s.last_len, 4);  /* the final, empty end-of-stream packet */
	CHECK_EQ(atomic_get(&mic_overlaps), 0);
	CHECK_EQ(atomic_get(&mic_outstanding), 0);
	CHECK_EQ(atomic_get(&rail_underflows), 0);
	/* HI-21: the reported duration stops with the session. */
	uint32_t elapsed = status_u32(14);
	k_msleep(50);
	CHECK_EQ(status_u32(14), elapsed);
}

static void test_stop_while_streaming(void)
{
	setup();
	send_start(0, true);
	CHECK(WAIT_FOR(status_state() == S_STREAMING, 1000));
	k_msleep(20);
	send_stop();
	CHECK(WAIT_FOR(status_state() == S_DONE, 1000));
	CHECK(WAIT_FOR(session_released(), 1000));
	check_accounting();
}

/* HI-10: STOP before the capture thread has reached STREAMING still ends the
 * session instead of being ignored until the 60-second cap. */
static void test_stop_while_armed(void)
{
	setup();
	atomic_set(&rail_enable_delay_ms, 100);
	send_start(0, true);
	CHECK_EQ(status_state(), S_ARMED);
	send_stop();
	CHECK(WAIT_FOR(status_state() == S_DONE, 1500));
	CHECK(WAIT_FOR(session_released(), 1000));
}

/* HI-10: a rejected START reports its error without making a live session
 * look armed, and STOP keeps working. */
static void test_rejected_start_keeps_session(void)
{
	setup();
	send_start(0, true);
	CHECK(WAIT_FOR(status_state() == S_STREAMING, 1000));
	send_start(0, false);
	uint8_t v[AUDIO_STATUS_LEN];
	read_status(v);
	CHECK_EQ(v[0], S_STREAMING);
	CHECK_EQ(v[1], AUDIO_ERR_AUTH);
	send_start(0, true);  /* authentic, but a session already holds the link */
	read_status(v);
	CHECK_EQ(v[0], S_STREAMING);
	CHECK_EQ(v[1], AUDIO_ERR_BUSY);
	send_stop();
	CHECK(WAIT_FOR(status_state() == S_DONE, 1000));
	CHECK(WAIT_FOR(session_released(), 1000));
}

/* HI-03/HI-04: a disconnect with PCM still buffered ends the workers promptly,
 * returns the rail and the gate, and leaks nothing to the next central. */
static void test_disconnect_with_buffered_pcm(void)
{
	setup();
	send_start(0, true);
	CHECK(WAIT_FOR(status_state() == S_STREAMING, 1000));
	shim_set_completion(SHIM_COMPLETE_HOLD);  /* the ring now fills up */
	k_msleep(100);
	disconnect();
	CHECK(WAIT_FOR(session_released(), 1500));
	CHECK(!link_is_busy());
	/* No spinning on the dead connection. */
	CHECK(shim_notify_after_disconnect() < 5);
	check_accounting();

	/* A new central that subscribes without START receives nothing. */
	shim_set_completion(SHIM_COMPLETE_NOW);
	connect(true);
	k_msleep(200);
	CHECK_EQ(shim_notify_stats(conn, DATA).count, 0);
	CHECK_EQ(status_state(), S_ARMED);
	/* ...and can start its own session. */
	atomic_set(&mic_blocks, 0);
	send_start(2, true);
	CHECK(WAIT_FOR(status_state() == S_DONE, 3000));
	CHECK(WAIT_FOR(session_released(), 1000));
	check_accounting();
	CHECK_EQ(atomic_get(&mic_overlaps), 0);
}

/* HI-03: a link that stops confirming notifications ends the session within
 * the stall timeout. */
static void test_stalled_link(void)
{
	setup();
	shim_set_completion(SHIM_COMPLETE_HOLD);
	send_start(0, true);
	CHECK(WAIT_FOR(status_state() == S_DONE, HIVE_AUDIO_STALL_TIMEOUT_MS + 1500));
	uint8_t v[AUDIO_STATUS_LEN];
	read_status(v);
	CHECK_EQ(v[1], AUDIO_ERR_STALLED);
	CHECK(WAIT_FOR(session_released(), 1000));
	check_accounting();
	shim_release_completions();
}

/* HI-03: unsubscribing mid-stream makes notifications fail; that ends the
 * session instead of retrying forever. */
static void test_unsubscribe_mid_stream(void)
{
	setup();
	send_start(0, true);
	CHECK(WAIT_FOR(status_state() == S_STREAMING, 1000));
	k_msleep(20);
	shim_subscribe(conn, DATA, false);
	DATA_CCC->ccc_changed(DATA_CCC, 0);
	CHECK(WAIT_FOR(status_state() == S_DONE, 2000));
	uint8_t v[AUDIO_STATUS_LEN];
	read_status(v);
	CHECK_EQ(v[1], AUDIO_ERR_NO_SUB);
	CHECK(WAIT_FOR(session_released(), 1000));
	check_accounting();
}

/* The nonce authorizes one session: replaying an accepted START fails. */
static void test_nonce_single_use(void)
{
	setup();
	uint8_t before[AUDIO_STATUS_LEN], after[AUDIO_STATUS_LEN];
	read_status(before);
	send_start(1, true);
	read_status(after);
	CHECK(memcmp(&before[2], &after[2], 8) != 0);
	CHECK(WAIT_FOR(session_released(), 3000));
}

int main(void)
{
	audio_init();
	RUN(test_complete_session);
	RUN(test_stop_while_streaming);
	RUN(test_stop_while_armed);
	RUN(test_rejected_start_keeps_session);
	RUN(test_disconnect_with_buffered_pcm);
	RUN(test_stalled_link);
	RUN(test_unsubscribe_mid_stream);
	RUN(test_nonce_single_use);
	return shim_failures != 0;
}

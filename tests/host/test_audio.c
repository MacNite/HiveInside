/* Audio session lifecycle against the real link.c and audio.c, with the
 * capture and TX worker threads running concurrently as on the device. */
#include "shim_test.h"

#include <stdlib.h>

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

static void send_start_gain(uint16_t duration_ds, bool valid_mac, int8_t gain)
{
	uint8_t v[AUDIO_STATUS_LEN], req[14] = { 0x01 }, mac[32];
	uint8_t msg[14];

	read_status(v);
	sys_put_le16(duration_ds, &req[1]);
	req[3] = 0; req[4] = 0; req[5] = (uint8_t)gain;
	memcpy(msg, &v[2], 8);
	memcpy(&msg[8], req, 6);
	shim_hmac_sha256(hive_audio_psk, sizeof(hive_audio_psk), msg, sizeof(msg), mac);
	memcpy(&req[6], mac, 8);
	if (!valid_mac) {
		req[6] ^= 0xff;
	}
	CTRL->write(conn, CTRL, req, sizeof(req), 0, 0);
}

static void send_start(uint16_t duration_ds, bool valid_mac)
{
	send_start_gain(duration_ds, valid_mac, 0);
}

static void send_stop(void)
{
	uint8_t op = 0x02;

	CTRL->write(conn, CTRL, &op, 1, 0, 0);
}

/* Every captured byte past the discarded warm-up blocks is either counted as
 * sent or as dropped. */
static void check_accounting(void)
{
	uint32_t blocks = (uint32_t)atomic_get(&mic_blocks);
	uint32_t kept = blocks > MIC_WARMUP_BLOCKS ? blocks - MIC_WARMUP_BLOCKS : 0;

	CHECK_EQ(status_u32(10) + status_u32(18), kept * BLOCK_BYTES);
}

/* What the DATA notifications carried: the gain byte of every header and the
 * PCM they delivered. */
static atomic_t data_packets, gain_mismatch, pcm_peak, pcm_samples;
static int8_t data_gain;
static int64_t pcm_sum;

static void observe_data(const struct bt_gatt_attr *attr, const uint8_t *data,
			 uint16_t len)
{
	if (attr != DATA || len < 4) {
		return;
	}
	if (__atomic_fetch_add(&data_packets, 1, __ATOMIC_SEQ_CST) == 0) {
		data_gain = (int8_t)data[3];
	} else if ((int8_t)data[3] != data_gain) {
		__atomic_add_fetch(&gain_mismatch, 1, __ATOMIC_SEQ_CST);
	}
	for (uint16_t i = 4; i + 1 < len; i += 2) {
		int32_t v = (int16_t)sys_get_le16(&data[i]);
		int32_t mag = v < 0 ? -v : v;
		if (mag > atomic_get(&pcm_peak)) {
			atomic_set(&pcm_peak, mag);
		}
		pcm_sum += v;
		__atomic_add_fetch(&pcm_samples, 1, __ATOMIC_SEQ_CST);
	}
}

/* A 1 kHz tone of the given amplitude on a DC offset, plus an optional spike
 * at a zero crossing. 1600 samples are exactly 100 periods, so the fake's
 * repeated block is a continuous signal. */
static void set_pdm_signal(int16_t dc, float amplitude, int16_t spike)
{
	for (size_t i = 0; i < MIC_BLOCK_SAMPLES; i++) {
		float tone = amplitude * sinf(2.0f * 3.14159265f * (float)(i % 16) / 16.0f);
		pdm_block[i] = (int16_t)(dc + lrintf(tone));
	}
	pdm_block[800] += spike;
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
	memset(pdm_block, 0, sizeof(pdm_block));
	atomic_set(&data_packets, 0);
	atomic_set(&gain_mismatch, 0);
	atomic_set(&pcm_peak, 0);
	atomic_set(&pcm_samples, 0);
	data_gain = 0;
	pcm_sum = 0;
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

/* Run one complete session and check what it delivered. */
static void run_session(int8_t gain)
{
	send_start_gain(5, true, gain);  /* 500 ms */
	CHECK(WAIT_FOR(status_state() == S_DONE, 3000));
	CHECK(WAIT_FOR(session_released(), 1000));
	check_accounting();
	CHECK(atomic_get(&pcm_samples) > 0);
	CHECK_EQ(atomic_get(&gain_mismatch), 0);
}

static uint8_t status_clipped_percent(void)
{
	uint8_t v[AUDIO_STATUS_LEN];

	read_status(v);
	return v[29];
}

/* Issue #78: the gain decision itself. */
static void test_auto_gain_rule(void)
{
	CHECK_EQ(audio_auto_gain_db(0.0f, 0.0f), HIVE_AUDIO_GAIN_MAX_DB);
	CHECK_EQ(audio_auto_gain_db(1.0f, 1.0f), 0);  /* never attenuates */
	/* -50.5 dBFS RMS, -40 dBFS peak: the RMS target (+30.5 dB) wins,
	 * rounded down. */
	CHECK_EQ(audio_auto_gain_db(0.003f, 0.01f), 30);
	/* -50.5 dBFS RMS, -10.5 dBFS peak: the peak ceiling (+7.5 dB) wins. */
	CHECK_EQ(audio_auto_gain_db(0.003f, 0.3f), 7);
	/* Very quiet: capped at the maximum. */
	CHECK_EQ(audio_auto_gain_db(0.0001f, 0.0002f), HIVE_AUDIO_GAIN_MAX_DB);
}

/* Issue #78: a quiet hive on a DC offset is boosted to the target with one
 * gain for the whole clip, the offset removed rather than amplified, and the
 * gain reported in every DATA header. */
static void test_auto_gain_quiet_hive(void)
{
	setup();
	/* Amplitude 100: -53.3 dBFS RMS, -50.3 dBFS peak -> +33 dB. */
	set_pdm_signal(500, 100.0f, 0);
	run_session(HIVE_AUDIO_GAIN_AUTO);
	CHECK_EQ(data_gain, 33);
	/* 100 * 10^(33/20) = 4467. */
	CHECK(atomic_get(&pcm_peak) > 4300 && atomic_get(&pcm_peak) < 4600);
	/* Amplified by 33 dB, the offset alone would sit near 22 000. */
	CHECK(llabs(pcm_sum / atomic_get(&pcm_samples)) < 50);
	CHECK_EQ(status_clipped_percent(), 0);
}

/* Issue #78: a transient in the pre-roll limits the boost so it stays under
 * the peak ceiling instead of clipping. */
static void test_auto_gain_peak_ceiling(void)
{
	setup();
	/* Peak ~16 000 (-6.2 dBFS) -> +3 dB, though the RMS alone wants +18. */
	set_pdm_signal(0, 100.0f, 16000);
	run_session(HIVE_AUDIO_GAIN_AUTO);
	CHECK_EQ(data_gain, 3);
	CHECK(atomic_get(&pcm_peak) < 32767);
	CHECK_EQ(status_clipped_percent(), 0);
}

/* Issue #78: a loud input is passed through at 0 dB, not attenuated. */
static void test_auto_gain_loud_input(void)
{
	setup();
	set_pdm_signal(0, 20000.0f, 0);
	run_session(HIVE_AUDIO_GAIN_AUTO);
	CHECK_EQ(data_gain, 0);
	CHECK_EQ(status_clipped_percent(), 0);
}

/* Issue #78: manual gains reach +40 dB, are clamped to the range, and are
 * reported in the DATA header too. */
static void test_manual_gain_range(void)
{
	setup();
	set_pdm_signal(0, 100.0f, 0);
	run_session(40);
	CHECK_EQ(data_gain, 40);
	CHECK(atomic_get(&pcm_peak) > 9500 && atomic_get(&pcm_peak) < 10500);

	setup();
	run_session(60);
	CHECK_EQ(data_gain, HIVE_AUDIO_GAIN_MAX_DB);

	setup();
	run_session(-60);
	CHECK_EQ(data_gain, HIVE_AUDIO_GAIN_MIN_DB);
}

int main(void)
{
	audio_init();
	shim_set_notify_observer(observe_data);
	RUN(test_complete_session);
	RUN(test_stop_while_streaming);
	RUN(test_stop_while_armed);
	RUN(test_rejected_start_keeps_session);
	RUN(test_disconnect_with_buffered_pcm);
	RUN(test_stalled_link);
	RUN(test_unsubscribe_mid_stream);
	RUN(test_nonce_single_use);
	RUN(test_auto_gain_rule);
	RUN(test_auto_gain_quiet_hive);
	RUN(test_auto_gain_peak_ceiling);
	RUN(test_auto_gain_loud_input);
	RUN(test_manual_gain_range);
	return shim_failures != 0;
}

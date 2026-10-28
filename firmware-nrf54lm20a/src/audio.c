/* Authenticated PCM16 capture and notification streaming. */
#include "audio.h"

#include "hive_config.h"
#include "link.h"
#include "mic.h"
#include "power.h"

#if ENABLE_AUDIO

#include <errno.h>
#include <math.h>
#include <psa/crypto.h>
#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#if __has_include("audio_secret.h")
#include "audio_secret.h"
#define AUDIO_KEY_PRESENT 1
#else
#define AUDIO_KEY_PRESENT 0
static const uint8_t hive_audio_psk[32];
#endif

#define AUDIO_STATUS_LEN 30
#define AUDIO_HEADER_LEN 4
#define AUDIO_PCM_MAX 240
#define AUDIO_CREDITS CONFIG_BT_BUF_ACL_TX_COUNT
#define AUDIO_BLOCK_BYTES (MIC_BLOCK_SAMPLES * sizeof(int16_t))

enum { AUDIO_OP_START = 0x01, AUDIO_OP_STOP = 0x02 };
enum { AUDIO_IDLE, AUDIO_ARMED, AUDIO_STREAMING, AUDIO_DONE };
enum {
	AUDIO_ERR_PARAM = 0x10, AUDIO_ERR_AUTH, AUDIO_ERR_BUSY,
	AUDIO_ERR_MIC, AUDIO_ERR_NO_SUB, AUDIO_ERR_STALLED,
	AUDIO_ERR_FORMAT,
};

static uint8_t state, error, nonce[8], format;
static bool nonce_usable;
static uint32_t bytes_sent, dropped_bytes, running_crc, clipped, samples;
static uint16_t seq, duration_ds;
static int8_t gain_db;
static float gain_factor;
static bool gain_auto;
static int64_t started_ms, finished_ms, deadline_ms;
static bool subscribed;
/* The connection a session was started on. Its workers stop as soon as that
 * connection is gone, even if a new central has connected in the meantime. */
static uint32_t session_gen;
static atomic_t session_active, stopping, capture_done, dropped_pending;
#define AUDIO_RING_BLOCKS (HIVE_AUDIO_RING_BYTES / AUDIO_BLOCK_BYTES)
BUILD_ASSERT(HIVE_AUDIO_AGC_BLOCKS >= 1 &&
	     HIVE_AUDIO_AGC_BLOCKS < AUDIO_RING_BLOCKS,
	     "the auto-gain pre-roll must fit in the ring without overrunning it");
BUILD_ASSERT(HIVE_AUDIO_GAIN_MIN_DB > INT8_MIN &&
	     HIVE_AUDIO_GAIN_MAX_DB < HIVE_AUDIO_GAIN_AUTO,
	     "manual gains must stay distinct from the auto-gain request");
static uint8_t audio_ring[AUDIO_RING_BLOCKS][AUDIO_BLOCK_BYTES] __aligned(4);
static uint16_t ring_head, ring_tail, ring_count, ring_offset;
K_MUTEX_DEFINE(audio_ring_lock);
/* Counters read by STATUS while the workers update them. */
static struct k_spinlock stats_lock;
K_SEM_DEFINE(audio_capture_start, 0, 1);
K_SEM_DEFINE(audio_tx_start, 0, 1);
K_SEM_DEFINE(audio_data_ready, 0, 1);
K_SEM_DEFINE(audio_credits, AUDIO_CREDITS, AUDIO_CREDITS);

/* bt_gatt_notify_cb() copies the value into its own buffer before returning,
 * so one staging buffer serves every notification; the credits alone bound
 * what is in flight. Word alignment keeps the int16_t samples at offset 4
 * aligned. */
static uint8_t tx_buf[AUDIO_HEADER_LEN + AUDIO_PCM_MAX] __aligned(4);
static ssize_t ctrl_write(struct bt_conn *, const struct bt_gatt_attr *,
			  const void *, uint16_t, uint16_t, uint8_t);
static ssize_t status_read(struct bt_conn *, const struct bt_gatt_attr *,
			   void *, uint16_t, uint16_t);
static void data_ccc_changed(const struct bt_gatt_attr *, uint16_t);

#define UUID128(n) BT_UUID_128_ENCODE(n, 0x7a1c, 0x4b9e, 0x9a2f, 0x1d6e0b9c1a01)
static struct bt_uuid_128 service_uuid = BT_UUID_INIT_128(UUID128(0x8e8b0002));
static struct bt_uuid_128 ctrl_uuid = BT_UUID_INIT_128(UUID128(0x8e8b0020));
static struct bt_uuid_128 data_uuid = BT_UUID_INIT_128(UUID128(0x8e8b0021));
static struct bt_uuid_128 status_uuid = BT_UUID_INIT_128(UUID128(0x8e8b0023));

BT_GATT_SERVICE_DEFINE(audio_service,
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
#define AUDIO_ATTR_DATA 4
#define AUDIO_ATTR_STATUS 7

static void rotate_nonce(void)
{
	nonce_usable = false;
	if (bt_rand(nonce, sizeof(nonce)) == 0) {
		nonce_usable = true;
	} else {
		/* Substituting a constant after RNG failure made an observed START
		 * HMAC replayable. Keep the advertised value inert and fail closed. */
		memset(nonce, 0, sizeof(nonce));
		printk("[AUDIO] nonce generation failed; START disabled\n");
	}
}

static void fill_status(uint8_t v[AUDIO_STATUS_LEN])
{
	k_spinlock_key_t key = k_spin_lock(&stats_lock);
	int64_t until = finished_ms > 0 ? finished_ms : k_uptime_get();

	v[0] = state; v[1] = error;
	memcpy(&v[2], nonce, sizeof(nonce));
	sys_put_le32(bytes_sent, &v[10]);
	/* Frozen at the end of a session, so DONE reports its real duration. */
	uint32_t elapsed = started_ms > 0 ? (uint32_t)(until - started_ms) : 0;
	sys_put_le32(elapsed, &v[14]);
	sys_put_le32(dropped_bytes, &v[18]);
	sys_put_le32(running_crc, &v[22]);
	sys_put_le16(MIC_SAMPLE_RATE, &v[26]);
	v[28] = format;
	v[29] = samples ? (uint8_t)MIN(100U, (clipped * 100U) / samples) : 0;
	k_spin_unlock(&stats_lock, key);
}

static void publish_status(void)
{
	uint8_t value[AUDIO_STATUS_LEN];
	struct bt_conn *conn = link_conn_get(link_generation());

	if (conn == NULL) {
		return;
	}
	fill_status(value);
	(void)bt_gatt_notify(conn, &audio_service.attrs[AUDIO_ATTR_STATUS],
			     value, sizeof(value));
	bt_conn_unref(conn);
}

static void set_state(uint8_t value, uint8_t fault)
{
	state = value; error = fault; publish_status();
}

static bool key_usable(void)
{
	uint8_t any = 0;
	for (size_t i = 0; i < sizeof(hive_audio_psk); i++) any |= hive_audio_psk[i];
	return AUDIO_KEY_PRESENT && any != 0;
}

static bool authenticate(const uint8_t *request)
{
	if (!nonce_usable) {
		printk("[AUDIO] START refused: no unpredictable nonce\n");
		return false;
	}
	if (!key_usable()) {
		printk("[AUDIO] START refused: audio_secret.h is absent or all zero\n");
		return false;
	}
	uint8_t message[14], mac[32] = { 0 };
	memcpy(message, nonce, 8);
	memcpy(&message[8], request, 6);
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t key;
	size_t mac_len = 0;
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
	psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
	psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
	psa_set_key_bits(&attr, 256);
	psa_status_t rc = psa_import_key(&attr, hive_audio_psk,
					 sizeof(hive_audio_psk), &key);
	psa_reset_key_attributes(&attr);
	if (rc == PSA_SUCCESS) {
		rc = psa_mac_compute(key, PSA_ALG_HMAC(PSA_ALG_SHA_256), message,
				     sizeof(message), mac, sizeof(mac), &mac_len);
		(void)psa_destroy_key(key);
	}
	uint8_t difference = (rc == PSA_SUCCESS && mac_len >= 8) ? 0 : 1;
	for (size_t i = 0; i < 8; i++) difference |= mac[i] ^ request[6 + i];
	return difference == 0;
}

static void data_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr); subscribed = value == BT_GATT_CCC_NOTIFY;
}

static void sent_cb(struct bt_conn *conn, void *user_data)
{
	ARG_UNUSED(conn); ARG_UNUSED(user_data);
	k_sem_give(&audio_credits);
}

/* Send tx_buf with one credit already taken. A failed send hands the credit
 * back and leaves the sequence number for the retry. */
static int notify_data(struct bt_conn *conn, size_t pcm_len, uint8_t flags)
{
	sys_put_le16(seq, tx_buf);
	tx_buf[2] = flags; tx_buf[3] = (uint8_t)gain_db;
	struct bt_gatt_notify_params params = {
		.attr = &audio_service.attrs[AUDIO_ATTR_DATA], .data = tx_buf,
		.len = pcm_len + AUDIO_HEADER_LEN, .func = sent_cb,
	};
	int rc = bt_gatt_notify_cb(conn, &params);
	if (rc != 0) {
		k_sem_give(&audio_credits);
	} else {
		seq++;
	}
	return rc;
}

/* notify_data(), retrying while the host is merely out of buffers, which a
 * credit does not fully rule out. Bounded by the stall timeout. */
static int notify_data_retry(struct bt_conn *conn, size_t pcm_len, uint8_t flags)
{
	int64_t give_up = k_uptime_get() + HIVE_AUDIO_STALL_TIMEOUT_MS;
	int rc;

	while ((rc = notify_data(conn, pcm_len, flags)) == -ENOMEM &&
	       k_uptime_get() < give_up) {
		k_sleep(K_MSEC(5));
		if (k_sem_take(&audio_credits,
			       K_MSEC(HIVE_AUDIO_STALL_TIMEOUT_MS)) != 0) {
			return -EAGAIN;
		}
	}
	return rc;
}

/* True while the connection the session was started on is still up. */
static bool session_link_alive(void)
{
	struct bt_conn *conn = link_conn_get(session_gen);

	if (conn == NULL) {
		return false;
	}
	bt_conn_unref(conn);
	return true;
}

/* The one place a session ends, reached only after both workers have stopped
 * touching the PDM controller and the ring. The session gate is the last thing
 * released, so neither the measurement loop nor a new START can begin before
 * the sensor rail reference is back and the old PCM is gone. */
static void session_finish(uint8_t final_state, uint8_t fault)
{
	(void)power_sensor_rail_disable();
	k_mutex_lock(&audio_ring_lock, K_FOREVER);
	ring_head = ring_tail = ring_count = ring_offset = 0;
	memset(audio_ring, 0, sizeof(audio_ring));
	k_mutex_unlock(&audio_ring_lock);
	/* A connection that replaced the session's own has already been given a
	 * fresh ARMED state and nonce; the old session's ending is not its news. */
	if (link_generation() == session_gen) {
		set_state(final_state, fault);
	}
	atomic_clear(&session_active);
	link_release(LINK_OWNER_AUDIO);
}

static void set_gain(int8_t db)
{
	gain_db = db;
	gain_factor = powf(10.0f, (float)db / 20.0f);
}

/* The one gain of an auto-gain clip, in whole dB, from the pre-roll's RMS and
 * peak as fractions of full scale: the boost that brings the RMS to the target,
 * unless the peak would pass the ceiling first. Rounded down so the ceiling
 * holds; silence gets the maximum, and a loud hive is never attenuated. */
static int8_t audio_auto_gain_db(float rms, float peak)
{
	float gain = (float)HIVE_AUDIO_GAIN_MAX_DB;

	if (rms > 0.0f) {
		gain = MIN(gain, HIVE_AUDIO_AGC_TARGET_DBFS - 20.0f * log10f(rms));
	}
	if (peak > 0.0f) {
		gain = MIN(gain, HIVE_AUDIO_AGC_PEAK_CEIL_DBFS - 20.0f * log10f(peak));
	}
	return (int8_t)CLAMP(floorf(gain), 0.0f, (float)HIVE_AUDIO_GAIN_MAX_DB);
}

/* One-pole DC blocker, y[n] = x[n] - x[n-1] + a*y[n-1]. Primed with the first
 * sample, so the PDM offset does not start the clip with a step. Only the
 * capture thread touches its state. */
#define DC_POLE (1.0f - 2.0f * 3.14159265f * HIVE_AUDIO_DC_CUTOFF_HZ / \
		 (float)MIC_SAMPLE_RATE)
static float dc_prev_in, dc_prev_out;
static bool dc_primed;

/* Filter n samples from in to out; out NULL only advances the filter. */
static void dc_block(const int16_t *in, int16_t *out, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		float x = (float)in[i];
		if (!dc_primed) {
			dc_prev_in = x;
			dc_primed = true;
		}
		float y = x - dc_prev_in + DC_POLE * dc_prev_out;
		dc_prev_in = x;
		dc_prev_out = y;
		if (out != NULL) {
			out[i] = (int16_t)CLAMP(lrintf(y), INT16_MIN, INT16_MAX);
		}
	}
}

static void capture_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	for (;;) {
		k_sem_take(&audio_capture_start, K_FOREVER);
		/* link_claim() transferred the session gate to this thread.  Keep
		 * regulator settling and PDM setup out of the GATT write callback. */
		if (power_sensor_rail_enable() != 0) {
			/* No reference was taken, so there is none to return. */
			atomic_clear(&session_active);
			if (link_generation() == session_gen) {
				set_state(AUDIO_ARMED, AUDIO_ERR_MIC);
			}
			link_release(LINK_OWNER_AUDIO);
			continue;
		}
		if (mic_stream_start() != 0) {
			session_finish(AUDIO_ARMED, AUDIO_ERR_MIC);
			continue;
		}
		set_state(AUDIO_STREAMING, 0);
		k_sem_give(&audio_tx_start);
		/* The first blocks carry the PDM filter's start-up transient: they
		 * would sound as a pop and inflate the auto-gain peak. They still
		 * run through the DC blocker so it has settled by the first kept
		 * sample. */
		uint32_t warmup = MIC_WARMUP_BLOCKS;
		dc_primed = false;
		dc_prev_out = 0.0f;
		while (!atomic_get(&stopping) && session_link_alive() &&
		       k_uptime_get() < deadline_ms) {
			void *block; uint32_t size;
			if (mic_stream_read(&block, &size, 250) != 0) continue;
			size_t n = MIN(size, AUDIO_BLOCK_BYTES) / sizeof(int16_t);
			if (warmup > 0) {
				warmup--;
				dc_block(block, NULL, n);
				mic_stream_release(block);
				continue;
			}
			k_mutex_lock(&audio_ring_lock, K_FOREVER);
			if (ring_count == AUDIO_RING_BLOCKS) {
				/* Discard exactly one capture block. Dropping a byte range
				 * could manufacture a false sample at the splice. Only
				 * its unsent part is lost. */
				k_spinlock_key_t key = k_spin_lock(&stats_lock);
				dropped_bytes += AUDIO_BLOCK_BYTES - ring_offset;
				k_spin_unlock(&stats_lock, key);
				ring_tail = (ring_tail + 1U) % AUDIO_RING_BLOCKS;
				ring_count--;
				ring_offset = 0;
				atomic_set(&dropped_pending, 1);
			}
			dc_block(block, (int16_t *)audio_ring[ring_head], n);
			ring_head = (ring_head + 1U) % AUDIO_RING_BLOCKS;
			ring_count++;
			k_mutex_unlock(&audio_ring_lock);
			mic_stream_release(block);
			k_sem_give(&audio_data_ready);
		}
		atomic_set(&stopping, 1);
		mic_stream_stop();
		atomic_set(&capture_done, 1);
		k_sem_give(&audio_data_ready);
	}
}
K_THREAD_DEFINE(audio_capture_thread, 2048, capture_thread_fn,
		NULL, NULL, NULL, 6, 0, 0);

/* Move up to one notification's worth of the ring into tx_buf, apply the gain
 * and return the byte count, or 0 when the ring is empty. */
static size_t stage_pcm(size_t limit)
{
	size_t amount = 0;

	k_mutex_lock(&audio_ring_lock, K_FOREVER);
	if (ring_count != 0) {
		amount = MIN(limit, (size_t)(AUDIO_BLOCK_BYTES - ring_offset));
		memcpy(&tx_buf[AUDIO_HEADER_LEN], &audio_ring[ring_tail][ring_offset],
		       amount);
		ring_offset += amount;
		if (ring_offset == AUDIO_BLOCK_BYTES) {
			ring_offset = 0;
			ring_tail = (ring_tail + 1U) % AUDIO_RING_BLOCKS;
			ring_count--;
		}
	}
	k_mutex_unlock(&audio_ring_lock);

	int16_t *pcm = (int16_t *)&tx_buf[AUDIO_HEADER_LEN];
	uint32_t clipped_now = 0;
	for (size_t i = 0; i < amount / 2; i++) {
		float scaled = (float)pcm[i] * gain_factor;
		if (scaled > INT16_MAX) { pcm[i] = INT16_MAX; clipped_now++; }
		else if (scaled < INT16_MIN) { pcm[i] = INT16_MIN; clipped_now++; }
		else pcm[i] = (int16_t)scaled;
	}
	k_spinlock_key_t key = k_spin_lock(&stats_lock);
	clipped += clipped_now;
	samples += amount / 2;
	k_spin_unlock(&stats_lock, key);
	return amount;
}

static bool ring_empty(void)
{
	k_mutex_lock(&audio_ring_lock, K_FOREVER);
	bool empty = ring_count == 0;
	k_mutex_unlock(&audio_ring_lock);
	return empty;
}

/* Bytes still buffered when a session is abandoned; they count as dropped. */
static uint32_t ring_unsent(void)
{
	k_mutex_lock(&audio_ring_lock, K_FOREVER);
	uint32_t unsent = ring_count == 0 ? 0 :
		(uint32_t)ring_count * AUDIO_BLOCK_BYTES - ring_offset;
	k_mutex_unlock(&audio_ring_lock);
	return unsent;
}

/* For an auto-gain clip, hold the first notification until the pre-roll is
 * buffered (or capture has ended early), then fix the clip's gain from it.
 * Nothing has been sent yet, so the whole clip carries the one gain. Returns
 * 0, or AUDIO_ERR_STALLED if the session's connection went away. */
static uint8_t settle_auto_gain(void)
{
	for (;;) {
		if (!session_link_alive()) {
			return AUDIO_ERR_STALLED;
		}
		k_mutex_lock(&audio_ring_lock, K_FOREVER);
		bool ready = ring_count >= HIVE_AUDIO_AGC_BLOCKS;
		k_mutex_unlock(&audio_ring_lock);
		if (ready || atomic_get(&capture_done)) {
			break;
		}
		(void)k_sem_take(&audio_data_ready, K_MSEC(100));
	}

	uint64_t sum_sq = 0;
	int32_t peak = 0;
	uint32_t n = 0;
	k_mutex_lock(&audio_ring_lock, K_FOREVER);
	for (uint16_t b = 0; b < ring_count; b++) {
		const int16_t *pcm = (const int16_t *)
			audio_ring[(ring_tail + b) % AUDIO_RING_BLOCKS];
		for (size_t i = 0; i < MIC_BLOCK_SAMPLES; i++) {
			int32_t v = pcm[i];
			sum_sq += (uint64_t)(v * v);
			peak = MAX(peak, v < 0 ? -v : v);
		}
		n += MIC_BLOCK_SAMPLES;
	}
	k_mutex_unlock(&audio_ring_lock);

	float rms = n ? sqrtf((float)sum_sq / (float)n) / 32768.0f : 0.0f;
	set_gain(audio_auto_gain_db(rms, (float)peak / 32768.0f));
	printk("[AUDIO] auto gain %+d dB (pre-roll rms %.1f dBFS, peak %d)\n",
	       gain_db, (double)(rms > 0.0f ? 20.0f * log10f(rms) : -200.0f),
	       (int)peak);
	return 0;
}

/* Stream the ring until capture has finished and the ring is drained.
 * Returns 0, or the fault that abandoned the session: the session's
 * connection went away, the link stopped returning credits, or a notification
 * was refused. Every wait in here is bounded. */
static uint8_t stream_session(void)
{
	if (gain_auto) {
		uint8_t fault = settle_auto_gain();
		if (fault != 0) {
			return fault;
		}
	}
	for (;;) {
		if (!session_link_alive()) {
			return AUDIO_ERR_STALLED;
		}
		if (ring_empty()) {
			if (atomic_get(&capture_done)) {
				return 0;
			}
			(void)k_sem_take(&audio_data_ready, K_MSEC(100));
			continue;
		}
		if (k_sem_take(&audio_credits,
			       K_MSEC(HIVE_AUDIO_STALL_TIMEOUT_MS)) != 0) {
			return AUDIO_ERR_STALLED;
		}
		struct bt_conn *conn = link_conn_get(session_gen);
		uint16_t mtu = conn ? bt_gatt_get_mtu(conn) : 0;
		size_t limit = MIN((size_t)AUDIO_PCM_MAX,
				   mtu > 7 ? (size_t)(mtu - 7) : 0) & ~(size_t)1;
		if (limit == 0) {
			k_sem_give(&audio_credits);
			if (conn) bt_conn_unref(conn);
			return AUDIO_ERR_STALLED;
		}
		size_t amount = stage_pcm(limit);
		uint8_t flags = atomic_cas(&dropped_pending, 1, 0) ? BIT(1) : 0;
		int rc = notify_data_retry(conn, amount, flags);
		bt_conn_unref(conn);
		if (rc != 0) {
			/* The staged bytes never left; account them as dropped. */
			k_spinlock_key_t key = k_spin_lock(&stats_lock);
			dropped_bytes += amount;
			k_spin_unlock(&stats_lock, key);
			return subscribed ? AUDIO_ERR_STALLED : AUDIO_ERR_NO_SUB;
		}
		/* CRC and byte count describe the same stream: the bytes handed
		 * to the link. */
		k_spinlock_key_t key = k_spin_lock(&stats_lock);
		running_crc = crc32_ieee_update(running_crc,
						&tx_buf[AUDIO_HEADER_LEN], amount);
		bytes_sent += amount;
		k_spin_unlock(&stats_lock, key);
	}
}

static void tx_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	for (;;) {
		k_sem_take(&audio_tx_start, K_FOREVER);
		uint8_t fault = stream_session();

		/* Stop capture (a no-op if it already ended) and wait for it to
		 * let go of the PDM controller. Its reads time out after 250 ms,
		 * so this wait is short and bounded. */
		atomic_set(&stopping, 1);
		while (!atomic_get(&capture_done)) {
			(void)k_sem_take(&audio_data_ready, K_MSEC(100));
		}
		if (fault != 0) {
			uint32_t unsent = ring_unsent();
			k_spinlock_key_t key = k_spin_lock(&stats_lock);
			dropped_bytes += unsent;
			k_spin_unlock(&stats_lock, key);
		} else {
			struct bt_conn *conn = link_conn_get(session_gen);
			if (conn != NULL && subscribed &&
			    k_sem_take(&audio_credits,
				       K_MSEC(HIVE_AUDIO_STALL_TIMEOUT_MS)) == 0) {
				(void)notify_data(conn, 0, BIT(0));
			}
			if (conn != NULL) bt_conn_unref(conn);
		}
		k_spinlock_key_t key = k_spin_lock(&stats_lock);
		finished_ms = k_uptime_get();
		k_spin_unlock(&stats_lock, key);
		session_finish(AUDIO_DONE, fault);
	}
}
K_THREAD_DEFINE(audio_tx_thread, 3072, tx_thread_fn, NULL, NULL, NULL, 7, 0, 0);

static ssize_t ctrl_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  const void *buf, uint16_t len, uint16_t offset,
			  uint8_t flags)
{
	ARG_UNUSED(attr); ARG_UNUSED(flags);
	const uint8_t *p = buf;
	if (offset != 0 || len == 0) return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	/* STOP cancels the session whether it is still ARMED (capture not yet
	 * running) or already STREAMING. */
	if (p[0] == AUDIO_OP_STOP && len == 1) {
		if (atomic_get(&session_active)) {
			atomic_set(&stopping, 1);
			k_sem_give(&audio_data_ready);
		}
		return len;
	}
	/* A refused request reports its error but never changes the state: a
	 * retried or malformed START must not make a live session look idle. */
	if (p[0] != AUDIO_OP_START || len != 14) {
		set_state(state, AUDIO_ERR_PARAM);
		return len;
	}
	if (p[3] != 0) { set_state(state, AUDIO_ERR_PARAM); return len; }
	if (p[4] != 0) { set_state(state, AUDIO_ERR_FORMAT); return len; }
	if (!authenticate(p)) {
		rotate_nonce(); set_state(state, AUDIO_ERR_AUTH); return len;
	}
	if (!subscribed) { set_state(state, AUDIO_ERR_NO_SUB); return len; }
	if (!link_claim(LINK_OWNER_AUDIO)) { set_state(state, AUDIO_ERR_BUSY); return len; }

	/* Each nonce authorizes exactly one session. */
	rotate_nonce();
	session_gen = link_generation();
	atomic_set(&session_active, 1);
	duration_ds = sys_get_le16(&p[1]); format = p[4];
	gain_auto = (int8_t)p[5] == HIVE_AUDIO_GAIN_AUTO;
	set_gain(gain_auto ? 0 : CLAMP((int8_t)p[5], HIVE_AUDIO_GAIN_MIN_DB,
				       HIVE_AUDIO_GAIN_MAX_DB));
	k_spinlock_key_t key = k_spin_lock(&stats_lock);
	bytes_sent = dropped_bytes = running_crc = clipped = samples = 0;
	started_ms = k_uptime_get();
	finished_ms = 0;
	k_spin_unlock(&stats_lock, key);
	uint32_t requested_ms = duration_ds ? (uint32_t)duration_ds * 100U :
					 HIVE_AUDIO_MAX_SECONDS * 1000U;
	deadline_ms = started_ms + MIN(requested_ms,
				       HIVE_AUDIO_MAX_SECONDS * 1000U);
	atomic_clear(&stopping); atomic_clear(&capture_done);
	atomic_clear(&dropped_pending);
	ring_head = ring_tail = ring_count = ring_offset = 0;
	k_sem_reset(&audio_data_ready);
	k_sem_reset(&audio_credits);
	for (int i = 0; i < AUDIO_CREDITS; i++) k_sem_give(&audio_credits);
	struct bt_le_conn_param cp = { .interval_min = HIVE_AUDIO_CONN_INTERVAL_UNITS,
		.interval_max = HIVE_AUDIO_CONN_INTERVAL_UNITS, .latency = 0,
		.timeout = 200 };
	(void)bt_conn_le_param_update(conn, &cp);
	(void)bt_conn_le_data_len_update(conn, BT_LE_DATA_LEN_PARAM_MAX);
	(void)bt_conn_le_phy_update(conn, BT_CONN_LE_PHY_PARAM_2M);
	set_state(AUDIO_ARMED, 0);
	k_sem_give(&audio_capture_start);
	return len;
}

static ssize_t status_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			   void *buf, uint16_t len, uint16_t offset)
{
	uint8_t value[AUDIO_STATUS_LEN]; fill_status(value);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(value));
}

void audio_init(void)
{
	psa_status_t rc = psa_crypto_init();
	state = AUDIO_IDLE; error = 0;
	if (rc != PSA_SUCCESS) printk("[AUDIO] PSA Crypto init failed (%d)\n", (int)rc);
}

void audio_link_connected(void)
{
	/* A session from the previous connection may still be shutting down; it
	 * owns its counters until session_finish(), so only the per-connection
	 * view is reset here. */
	subscribed = false; rotate_nonce();
	if (!atomic_get(&session_active)) {
		k_spinlock_key_t key = k_spin_lock(&stats_lock);
		started_ms = finished_ms = 0;
		k_spin_unlock(&stats_lock, key);
	}
	set_state(AUDIO_ARMED, 0);
}

void audio_link_disconnected(void)
{
	subscribed = false; atomic_set(&stopping, 1); k_sem_give(&audio_data_ready);
}

#else
void audio_init(void) { }
void audio_link_connected(void) { }
void audio_link_disconnected(void) { }
#endif

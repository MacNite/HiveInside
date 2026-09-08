/* Authenticated PCM16 capture and notification streaming. */
#include "audio.h"

#include "hive_config.h"
#include "link.h"
#include "mic.h"
#include "power.h"

#if ENABLE_AUDIO

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
static uint32_t bytes_sent, dropped_bytes, running_crc, clipped, samples;
static uint16_t seq, duration_ds;
static int8_t gain_db;
static float gain_factor;
static int64_t started_ms, deadline_ms;
static bool subscribed;
static atomic_t stopping, capture_done, dropped_pending;
#define AUDIO_RING_BLOCKS (HIVE_AUDIO_RING_BYTES / AUDIO_BLOCK_BYTES)
static uint8_t audio_ring[AUDIO_RING_BLOCKS][AUDIO_BLOCK_BYTES];
static uint16_t ring_head, ring_tail, ring_count, ring_offset;
K_MUTEX_DEFINE(audio_ring_lock);
K_SEM_DEFINE(audio_capture_start, 0, 1);
K_SEM_DEFINE(audio_tx_start, 0, 1);
K_SEM_DEFINE(audio_data_ready, 0, 1);
K_SEM_DEFINE(audio_credits, AUDIO_CREDITS, AUDIO_CREDITS);

struct tx_packet { uint8_t data[AUDIO_HEADER_LEN + AUDIO_PCM_MAX]; bool busy; };
static struct tx_packet packets[AUDIO_CREDITS];
static struct k_spinlock packet_lock;

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
	if (bt_rand(nonce, sizeof(nonce)) != 0) {
		/* A predictable challenge must never make a microphone available. */
		memset(nonce, 0, sizeof(nonce));
	}
}

static void fill_status(uint8_t v[AUDIO_STATUS_LEN])
{
	v[0] = state; v[1] = error;
	memcpy(&v[2], nonce, sizeof(nonce));
	sys_put_le32(bytes_sent, &v[10]);
	uint32_t elapsed = started_ms > 0 ? (uint32_t)(k_uptime_get() - started_ms) : 0;
	sys_put_le32(elapsed, &v[14]);
	sys_put_le32(dropped_bytes, &v[18]);
	sys_put_le32(running_crc, &v[22]);
	sys_put_le16(MIC_SAMPLE_RATE, &v[26]);
	v[28] = format;
	v[29] = samples ? (uint8_t)MIN(100U, (clipped * 100U) / samples) : 0;
}

static void publish_status(void)
{
	uint8_t value[AUDIO_STATUS_LEN];
	fill_status(value);
	(void)bt_gatt_notify(link_conn(), &audio_service.attrs[AUDIO_ATTR_STATUS],
			     value, sizeof(value));
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

static struct tx_packet *packet_take(void)
{
	k_spinlock_key_t key = k_spin_lock(&packet_lock);
	struct tx_packet *found = NULL;
	for (size_t i = 0; i < ARRAY_SIZE(packets); i++) {
		if (!packets[i].busy) { packets[i].busy = true; found = &packets[i]; break; }
	}
	k_spin_unlock(&packet_lock, key);
	return found;
}

static void sent_cb(struct bt_conn *conn, void *user_data)
{
	ARG_UNUSED(conn);
	k_spinlock_key_t key = k_spin_lock(&packet_lock);
	((struct tx_packet *)user_data)->busy = false;
	k_spin_unlock(&packet_lock, key);
	k_sem_give(&audio_credits);
}

static int notify_packet(struct tx_packet *packet, size_t pcm_len, uint8_t flags)
{
	sys_put_le16(seq++, packet->data);
	packet->data[2] = flags; packet->data[3] = 0;
	struct bt_gatt_notify_params params = {
		.attr = &audio_service.attrs[AUDIO_ATTR_DATA], .data = packet->data,
		.len = pcm_len + AUDIO_HEADER_LEN, .func = sent_cb,
		.user_data = packet,
	};
	int rc = bt_gatt_notify_cb(link_conn(), &params);
	if (rc != 0) {
		packet->busy = false; k_sem_give(&audio_credits);
	}
	return rc;
}

static void capture_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	for (;;) {
		k_sem_take(&audio_capture_start, K_FOREVER);
		/* link_claim() transferred the session gate to this thread.  Keep
		 * regulator settling and PDM setup out of the GATT write callback. */
		if (power_sensor_rail_enable() != 0 || mic_stream_start() != 0) {
			(void)power_sensor_rail_disable();
			set_state(AUDIO_ARMED, AUDIO_ERR_MIC);
			link_release(LINK_OWNER_AUDIO);
			continue;
		}
		set_state(AUDIO_STREAMING, 0);
		k_sem_give(&audio_tx_start);
		while (!atomic_get(&stopping) && link_conn() != NULL &&
		       k_uptime_get() < deadline_ms) {
			void *block; uint32_t size;
			if (mic_stream_read(&block, &size, 250) != 0) continue;
			k_mutex_lock(&audio_ring_lock, K_FOREVER);
			if (ring_count == AUDIO_RING_BLOCKS) {
				/* Discard exactly one capture block. Dropping a byte range
				 * could manufacture a false sample at the splice. */
				ring_tail = (ring_tail + 1U) % AUDIO_RING_BLOCKS;
				ring_count--;
				ring_offset = 0;
				dropped_bytes += AUDIO_BLOCK_BYTES;
				atomic_set(&dropped_pending, 1);
			}
			memcpy(audio_ring[ring_head], block, MIN(size, AUDIO_BLOCK_BYTES));
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

static void tx_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	for (;;) {
		k_sem_take(&audio_tx_start, K_FOREVER);
		for (;;) {
			k_mutex_lock(&audio_ring_lock, K_FOREVER);
			uint32_t available = ring_count == 0 ? 0 :
				AUDIO_BLOCK_BYTES - ring_offset;
			k_mutex_unlock(&audio_ring_lock);
			if (available == 0) {
				if (atomic_get(&capture_done)) break;
				(void)k_sem_take(&audio_data_ready, K_MSEC(100));
				continue;
			}
			if (k_sem_take(&audio_credits,
				       K_MSEC(HIVE_AUDIO_STALL_TIMEOUT_MS)) != 0) {
				error = AUDIO_ERR_STALLED; atomic_set(&stopping, 1); continue;
			}
			struct tx_packet *packet = packet_take();
			if (packet == NULL) { k_sem_give(&audio_credits); continue; }
			uint16_t mtu = link_conn() ? bt_gatt_get_mtu(link_conn()) : 0;
			size_t amount = MIN((size_t)AUDIO_PCM_MAX,
					    mtu > 7 ? (size_t)(mtu - 7) : 0);
			amount &= ~1U;
			if (amount == 0) { packet->busy = false; k_sem_give(&audio_credits); atomic_set(&stopping, 1); continue; }
			k_mutex_lock(&audio_ring_lock, K_FOREVER);
			amount = MIN(amount, available);
			memcpy(&packet->data[4], &audio_ring[ring_tail][ring_offset], amount);
			ring_offset += amount;
			if (ring_offset == AUDIO_BLOCK_BYTES) {
				ring_offset = 0;
				ring_tail = (ring_tail + 1U) % AUDIO_RING_BLOCKS;
				ring_count--;
			}
			k_mutex_unlock(&audio_ring_lock);
			int16_t *pcm = (int16_t *)&packet->data[4];
			for (size_t i = 0; i < amount / 2; i++) {
				float scaled = (float)pcm[i] * gain_factor;
				if (scaled > INT16_MAX) { pcm[i] = INT16_MAX; clipped++; }
				else if (scaled < INT16_MIN) { pcm[i] = INT16_MIN; clipped++; }
				else pcm[i] = (int16_t)scaled;
				samples++;
			}
			running_crc = crc32_ieee_update(running_crc, &packet->data[4], amount);
			uint8_t flags = atomic_cas(&dropped_pending, 1, 0) ? BIT(1) : 0;
			if (notify_packet(packet, amount, flags) == 0) bytes_sent += amount;
			else atomic_set(&stopping, 1);
		}
		if (link_conn() != NULL && subscribed &&
		    k_sem_take(&audio_credits, K_MSEC(HIVE_AUDIO_STALL_TIMEOUT_MS)) == 0) {
			struct tx_packet *packet = packet_take();
			if (packet != NULL) (void)notify_packet(packet, 0, BIT(0));
			else k_sem_give(&audio_credits);
		}
		(void)power_sensor_rail_disable();
		state = AUDIO_DONE;
		rotate_nonce();
		publish_status();
		link_release(LINK_OWNER_AUDIO);
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
	if (p[0] == AUDIO_OP_STOP && len == 1) {
		if (state == AUDIO_STREAMING) atomic_set(&stopping, 1);
		return len;
	}
	if (p[0] != AUDIO_OP_START || len != 14) {
		set_state(state, AUDIO_ERR_PARAM);
		return len;
	}
	if (p[3] != 0) { set_state(state, AUDIO_ERR_PARAM); return len; }
	if (p[4] != 0) { set_state(state, AUDIO_ERR_FORMAT); return len; }
	if (!authenticate(p)) {
		rotate_nonce(); set_state(AUDIO_ARMED, AUDIO_ERR_AUTH); return len;
	}
	if (!subscribed) { set_state(AUDIO_ARMED, AUDIO_ERR_NO_SUB); return len; }
	if (!link_claim(LINK_OWNER_AUDIO)) { set_state(AUDIO_ARMED, AUDIO_ERR_BUSY); return len; }

	duration_ds = sys_get_le16(&p[1]); format = p[4];
	gain_db = CLAMP((int8_t)p[5], -20, 20);
	gain_factor = powf(10.0f, (float)gain_db / 20.0f);
	bytes_sent = dropped_bytes = running_crc = clipped = samples = 0;
	started_ms = k_uptime_get();
	uint32_t requested_ms = duration_ds ? (uint32_t)duration_ds * 100U :
					 HIVE_AUDIO_MAX_SECONDS * 1000U;
	deadline_ms = started_ms + MIN(requested_ms,
				       HIVE_AUDIO_MAX_SECONDS * 1000U);
	atomic_clear(&stopping); atomic_clear(&capture_done);
	atomic_clear(&dropped_pending);
	ring_head = ring_tail = ring_count = ring_offset = 0;
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
	subscribed = false; started_ms = 0; rotate_nonce(); set_state(AUDIO_ARMED, 0);
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

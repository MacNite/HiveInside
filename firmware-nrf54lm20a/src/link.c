/* The beacon, OTA and audio service share one peripheral connection. Keeping
 * its lifetime here prevents two services from restarting advertising twice. */
#include "link.h"

#include "audio.h"
#include "beacon.h"
#include "hive_config.h"
#include "ota.h"
#include "watchdog.h"

#include <zephyr/bluetooth/hci.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

static struct bt_conn *active_conn;
static enum link_owner owner;
/* Bumped on every connection, so a worker that outlives its connection can
 * tell that link_conn() now belongs to somebody else. */
static uint32_t generation;
static struct k_spinlock state_lock;
K_SEM_DEFINE(session_gate, 1, 1);

static void arm_timeout_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(arm_timeout_work, arm_timeout_handler);

struct bt_conn *link_conn(void)
{
	return active_conn;
}

uint32_t link_generation(void)
{
	k_spinlock_key_t key = k_spin_lock(&state_lock);
	uint32_t result = generation;
	k_spin_unlock(&state_lock, key);
	return result;
}

struct bt_conn *link_conn_get(uint32_t expected)
{
	struct bt_conn *conn = NULL;
	k_spinlock_key_t key = k_spin_lock(&state_lock);

	if (active_conn != NULL && generation == expected) {
		conn = bt_conn_ref(active_conn);
	}
	k_spin_unlock(&state_lock, key);
	return conn;
}

bool link_claim(enum link_owner requested)
{
	bool claimed = false;

	/* GATT callbacks must never wait for a sensor capture.  Taking the gate
	 * without waiting makes START/BEGIN report busy while a cycle is in flight. */
	if (k_sem_take(&session_gate, K_NO_WAIT) != 0) {
		return false;
	}
	k_spinlock_key_t key = k_spin_lock(&state_lock);

	if (active_conn != NULL && owner == LINK_OWNER_NONE &&
	    requested != LINK_OWNER_NONE) {
		owner = requested;
		claimed = true;
	}
	k_spin_unlock(&state_lock, key);
	if (!claimed) {
		k_sem_give(&session_gate);
	}
	if (claimed) {
		(void)k_work_cancel_delayable(&arm_timeout_work);
	}
	return claimed;
}

void link_release(enum link_owner released)
{
	bool arm = false;
	bool released_gate = false;
	k_spinlock_key_t key = k_spin_lock(&state_lock);
	if (owner == released) {
		owner = LINK_OWNER_NONE;
		arm = active_conn != NULL;
		released_gate = true;
	}
	k_spin_unlock(&state_lock, key);
	/* Only the successful owner transition above owns this semaphore. */
	if (released_gate) {
		k_sem_give(&session_gate);
	}
	/* Releasing a completed service does not make an otherwise idle
	 * connection safe to retain forever. Give the central a fresh arm window
	 * in which to begin another bounded session. */
	if (arm) {
		(void)k_work_reschedule(&arm_timeout_work,
					K_MSEC(HIVE_LINK_ARM_TIMEOUT_MS));
	}
}

bool link_measurement_begin(void)
{
	/* A session can last minutes, so keep the watchdog alive rather than
	 * blocking the main thread indefinitely. */
	while (k_sem_take(&session_gate,
			  K_MSEC(HIVE_WDT_FEED_INTERVAL_MS)) != 0) {
		hive_watchdog_feed();
	}

	/* A connection may have arrived while main was waiting for the gate. */
	if (link_is_busy()) {
		k_sem_give(&session_gate);
		return false;
	}
	return true;
}

void link_measurement_end(void)
{
	k_sem_give(&session_gate);
}

bool link_is_owner(enum link_owner queried)
{
	k_spinlock_key_t key = k_spin_lock(&state_lock);
	bool result = owner == queried;
	k_spin_unlock(&state_lock, key);
	return result;
}

bool link_is_busy(void)
{
	k_spinlock_key_t key = k_spin_lock(&state_lock);
	bool result = active_conn != NULL || owner != LINK_OWNER_NONE;
	k_spin_unlock(&state_lock, key);
	return result;
}

static void log_current_params(struct bt_conn *conn)
{
	struct bt_conn_info info;
	if (bt_conn_get_info(conn, &info) == 0 && info.type == BT_CONN_TYPE_LE) {
		/* interval_us avoids Zephyr's deprecated, lossy interval field. */
		printk("[LINK] interval=%u us latency=%u timeout=%u ms\n",
		       (unsigned)info.le.interval_us, (unsigned)info.le.latency,
		       (unsigned)info.le.timeout * 10U);
	}
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	struct bt_le_conn_param param = {
		.interval_min = 24, .interval_max = 40, .latency = 0,
		.timeout = 200,
	};
	if (err != 0U) {
		return;
	}
	k_spinlock_key_t key = k_spin_lock(&state_lock);
	active_conn = bt_conn_ref(conn);
	generation++;
	/* owner is deliberately left alone: a worker from the previous
	 * connection may still be shutting down and holds the session gate
	 * until it calls link_release(). A START or BEGIN on this connection
	 * reports busy until then, and so does the measurement loop. */
	k_spin_unlock(&state_lock, key);
	beacon_connected();
	ota_link_connected();
	audio_link_connected();
	/* Preserve OTA's established 30--50 ms baseline. Audio replaces it with
	 * its measured 15 ms interval only after an authenticated START. */
	(void)bt_conn_le_param_update(conn, &param);
	log_current_params(conn);
	(void)k_work_reschedule(&arm_timeout_work,
				K_MSEC(HIVE_LINK_ARM_TIMEOUT_MS));
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	struct bt_conn *old;
	k_spinlock_key_t key;

	ARG_UNUSED(conn);
	(void)k_work_cancel_delayable(&arm_timeout_work);
	key = k_spin_lock(&state_lock);
	old = active_conn;
	active_conn = NULL;
	k_spin_unlock(&state_lock, key);
	/* Ownership is not taken away here. Each service releases its own
	 * claim once its work has actually stopped: OTA immediately, audio and
	 * the throughput spike when their worker threads have finished with
	 * the PDM controller, the sensor rail and the ring. Releasing the
	 * gate earlier let the measurement loop and a new connection's session
	 * start while the old one was still running. */
	ota_link_disconnected();
	audio_link_disconnected();
	if (old != NULL) {
		bt_conn_unref(old);
	}
	printk("[LINK] disconnected (0x%02x)\n", reason);
	beacon_disconnected();
}

static void param_updated(struct bt_conn *conn, uint16_t interval,
			  uint16_t latency, uint16_t timeout)
{
	ARG_UNUSED(conn);
	printk("[LINK] interval now %u units latency=%u timeout=%u ms\n",
	       (unsigned)interval, (unsigned)latency, (unsigned)timeout * 10U);
}

#if defined(CONFIG_BT_USER_PHY_UPDATE)
static void phy_updated(struct bt_conn *conn, struct bt_conn_le_phy_info *info)
{
	ARG_UNUSED(conn);
	printk("[LINK] PHY now tx=%u rx=%u\n", (unsigned)info->tx_phy,
	       (unsigned)info->rx_phy);
}
#endif

#if defined(CONFIG_BT_USER_DATA_LEN_UPDATE)
static void data_len_updated(struct bt_conn *conn,
			     struct bt_conn_le_data_len_info *info)
{
	ARG_UNUSED(conn);
	printk("[LINK] data length tx=%u B/%u us rx=%u B/%u us\n",
	       (unsigned)info->tx_max_len, (unsigned)info->tx_max_time,
	       (unsigned)info->rx_max_len, (unsigned)info->rx_max_time);
}
#endif

BT_CONN_CB_DEFINE(link_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.le_param_updated = param_updated,
#if defined(CONFIG_BT_USER_PHY_UPDATE)
	.le_phy_updated = phy_updated,
#endif
#if defined(CONFIG_BT_USER_DATA_LEN_UPDATE)
	.le_data_len_updated = data_len_updated,
#endif
};

void link_disconnect(void)
{
	struct bt_conn *conn = link_conn_get(link_generation());

	if (conn != NULL) {
		(void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		bt_conn_unref(conn);
	}
}

static void arm_timeout_handler(struct k_work *work)
{
	struct bt_conn *conn = NULL;
	k_spinlock_key_t key = k_spin_lock(&state_lock);

	ARG_UNUSED(work);
	if (active_conn != NULL && owner == LINK_OWNER_NONE) {
		conn = bt_conn_ref(active_conn);
	}
	k_spin_unlock(&state_lock, key);
	if (conn != NULL) {
		printk("[LINK] connection not claimed; disconnecting\n");
		(void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		bt_conn_unref(conn);
	}
}

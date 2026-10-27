/* Host stand-ins for the Zephyr APIs the firmware modules under test use.
 *
 * Only what link.c, ota.c, audio.c and power.c touch is here, with the same
 * names and signatures, so the real sources compile unchanged. Kernel objects
 * are backed by pthreads and run concurrently for real; Bluetooth, flash,
 * MCUboot and the regulators are fakes that tests drive and inspect through
 * shim_test.h. Delayable work never fires on its own: a test advances the
 * clock with shim_advance_ms(), which runs whatever has come due.
 */
#pragma once

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

/* ── sys/util.h ─────────────────────────────────────────────────────────── */

#define BUILD_ASSERT(cond, ...) _Static_assert(cond, "" __VA_ARGS__)
#define ARG_UNUSED(x) (void)(x)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define BIT(n) (1UL << (n))
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif
#define CLAMP(v, lo, hi) MIN(MAX((v), (lo)), (hi))
#define __aligned(n) __attribute__((aligned(n)))

/* ── printk ─────────────────────────────────────────────────────────────── */

void printk(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ── byteorder ──────────────────────────────────────────────────────────── */

static inline void sys_put_le16(uint16_t v, uint8_t *p)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static inline void sys_put_le32(uint32_t v, uint8_t *p)
{
	sys_put_le16((uint16_t)v, p); sys_put_le16((uint16_t)(v >> 16), p + 2);
}
static inline uint16_t sys_get_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}
static inline uint32_t sys_get_le32(const uint8_t *p)
{
	return sys_get_le16(p) | ((uint32_t)sys_get_le16(p + 2) << 16);
}

/* ── crc ────────────────────────────────────────────────────────────────── */

uint32_t crc32_ieee_update(uint32_t crc, const uint8_t *data, size_t len);
uint32_t crc32_ieee(const uint8_t *data, size_t len);

/* ── kernel: time ───────────────────────────────────────────────────────── */

typedef struct { int64_t ms; } k_timeout_t;
#define K_FOREVER ((k_timeout_t){ -1 })
#define K_NO_WAIT ((k_timeout_t){ 0 })
#define K_MSEC(m) ((k_timeout_t){ (int64_t)(m) })

int64_t k_uptime_get(void);
int32_t k_msleep(int32_t ms);
int32_t k_sleep(k_timeout_t t);
void k_yield(void);

/* ── kernel: synchronisation ────────────────────────────────────────────── */

struct k_sem {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	unsigned int count, limit;
};
#define K_SEM_DEFINE(name, init, max)                                     \
	struct k_sem name = { PTHREAD_MUTEX_INITIALIZER,                  \
			      PTHREAD_COND_INITIALIZER, (init), (max) }
int k_sem_take(struct k_sem *sem, k_timeout_t timeout);
void k_sem_give(struct k_sem *sem);
void k_sem_reset(struct k_sem *sem);
unsigned int k_sem_count_get(struct k_sem *sem);

struct k_mutex { pthread_mutex_t lock; };
#define K_MUTEX_DEFINE(name) \
	struct k_mutex name = { PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP }
int k_mutex_lock(struct k_mutex *m, k_timeout_t timeout);
int k_mutex_unlock(struct k_mutex *m);

/* Zero-initialised like Zephyr's: a zeroed pthread mutex is a valid
 * default mutex on glibc. */
struct k_spinlock { pthread_mutex_t lock; };
typedef struct { int unused; } k_spinlock_key_t;
k_spinlock_key_t k_spin_lock(struct k_spinlock *l);
void k_spin_unlock(struct k_spinlock *l, k_spinlock_key_t key);

typedef long atomic_t;
static inline long atomic_get(const atomic_t *a)
{
	return __atomic_load_n(a, __ATOMIC_SEQ_CST);
}
static inline long atomic_set(atomic_t *a, long v)
{
	return __atomic_exchange_n(a, v, __ATOMIC_SEQ_CST);
}
static inline long atomic_clear(atomic_t *a) { return atomic_set(a, 0); }
static inline bool atomic_cas(atomic_t *a, long old, long new_value)
{
	return __atomic_compare_exchange_n(a, &old, new_value, false,
					   __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

/* ── kernel: work and threads ───────────────────────────────────────────── */

struct k_work;
typedef void (*k_work_handler_t)(struct k_work *work);
struct k_work { k_work_handler_t handler; };
struct k_work_delayable {
	struct k_work work;
	bool pending, registered;
	int64_t due_ms;
	struct k_work_delayable *next;
};
#define K_WORK_DELAYABLE_DEFINE(name, fn) \
	struct k_work_delayable name = { .work = { .handler = (fn) } }
int k_work_reschedule(struct k_work_delayable *dwork, k_timeout_t delay);
int k_work_cancel_delayable(struct k_work_delayable *dwork);

void shim_thread_spawn(const char *name, void (*fn)(void *, void *, void *));
#define K_THREAD_DEFINE(name, stack, fn, p1, p2, p3, prio, opts, delay)   \
	__attribute__((constructor)) static void name##_shim_start(void)   \
	{                                                                   \
		shim_thread_spawn(#name, fn);                               \
	}

/* ── device / devicetree / regulator (power.c) ──────────────────────────── */

struct device { const char *name; bool ready; };
static inline bool device_is_ready(const struct device *dev) { return dev->ready; }

/* Resolve every devicetree probe in power.c to its real branch, with the two
 * regulators as named fake devices. */
#define DT_HAS_COMPAT_STATUS_OKAY(compat) 1
#define DT_NODE_EXISTS(node) 1
#define DT_COMPAT_GET_ANY_STATUS_OKAY(compat) compat
#define DT_CHILD(parent, child) child
#define DT_NODELABEL(label) label
#define SHIM_DEVICE_(node) (&shim_dev_##node)
#define DEVICE_DT_GET(node) SHIM_DEVICE_(node)
extern struct device shim_dev_ldo1, shim_dev_power_en;

int regulator_enable(const struct device *dev);
int regulator_disable(const struct device *dev);
int regulator_set_voltage(const struct device *dev, int32_t min_uv, int32_t max_uv);

/* ── Bluetooth ──────────────────────────────────────────────────────────── */

struct bt_conn {
	int refs;
	uint16_t mtu;
	bool connected;
};
struct bt_conn *bt_conn_ref(struct bt_conn *conn);
void bt_conn_unref(struct bt_conn *conn);
int bt_conn_disconnect(struct bt_conn *conn, uint8_t reason);

struct bt_le_conn_param {
	uint16_t interval_min, interval_max, latency, timeout;
};
#define BT_LE_DATA_LEN_PARAM_MAX NULL
#define BT_CONN_LE_PHY_PARAM_2M NULL
int bt_conn_le_param_update(struct bt_conn *conn, const struct bt_le_conn_param *p);
int bt_conn_le_data_len_update(struct bt_conn *conn, const void *p);
int bt_conn_le_phy_update(struct bt_conn *conn, const void *p);

enum { BT_CONN_TYPE_LE = 1 };
struct bt_conn_info {
	int type;
	struct { uint32_t interval_us; uint16_t latency, timeout; } le;
};
int bt_conn_get_info(const struct bt_conn *conn, struct bt_conn_info *info);

struct bt_conn_cb {
	void (*connected)(struct bt_conn *conn, uint8_t err);
	void (*disconnected)(struct bt_conn *conn, uint8_t reason);
	void (*le_param_updated)(struct bt_conn *conn, uint16_t interval,
				 uint16_t latency, uint16_t timeout);
};
#define BT_CONN_CB_DEFINE(name) const struct bt_conn_cb name

#define BT_HCI_ERR_REMOTE_USER_TERM_CONN 0x13

struct bt_uuid { uint8_t type; };
struct bt_uuid_128 { struct bt_uuid uuid; uint8_t val[16]; };
#define BT_UUID_128_ENCODE(w32, w1, w2, w3, w48) 0
#define BT_UUID_INIT_128(...) { .uuid = { 2 } }

#define BT_ATT_ERR_INVALID_OFFSET 0x07
#define BT_ATT_ERR_INVALID_ATTRIBUTE_LEN 0x0d
#define BT_ATT_ERR_UNLIKELY 0x0e
#define BT_ATT_ERR_VALUE_NOT_ALLOWED 0x13
#define BT_GATT_ERR(att_err) (-(att_err))

#define BT_GATT_CHRC_READ 0x02
#define BT_GATT_CHRC_WRITE_WITHOUT_RESP 0x04
#define BT_GATT_CHRC_WRITE 0x08
#define BT_GATT_CHRC_NOTIFY 0x10
#define BT_GATT_PERM_NONE 0
#define BT_GATT_PERM_READ 1
#define BT_GATT_PERM_WRITE 2
#define BT_GATT_CCC_NOTIFY 1

struct bt_gatt_attr;
typedef ssize_t (*bt_gatt_attr_read_func_t)(struct bt_conn *, const struct bt_gatt_attr *,
					     void *, uint16_t, uint16_t);
typedef ssize_t (*bt_gatt_attr_write_func_t)(struct bt_conn *, const struct bt_gatt_attr *,
					      const void *, uint16_t, uint16_t, uint8_t);
typedef void (*bt_gatt_ccc_changed_t)(const struct bt_gatt_attr *, uint16_t);
struct bt_gatt_attr {
	const struct bt_uuid *uuid;
	bt_gatt_attr_read_func_t read;
	bt_gatt_attr_write_func_t write;
	bt_gatt_ccc_changed_t ccc_changed;
	uint16_t perm;
};
struct bt_gatt_service_static {
	struct bt_gatt_attr *attrs;
	size_t attr_count;
};
/* Same attribute layout as Zephyr: a characteristic is a declaration
 * attribute followed by its value attribute. */
#define BT_GATT_PRIMARY_SERVICE(u) { .uuid = (u) }
#define BT_GATT_CHARACTERISTIC(u, props, perm_, rd, wr, ud) \
	{ .uuid = (u) }, { .uuid = (u), .read = (rd), .write = (wr), .perm = (perm_) }
#define BT_GATT_CCC(changed, perm_) { .ccc_changed = (changed), .perm = (perm_) }
#define BT_GATT_SERVICE_DEFINE(name, ...)                                  \
	struct bt_gatt_attr name##_attrs[] = { __VA_ARGS__ };              \
	const struct bt_gatt_service_static name = { name##_attrs,         \
						     ARRAY_SIZE(name##_attrs) }

typedef void (*bt_gatt_complete_func_t)(struct bt_conn *conn, void *user_data);
struct bt_gatt_notify_params {
	const struct bt_gatt_attr *attr;
	const void *data;
	uint16_t len;
	bt_gatt_complete_func_t func;
	void *user_data;
};
int bt_gatt_notify(struct bt_conn *conn, const struct bt_gatt_attr *attr,
		   const void *data, uint16_t len);
int bt_gatt_notify_cb(struct bt_conn *conn, struct bt_gatt_notify_params *params);
uint16_t bt_gatt_get_mtu(struct bt_conn *conn);
ssize_t bt_gatt_attr_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  void *buf, uint16_t buf_len, uint16_t offset,
			  const void *value, uint16_t value_len);
int bt_rand(void *buf, size_t len);

/* ── flash_img / flash_map / MCUboot / reboot (ota.c) ───────────────────── */

struct flash_img_context { size_t written, buffered; };
int flash_img_init(struct flash_img_context *ctx);
int flash_img_buffered_write(struct flash_img_context *ctx, const uint8_t *data,
			     size_t len, bool flush);
size_t flash_img_bytes_written(struct flash_img_context *ctx);

struct flash_area { uint32_t fa_off; size_t fa_size; };
#define FIXED_PARTITION_ID(label) 1
int flash_area_open(uint8_t id, const struct flash_area **fa);
void flash_area_close(const struct flash_area *fa);

#define BOOT_UPGRADE_TEST 0
int boot_request_upgrade(int permanent);

#define SYS_REBOOT_COLD 1
void sys_reboot(int type);

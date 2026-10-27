/* Implementations behind zephyr_shim.h and shim_test.h. */
#include "shim_test.h"

#include <psa/crypto.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int shim_failures;

/* ── printk ─────────────────────────────────────────────────────────────── */

void printk(const char *fmt, ...)
{
	static int verbose = -1;
	va_list ap;

	if (verbose < 0) {
		verbose = getenv("SHIM_VERBOSE") != NULL;
	}
	if (!verbose) {
		return;
	}
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

/* ── crc32 (zlib-compatible, as Zephyr's crc32_ieee_update) ─────────────── */

uint32_t crc32_ieee_update(uint32_t crc, const uint8_t *data, size_t len)
{
	crc = ~crc;
	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int b = 0; b < 8; b++) {
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
		}
	}
	return ~crc;
}

uint32_t crc32_ieee(const uint8_t *data, size_t len)
{
	return crc32_ieee_update(0, data, len);
}

/* ── time ───────────────────────────────────────────────────────────────── */

static int64_t clock_offset_ms;

int64_t shim_real_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t k_uptime_get(void)
{
	/* Starts above zero, like a booted Zephyr, so started_ms > 0 holds. */
	return shim_real_ms() + __atomic_load_n(&clock_offset_ms, __ATOMIC_SEQ_CST) + 1000;
}

int32_t k_msleep(int32_t ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
	return 0;
}

int32_t k_sleep(k_timeout_t t)
{
	return k_msleep((int32_t)t.ms);
}

void k_yield(void)
{
	sched_yield();
}

/* ── semaphores, mutexes, spinlocks ─────────────────────────────────────── */

static void abs_deadline(struct timespec *ts, int64_t ms)
{
	clock_gettime(CLOCK_REALTIME, ts);
	ts->tv_sec += ms / 1000;
	ts->tv_nsec += (long)(ms % 1000) * 1000000L;
	if (ts->tv_nsec >= 1000000000L) {
		ts->tv_sec++;
		ts->tv_nsec -= 1000000000L;
	}
}

int k_sem_take(struct k_sem *sem, k_timeout_t timeout)
{
	struct timespec deadline;
	int rc = 0;

	if (timeout.ms > 0) {
		abs_deadline(&deadline, timeout.ms);
	}
	pthread_mutex_lock(&sem->lock);
	while (sem->count == 0) {
		if (timeout.ms == 0) {
			rc = -EBUSY;
			break;
		}
		if (timeout.ms < 0) {
			pthread_cond_wait(&sem->cond, &sem->lock);
		} else if (pthread_cond_timedwait(&sem->cond, &sem->lock,
						  &deadline) == ETIMEDOUT) {
			rc = -EAGAIN;
			break;
		}
	}
	if (rc == 0) {
		sem->count--;
	}
	pthread_mutex_unlock(&sem->lock);
	return rc;
}

void k_sem_give(struct k_sem *sem)
{
	pthread_mutex_lock(&sem->lock);
	if (sem->count < sem->limit) {
		sem->count++;
	}
	pthread_cond_signal(&sem->cond);
	pthread_mutex_unlock(&sem->lock);
}

void k_sem_reset(struct k_sem *sem)
{
	pthread_mutex_lock(&sem->lock);
	sem->count = 0;
	pthread_mutex_unlock(&sem->lock);
}

unsigned int k_sem_count_get(struct k_sem *sem)
{
	pthread_mutex_lock(&sem->lock);
	unsigned int count = sem->count;
	pthread_mutex_unlock(&sem->lock);
	return count;
}

int k_mutex_lock(struct k_mutex *m, k_timeout_t timeout)
{
	ARG_UNUSED(timeout);
	return pthread_mutex_lock(&m->lock) == 0 ? 0 : -EINVAL;
}

int k_mutex_unlock(struct k_mutex *m)
{
	return pthread_mutex_unlock(&m->lock) == 0 ? 0 : -EINVAL;
}

k_spinlock_key_t k_spin_lock(struct k_spinlock *l)
{
	pthread_mutex_lock(&l->lock);
	return (k_spinlock_key_t){ 0 };
}

void k_spin_unlock(struct k_spinlock *l, k_spinlock_key_t key)
{
	ARG_UNUSED(key);
	pthread_mutex_unlock(&l->lock);
}

/* ── delayable work ─────────────────────────────────────────────────────── */

static pthread_mutex_t work_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static struct k_work_delayable *work_list;

int k_work_reschedule(struct k_work_delayable *dwork, k_timeout_t delay)
{
	pthread_mutex_lock(&work_lock);
	if (!dwork->registered) {
		dwork->registered = true;
		dwork->next = work_list;
		work_list = dwork;
	}
	dwork->pending = true;
	dwork->due_ms = k_uptime_get() + (delay.ms < 0 ? 0 : delay.ms);
	pthread_mutex_unlock(&work_lock);
	return 1;
}

int k_work_cancel_delayable(struct k_work_delayable *dwork)
{
	pthread_mutex_lock(&work_lock);
	dwork->pending = false;
	pthread_mutex_unlock(&work_lock);
	return 0;
}

bool shim_work_pending(const struct k_work_delayable *dwork, int64_t *due_in_ms)
{
	pthread_mutex_lock(&work_lock);
	bool pending = dwork->pending;
	if (pending && due_in_ms != NULL) {
		*due_in_ms = dwork->due_ms - k_uptime_get();
	}
	pthread_mutex_unlock(&work_lock);
	return pending;
}

void shim_advance_ms(int64_t ms)
{
	__atomic_add_fetch(&clock_offset_ms, ms, __ATOMIC_SEQ_CST);
	for (;;) {
		struct k_work_delayable *next = NULL;
		int64_t now = k_uptime_get();

		pthread_mutex_lock(&work_lock);
		for (struct k_work_delayable *w = work_list; w; w = w->next) {
			if (w->pending && w->due_ms <= now &&
			    (next == NULL || w->due_ms < next->due_ms)) {
				next = w;
			}
		}
		if (next != NULL) {
			next->pending = false;
		}
		pthread_mutex_unlock(&work_lock);
		if (next == NULL) {
			return;
		}
		next->work.handler(&next->work);
	}
}

/* ── threads ────────────────────────────────────────────────────────────── */

struct thread_start { void (*fn)(void *, void *, void *); };

static void *thread_main(void *arg)
{
	struct thread_start start = *(struct thread_start *)arg;

	free(arg);
	start.fn(NULL, NULL, NULL);
	return NULL;
}

void shim_thread_spawn(const char *name, void (*fn)(void *, void *, void *))
{
	pthread_t thread;
	struct thread_start *start = malloc(sizeof(*start));

	start->fn = fn;
	if (pthread_create(&thread, NULL, thread_main, start) != 0) {
		fprintf(stderr, "cannot start thread %s\n", name);
		abort();
	}
	pthread_detach(thread);
}

/* ── Bluetooth fakes ────────────────────────────────────────────────────── */

#define MAX_SUBS 16
#define MAX_STATS 32
#define MAX_HELD 64

static pthread_mutex_t bt_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static struct { const struct bt_conn *conn; const struct bt_gatt_attr *attr; } subs[MAX_SUBS];
static struct {
	const struct bt_conn *conn;
	const struct bt_gatt_attr *attr;
	struct shim_notify_stats stats;
} stats[MAX_STATS];
static struct { bt_gatt_complete_func_t func; struct bt_conn *conn; void *user_data; } held[MAX_HELD];
static unsigned int held_count, after_disconnect, disconnect_requests;
static enum shim_completion completion_mode;
static int forced_notify_rc;

/* Never freed or reused, so a test may still inspect a dropped connection. */
static struct bt_conn conn_pool[256];
static unsigned int conn_pool_used;

struct bt_conn *shim_conn_new(uint16_t mtu)
{
	if (conn_pool_used == ARRAY_SIZE(conn_pool)) {
		abort();
	}
	struct bt_conn *conn = &conn_pool[conn_pool_used++];

	conn->refs = 1;
	conn->mtu = mtu;
	conn->connected = true;
	return conn;
}

void shim_conn_drop(struct bt_conn *conn)
{
	pthread_mutex_lock(&bt_lock);
	conn->connected = false;
	pthread_mutex_unlock(&bt_lock);
	bt_conn_unref(conn);
}

struct bt_conn *bt_conn_ref(struct bt_conn *conn)
{
	__atomic_add_fetch(&conn->refs, 1, __ATOMIC_SEQ_CST);
	return conn;
}

void bt_conn_unref(struct bt_conn *conn)
{
	int left = __atomic_sub_fetch(&conn->refs, 1, __ATOMIC_SEQ_CST);

	if (left < 0) {
		fprintf(stderr, "bt_conn_unref: reference count underflow\n");
		shim_failures++;
	}
}

int bt_conn_disconnect(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn); ARG_UNUSED(reason);
	__atomic_add_fetch(&disconnect_requests, 1, __ATOMIC_SEQ_CST);
	return 0;
}

int bt_conn_le_param_update(struct bt_conn *conn, const struct bt_le_conn_param *p)
{
	ARG_UNUSED(conn); ARG_UNUSED(p);
	return 0;
}

int bt_conn_le_data_len_update(struct bt_conn *conn, const void *p)
{
	ARG_UNUSED(conn); ARG_UNUSED(p);
	return 0;
}

int bt_conn_le_phy_update(struct bt_conn *conn, const void *p)
{
	ARG_UNUSED(conn); ARG_UNUSED(p);
	return 0;
}

int bt_conn_get_info(const struct bt_conn *conn, struct bt_conn_info *info)
{
	ARG_UNUSED(conn); ARG_UNUSED(info);
	return -ENOTSUP;
}

void shim_subscribe(struct bt_conn *conn, const struct bt_gatt_attr *attr, bool on)
{
	pthread_mutex_lock(&bt_lock);
	for (int i = 0; i < MAX_SUBS; i++) {
		if (subs[i].conn == conn && subs[i].attr == attr) {
			if (!on) {
				subs[i].conn = NULL;
				subs[i].attr = NULL;
			}
			pthread_mutex_unlock(&bt_lock);
			return;
		}
	}
	for (int i = 0; on && i < MAX_SUBS; i++) {
		if (subs[i].conn == NULL) {
			subs[i].conn = conn;
			subs[i].attr = attr;
			break;
		}
	}
	pthread_mutex_unlock(&bt_lock);
}

static bool subscribed_locked(const struct bt_conn *conn, const struct bt_gatt_attr *attr)
{
	for (int i = 0; i < MAX_SUBS; i++) {
		if (subs[i].conn == conn && subs[i].attr == attr) {
			return true;
		}
	}
	return false;
}

static struct shim_notify_stats *stats_slot_locked(const struct bt_conn *conn,
						   const struct bt_gatt_attr *attr)
{
	for (int i = 0; i < MAX_STATS; i++) {
		if (stats[i].conn == conn && stats[i].attr == attr) {
			return &stats[i].stats;
		}
	}
	for (int i = 0; i < MAX_STATS; i++) {
		if (stats[i].conn == NULL) {
			stats[i].conn = conn;
			stats[i].attr = attr;
			return &stats[i].stats;
		}
	}
	abort();
}

void shim_set_completion(enum shim_completion mode)
{
	pthread_mutex_lock(&bt_lock);
	completion_mode = mode;
	pthread_mutex_unlock(&bt_lock);
}

void shim_release_completions(void)
{
	pthread_mutex_lock(&bt_lock);
	unsigned int count = held_count;
	held_count = 0;
	typeof(held) copy;
	memcpy(copy, held, sizeof(held));
	pthread_mutex_unlock(&bt_lock);
	for (unsigned int i = 0; i < count; i++) {
		copy[i].func(copy[i].conn, copy[i].user_data);
	}
}

void shim_force_notify_result(int rc)
{
	pthread_mutex_lock(&bt_lock);
	forced_notify_rc = rc;
	pthread_mutex_unlock(&bt_lock);
}

static int notify(struct bt_conn *conn, const struct bt_gatt_attr *attr,
		  const void *data, uint16_t len, bt_gatt_complete_func_t func,
		  void *user_data)
{
	bool complete_now;

	if (conn == NULL) {
		/* Zephyr notifies every subscribed connection; the fakes only
		 * ever have one, so a NULL target is simply recorded nowhere. */
		return 0;
	}
	pthread_mutex_lock(&bt_lock);
	if (forced_notify_rc != 0) {
		int rc = forced_notify_rc;
		pthread_mutex_unlock(&bt_lock);
		return rc;
	}
	if (!conn->connected) {
		after_disconnect++;
		pthread_mutex_unlock(&bt_lock);
		return -ENOTCONN;
	}
	if (!subscribed_locked(conn, attr)) {
		pthread_mutex_unlock(&bt_lock);
		return -EINVAL;
	}
	struct shim_notify_stats *s = stats_slot_locked(conn, attr);
	s->count++;
	s->bytes += len;
	s->last_len = MIN(len, (uint16_t)sizeof(s->last));
	memcpy(s->last, data, s->last_len);
	complete_now = completion_mode == SHIM_COMPLETE_NOW;
	if (func != NULL && !complete_now) {
		if (held_count == MAX_HELD) {
			abort();
		}
		held[held_count].func = func;
		held[held_count].conn = conn;
		held[held_count].user_data = user_data;
		held_count++;
	}
	pthread_mutex_unlock(&bt_lock);
	if (func != NULL && complete_now) {
		func(conn, user_data);
	}
	return 0;
}

int bt_gatt_notify(struct bt_conn *conn, const struct bt_gatt_attr *attr,
		   const void *data, uint16_t len)
{
	return notify(conn, attr, data, len, NULL, NULL);
}

int bt_gatt_notify_cb(struct bt_conn *conn, struct bt_gatt_notify_params *params)
{
	return notify(conn, params->attr, params->data, params->len, params->func,
		      params->user_data);
}

uint16_t bt_gatt_get_mtu(struct bt_conn *conn)
{
	pthread_mutex_lock(&bt_lock);
	uint16_t mtu = conn->connected ? conn->mtu : 0;
	pthread_mutex_unlock(&bt_lock);
	return mtu;
}

ssize_t bt_gatt_attr_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  void *buf, uint16_t buf_len, uint16_t offset,
			  const void *value, uint16_t value_len)
{
	ARG_UNUSED(conn); ARG_UNUSED(attr);
	if (offset > value_len) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	uint16_t len = MIN(buf_len, (uint16_t)(value_len - offset));
	memcpy(buf, (const uint8_t *)value + offset, len);
	return len;
}

int bt_rand(void *buf, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		((uint8_t *)buf)[i] = (uint8_t)rand();
	}
	return 0;
}

struct shim_notify_stats shim_notify_stats(const struct bt_conn *conn,
					   const struct bt_gatt_attr *attr)
{
	struct shim_notify_stats total = { 0 };

	pthread_mutex_lock(&bt_lock);
	for (int i = 0; i < MAX_STATS; i++) {
		if (stats[i].attr == attr && stats[i].conn != NULL &&
		    (conn == NULL || stats[i].conn == conn)) {
			total.count += stats[i].stats.count;
			total.bytes += stats[i].stats.bytes;
			total.last_len = stats[i].stats.last_len;
			memcpy(total.last, stats[i].stats.last, sizeof(total.last));
		}
	}
	pthread_mutex_unlock(&bt_lock);
	return total;
}

unsigned int shim_notify_after_disconnect(void)
{
	pthread_mutex_lock(&bt_lock);
	unsigned int n = after_disconnect;
	pthread_mutex_unlock(&bt_lock);
	return n;
}

unsigned int shim_disconnect_requests(void)
{
	return __atomic_load_n(&disconnect_requests, __ATOMIC_SEQ_CST);
}

void shim_bt_reset(void)
{
	pthread_mutex_lock(&bt_lock);
	memset(subs, 0, sizeof(subs));
	memset(stats, 0, sizeof(stats));
	held_count = 0;
	after_disconnect = 0;
	completion_mode = SHIM_COMPLETE_NOW;
	forced_notify_rc = 0;
	__atomic_store_n(&disconnect_requests, 0, __ATOMIC_SEQ_CST);
	pthread_mutex_unlock(&bt_lock);
}

/* ── flash, MCUboot, reboot ─────────────────────────────────────────────── */

static unsigned int flash_fail_countdown;
static int flash_fail_rc;
static size_t flash_written;
static bool upgrade_pending;
static unsigned int reboots;
static const struct flash_area slot1 = { 0x80000, SHIM_SLOT_SIZE };
static struct { bool valid; struct mcuboot_img_sem_ver ver; } bank[2];

int flash_img_init(struct flash_img_context *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
	flash_written = 0;
	return 0;
}

int flash_img_buffered_write(struct flash_img_context *ctx, const uint8_t *data,
			     size_t len, bool flush)
{
	ARG_UNUSED(data); ARG_UNUSED(flush);
	if (flash_fail_countdown != 0 && --flash_fail_countdown == 0) {
		return flash_fail_rc;
	}
	ctx->written += len;
	flash_written += len;
	return 0;
}

size_t flash_img_bytes_written(struct flash_img_context *ctx)
{
	return ctx->written;
}

int flash_area_open(uint8_t id, const struct flash_area **fa)
{
	ARG_UNUSED(id);
	*fa = &slot1;
	return 0;
}

void flash_area_close(const struct flash_area *fa)
{
	ARG_UNUSED(fa);
}

int boot_request_upgrade(int permanent)
{
	ARG_UNUSED(permanent);
	upgrade_pending = true;
	return 0;
}

int boot_read_bank_header(uint8_t area_id, struct mcuboot_img_header *header,
			  size_t header_size)
{
	if (area_id > 1 || header_size < sizeof(*header)) {
		return -EINVAL;
	}
	if (!bank[area_id].valid) {
		return -EIO;
	}
	memset(header, 0, sizeof(*header));
	header->mcuboot_version = 1;
	header->h.v1.sem_ver = bank[area_id].ver;
	return 0;
}

void shim_set_image_version(int slot, bool valid, uint8_t major, uint8_t minor,
			    uint16_t revision)
{
	bank[slot].valid = valid;
	bank[slot].ver = (struct mcuboot_img_sem_ver){ major, minor, revision, 0 };
}

void sys_reboot(int type)
{
	ARG_UNUSED(type);
	reboots++;
}

void shim_flash_fail_write(unsigned int n, int rc)
{
	flash_fail_countdown = n;
	flash_fail_rc = rc;
}

size_t shim_flash_written(void) { return flash_written; }
bool shim_upgrade_pending(void) { return upgrade_pending; }
unsigned int shim_reboots(void) { return reboots; }

void shim_flash_reset(void)
{
	flash_fail_countdown = 0;
	flash_written = 0;
	upgrade_pending = false;
	reboots = 0;
	shim_set_image_version(0, true, 1, 0, 0);
	shim_set_image_version(1, true, 1, 0, 0);
}

/* ── regulators ─────────────────────────────────────────────────────────── */

struct device shim_dev_ldo1 = { "LDO1", true };
struct device shim_dev_power_en = { "power_en", true };

static struct reg_state {
	const struct device *dev;
	int refs;
	int fail_enable, fail_disable;
} regs[] = { { .dev = &shim_dev_ldo1 }, { .dev = &shim_dev_power_en } };

static struct reg_state *reg(const struct device *dev)
{
	for (size_t i = 0; i < ARRAY_SIZE(regs); i++) {
		if (regs[i].dev == dev) {
			return &regs[i];
		}
	}
	abort();
}

/* Zephyr's regulator core: a reference count, hardware switched at 0 <-> 1,
 * and a disable at zero is a no-op. */
int regulator_enable(const struct device *dev)
{
	struct reg_state *r = reg(dev);

	if (r->fail_enable != 0) {
		int rc = r->fail_enable;
		r->fail_enable = 0;
		return rc;
	}
	r->refs++;
	return 0;
}

int regulator_disable(const struct device *dev)
{
	struct reg_state *r = reg(dev);

	if (r->fail_disable != 0) {
		int rc = r->fail_disable;
		r->fail_disable = 0;
		return rc;
	}
	if (r->refs > 0) {
		r->refs--;
	}
	return 0;
}

int regulator_set_voltage(const struct device *dev, int32_t min_uv, int32_t max_uv)
{
	ARG_UNUSED(dev); ARG_UNUSED(min_uv); ARG_UNUSED(max_uv);
	return 0;
}

int shim_regulator_refs(const struct device *dev) { return reg(dev)->refs; }
bool shim_regulator_on(const struct device *dev) { return reg(dev)->refs > 0; }

void shim_regulator_fail_next_enable(const struct device *dev, int rc)
{
	reg(dev)->fail_enable = rc;
}

void shim_regulator_fail_next_disable(const struct device *dev, int rc)
{
	reg(dev)->fail_disable = rc;
}

void shim_regulator_reset(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(regs); i++) {
		regs[i].refs = 0;
		regs[i].fail_enable = regs[i].fail_disable = 0;
	}
}

/* ── PSA: HMAC-SHA256 ───────────────────────────────────────────────────── */

struct sha256 { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t used; };

static const uint32_t K256[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
	0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
	0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
	0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
	0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
	0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(struct sha256 *s, const uint8_t *p)
{
	uint32_t w[64], a, b, c, d, e, f, g, h;

	for (int i = 0; i < 16; i++) {
		w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
		       (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
	}
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
	e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
	for (int i = 0; i < 64; i++) {
		uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) +
			      ((e & f) ^ (~e & g)) + K256[i] + w[i];
		uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) +
			      ((a & b) ^ (a & c) ^ (b & c));
		h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
	}
	s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
	s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha256_init(struct sha256 *s)
{
	static const uint32_t iv[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
	};
	memcpy(s->h, iv, sizeof(iv));
	s->len = 0;
	s->used = 0;
}

static void sha256_update(struct sha256 *s, const uint8_t *p, size_t n)
{
	s->len += n;
	while (n > 0) {
		size_t take = MIN(n, 64 - s->used);
		memcpy(s->buf + s->used, p, take);
		s->used += take; p += take; n -= take;
		if (s->used == 64) {
			sha256_block(s, s->buf);
			s->used = 0;
		}
	}
}

static void sha256_final(struct sha256 *s, uint8_t out[32])
{
	uint64_t bits = s->len * 8;
	uint8_t pad = 0x80, zero = 0, len_be[8];

	sha256_update(s, &pad, 1);
	while (s->used != 56) {
		sha256_update(s, &zero, 1);
	}
	for (int i = 0; i < 8; i++) {
		len_be[i] = (uint8_t)(bits >> (56 - 8 * i));
	}
	sha256_update(s, len_be, 8);
	for (int i = 0; i < 8; i++) {
		out[4 * i] = (uint8_t)(s->h[i] >> 24);
		out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
		out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
		out[4 * i + 3] = (uint8_t)s->h[i];
	}
}

void shim_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg,
		      size_t msg_len, uint8_t out[32])
{
	uint8_t k[64] = { 0 }, ipad[64], opad[64], inner[32];
	struct sha256 s;

	if (key_len > 64) {
		sha256_init(&s);
		sha256_update(&s, key, key_len);
		sha256_final(&s, k);
	} else {
		memcpy(k, key, key_len);
	}
	for (int i = 0; i < 64; i++) {
		ipad[i] = k[i] ^ 0x36;
		opad[i] = k[i] ^ 0x5c;
	}
	sha256_init(&s);
	sha256_update(&s, ipad, 64);
	sha256_update(&s, msg, msg_len);
	sha256_final(&s, inner);
	sha256_init(&s);
	sha256_update(&s, opad, 64);
	sha256_update(&s, inner, 32);
	sha256_final(&s, out);
}

static uint8_t psa_key[64];
static size_t psa_key_len;

psa_status_t psa_crypto_init(void) { return PSA_SUCCESS; }
void psa_set_key_usage_flags(psa_key_attributes_t *a, uint32_t usage) { a->usage = usage; }
void psa_set_key_algorithm(psa_key_attributes_t *a, psa_algorithm_t alg) { a->alg = alg; }
void psa_set_key_type(psa_key_attributes_t *a, uint32_t type) { a->type = type; }
void psa_set_key_bits(psa_key_attributes_t *a, size_t bits) { a->bits = bits; }
void psa_reset_key_attributes(psa_key_attributes_t *a) { memset(a, 0, sizeof(*a)); }

psa_status_t psa_import_key(const psa_key_attributes_t *a, const uint8_t *data,
			    size_t len, psa_key_id_t *key)
{
	if (a->type != PSA_KEY_TYPE_HMAC || len > sizeof(psa_key)) {
		return PSA_ERROR_NOT_SUPPORTED;
	}
	memcpy(psa_key, data, len);
	psa_key_len = len;
	*key = 1;
	return PSA_SUCCESS;
}

psa_status_t psa_mac_compute(psa_key_id_t key, psa_algorithm_t alg,
			     const uint8_t *input, size_t input_len, uint8_t *mac,
			     size_t mac_size, size_t *mac_len)
{
	if (key != 1 || alg != PSA_ALG_HMAC(PSA_ALG_SHA_256)) {
		return PSA_ERROR_INVALID_HANDLE;
	}
	if (mac_size < 32) {
		return PSA_ERROR_BUFFER_TOO_SMALL;
	}
	shim_hmac_sha256(psa_key, psa_key_len, input, input_len, mac);
	*mac_len = 32;
	return PSA_SUCCESS;
}

psa_status_t psa_destroy_key(psa_key_id_t key)
{
	ARG_UNUSED(key);
	return PSA_SUCCESS;
}

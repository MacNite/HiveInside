/* Test-side controls for the fakes in zephyr_shim.h. */
#pragma once

#include "zephyr_shim.h"

/* ── clock and delayable work ───────────────────────────────────────────── */

/* Move k_uptime_get() forward and run every delayable work item that has
 * come due, in due order, on the calling thread. */
void shim_advance_ms(int64_t ms);
/* True if dwork is scheduled; *due_in_ms (optional) receives the delay. */
bool shim_work_pending(const struct k_work_delayable *dwork, int64_t *due_in_ms);

/* ── Bluetooth ──────────────────────────────────────────────────────────── */

/* A fresh connection object, as the host stack would hand to connected().
 * The stack's own reference is dropped by shim_conn_drop(). */
struct bt_conn *shim_conn_new(uint16_t mtu);
void shim_conn_drop(struct bt_conn *conn);

/* Notifications only reach subscribed (conn, attr) pairs, as with
 * CONFIG_BT_GATT_ENFORCE_SUBSCRIPTION. */
void shim_subscribe(struct bt_conn *conn, const struct bt_gatt_attr *attr, bool on);

enum shim_completion {
	SHIM_COMPLETE_NOW,   /* the controller confirms each notification at once */
	SHIM_COMPLETE_HOLD,  /* confirmations wait for shim_release_completions() */
};
void shim_set_completion(enum shim_completion mode);
void shim_release_completions(void);
/* Every bt_gatt_notify[_cb] returns rc while nonzero. */
void shim_force_notify_result(int rc);

struct shim_notify_stats {
	unsigned int count;      /* successful notifications */
	size_t bytes;            /* their payload bytes */
	uint8_t last[256];       /* the most recent payload */
	uint16_t last_len;
};
/* Totals for one (conn, attr) pair; conn NULL sums every connection. */
struct shim_notify_stats shim_notify_stats(const struct bt_conn *conn,
					   const struct bt_gatt_attr *attr);
/* Notify calls made on a connection that was already gone. */
unsigned int shim_notify_after_disconnect(void);
unsigned int shim_disconnect_requests(void);
void shim_bt_reset(void);

/* ── flash, MCUboot, reboot ─────────────────────────────────────────────── */

/* Make the n-th following flash_img_buffered_write() (1 = next) fail. */
void shim_flash_fail_write(unsigned int n, int rc);
size_t shim_flash_written(void);
bool shim_upgrade_pending(void);
/* The signed header version boot_read_bank_header() reports for slot 0
 * (running) or slot 1 (uploaded); valid false makes the read fail. Both
 * default to a valid 1.0.0. */
void shim_set_image_version(int slot, bool valid, uint8_t major, uint8_t minor,
			    uint16_t revision);
unsigned int shim_reboots(void);
void shim_flash_reset(void);
#define SHIM_SLOT_SIZE (64U * 1024U)

/* ── regulators ─────────────────────────────────────────────────────────── */

/* Reference count and hardware state of a fake regulator. */
int shim_regulator_refs(const struct device *dev);
bool shim_regulator_on(const struct device *dev);
/* The next regulator_enable() (or _disable()) on dev returns rc. */
void shim_regulator_fail_next_enable(const struct device *dev, int rc);
void shim_regulator_fail_next_disable(const struct device *dev, int rc);
void shim_regulator_reset(void);

/* ── tiny test runner ───────────────────────────────────────────────────── */

extern int shim_failures;
#define CHECK(cond)                                                          \
	do {                                                                 \
		if (!(cond)) {                                               \
			fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, \
				__LINE__, #cond);                            \
			shim_failures++;                                     \
		}                                                            \
	} while (0)
#define CHECK_EQ(a, b)                                                       \
	do {                                                                 \
		long long a_ = (long long)(a), b_ = (long long)(b);          \
		if (a_ != b_) {                                              \
			fprintf(stderr, "%s:%d: %s == %s failed (%lld != %lld)\n", \
				__FILE__, __LINE__, #a, #b, a_, b_);         \
			shim_failures++;                                     \
		}                                                            \
	} while (0)
#define RUN(test)                                                            \
	do {                                                                 \
		int before_ = shim_failures;                                 \
		test();                                                      \
		fprintf(stderr, "%s %s\n",                                   \
			shim_failures == before_ ? "PASS" : "FAIL", #test);  \
	} while (0)
/* Poll cond for up to timeout_ms of real time; true if it became true. */
#define WAIT_FOR(cond, timeout_ms)                                            \
	({                                                                    \
		int64_t until_ = shim_real_ms() + (timeout_ms);               \
		while (!(cond) && shim_real_ms() < until_) {                  \
			k_msleep(1);                                          \
		}                                                             \
		(bool)(cond);                                                 \
	})
int64_t shim_real_ms(void);

#include <stdio.h>

/*
 * watchdog.c — hardware watchdog, resolved from the devicetree.
 *
 * The node is looked up the same way mic.c finds the PDM controller: by alias
 * first, then by compatible, so a board-tree change that renames the instance
 * does not silently disable the protection. On the nRF54LM20A both wdt30 and
 * wdt31 ship disabled in the SoC devicetree, so app.overlay enables wdt31 —
 * wdt31 rather than wdt30 because wdt30 is compiled out of the SoC devicetree
 * entirely in a non-secure address map.
 */
#include "hive_config.h"
#include "watchdog.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/sys/printk.h>

#if DT_NODE_HAS_STATUS(DT_ALIAS(watchdog0), okay)
#define HIVE_WDT_NODE DT_ALIAS(watchdog0)
#elif DT_HAS_COMPAT_STATUS_OKAY(nordic_nrf_wdt)
#define HIVE_WDT_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(nordic_nrf_wdt)
#endif

#ifdef HIVE_WDT_NODE

BUILD_ASSERT(HIVE_WDT_FEED_INTERVAL_MS > 0U,
	     "HIVE_WDT_FEED_INTERVAL_MS must be non-zero: the idle loop divides "
	     "the measurement interval by it and would not terminate.");
BUILD_ASSERT(HIVE_WDT_FEED_INTERVAL_MS < HIVE_WDT_TIMEOUT_MS,
	     "HIVE_WDT_FEED_INTERVAL_MS must be shorter than the timeout, with "
	     "margin for the longest blocking capture.");

static const struct device *const wdt = DEVICE_DT_GET(HIVE_WDT_NODE);

/* Negative until a timeout channel is installed and the watchdog is running.
 * Every entry point checks this, so a failed setup degrades to the previous
 * behaviour (no watchdog) instead of resetting the node in a loop. */
static int wdt_channel = -1;

void hive_watchdog_init(void)
{
	if (!device_is_ready(wdt)) {
		printk("[WDT] watchdog device is not ready\n");
		return;
	}

	/* No callback: with the console compiled out of a deployment image there
	 * is nothing useful to do in the pre-reset window, and a NULL callback is
	 * the combination every driver supports for a plain RESET_SOC timeout.
	 * window.min stays 0 — this is a plain timeout, not a windowed watchdog,
	 * so an early feed must not itself be a fault. */
	struct wdt_timeout_cfg cfg = {
		.flags = WDT_FLAG_RESET_SOC,
		.window = {
			.min = 0U,
			.max = HIVE_WDT_TIMEOUT_MS,
		},
		.callback = NULL,
	};

	int channel = wdt_install_timeout(wdt, &cfg);

	if (channel < 0) {
		printk("[WDT] install failed (%d); continuing without a watchdog\n",
		       channel);
		return;
	}

	/* Pause while halted by a debugger so a breakpoint during bring-up does
	 * not reset the board. Deliberately not WDT_OPT_PAUSE_IN_SLEEP: the CPU is
	 * idle for almost the whole measurement interval, and a watchdog that
	 * stops counting there would never fire. Not every driver accepts the
	 * debug option, so fall back to no options rather than lose the
	 * watchdog. */
	int err = wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);

	if (err == -ENOTSUP) {
		err = wdt_setup(wdt, 0);
	}
	if (err != 0) {
		printk("[WDT] setup failed (%d); continuing without a watchdog\n",
		       err);
		return;
	}

	wdt_channel = channel;
	printk("[WDT] armed: %u ms timeout, fed every %u ms\n",
	       (unsigned)HIVE_WDT_TIMEOUT_MS, (unsigned)HIVE_WDT_FEED_INTERVAL_MS);
}

void hive_watchdog_feed(void)
{
	if (wdt_channel < 0) {
		return;
	}
	(void)wdt_feed(wdt, wdt_channel);
}

bool hive_watchdog_active(void)
{
	return wdt_channel >= 0;
}

#else /* no enabled watchdog node in the devicetree */

void hive_watchdog_init(void)
{
	printk("[WDT] no enabled watchdog devicetree node — running unprotected\n");
}

void hive_watchdog_feed(void)
{
}

bool hive_watchdog_active(void)
{
	return false;
}

#endif

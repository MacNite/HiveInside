/*
 * main.c — HiveInside XIAO nRF54LM20A Sense sensor readout (target 1).
 *
 * Bring up the sensor power rail, then every MEASURE_INTERVAL_MS read all four
 * sensors — SHT40 climate, LSM6DS3TR-C acceleration, PDM microphone level and
 * nPM1300 battery — and print the result to the USB serial console.
 *
 * "USB serial console": the on-board SAMD11 CMSIS-DAP debugger bridges the
 * SoC's uart20 to a USB CDC ACM port (/dev/ttyACM0) over the same USB-C cable
 * used for flashing, so printk() output appears on the host with no second
 * adapter. See docs/flashing.md. The console is a plain polled UART, so the
 * firmware boots and keeps sampling whether or not a terminal is attached.
 *
 * Alongside the plain readings it runs the same vibration and acoustic FFT band
 * analysis as the ESP32-C6 prototype, prints those bands, and broadcasts the
 * reduced measurement in HiveHub's BLE manufacturer-data format.
 */
#include "accel.h"
#include "battery.h"
#include "beacon.h"
#include "hive_config.h"
#include "measurement.h"
#include "mic.h"
#include "ota.h"
#include "power.h"
#include "sht40.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/dfu/mcuboot.h>

#define MEASUREMENT_LED_NODE DT_ALIAS(led0)

#if DT_NODE_HAS_STATUS(MEASUREMENT_LED_NODE, okay)
static const struct gpio_dt_spec measurement_led =
	GPIO_DT_SPEC_GET(MEASUREMENT_LED_NODE, gpios);
static bool measurement_led_ready;

static void measurement_led_init(void)
{
	if (!gpio_is_ready_dt(&measurement_led)) {
		printk("[LED] built-in LED controller is not ready\n");
		return;
	}
	int err = gpio_pin_configure_dt(&measurement_led, GPIO_OUTPUT_INACTIVE);

	if (err != 0) {
		printk("[LED] built-in LED setup failed (%d)\n", err);
		return;
	}
	measurement_led_ready = true;
}

static void measurement_led_blink(void)
{
	if (!measurement_led_ready) {
		return;
	}
	(void)gpio_pin_set_dt(&measurement_led, 1);
	k_msleep(MEASUREMENT_LED_BLINK_MS);
	(void)gpio_pin_set_dt(&measurement_led, 0);
}
#else
static void measurement_led_init(void)
{
	printk("[LED] no led0 devicetree alias; measurement blink disabled\n");
}

static void measurement_led_blink(void)
{
}
#endif

static void print_readout(const struct measurement *m)
{
	printk("---- HiveInside readout ----\n");

	if (m->sht_ok) {
		printk("  climate : %.2f C   %.1f %%RH\n", (double)m->temp_c,
		       (double)m->humidity_pct);
	} else {
		printk("  climate : n/a\n");
	}

	if (m->accel_ok) {
		printk("  accel   : x=%.1f y=%.1f z=%.1f mg  |a|=%.1f mg\n",
		       (double)m->accel_x_mg, (double)m->accel_y_mg,
		       (double)m->accel_z_mg, (double)m->accel_mag_mg);
		printk("  accel AC: rms=%.1f peak=%.1f mg  (%u@%uHz)\n",
		       (double)m->accel_rms_mg, (double)m->accel_peak_mg,
		       (unsigned)m->accel_sample_count,
		       (unsigned)m->accel_sample_rate_hz);
		printk("  vib FFT : swarm=%.2f fan=%.2f act=%.2f mg\n",
		       (double)m->accel_band_swarm_mg,
		       (double)m->accel_band_fanning_mg,
		       (double)m->accel_band_activity_mg);
	} else {
		printk("  accel   : n/a\n");
	}

	if (m->mic_ok) {
		printk("  sound   : rms=%.1f dBFS  peak=%.1f dBFS  (%u frames)\n",
		       (double)m->mic_rms_dbfs, (double)m->mic_peak_dbfs,
		       (unsigned)m->mic_frames);
		printk("  ac FFT  : sub=%.1f hum=%.1f pipe=%.1f stress=%.1f hi=%.1f dBFS\n",
		       (double)m->mic_band_sub_bass_dbfs,
		       (double)m->mic_band_hum_dbfs,
		       (double)m->mic_band_piping_dbfs,
		       (double)m->mic_band_stress_dbfs,
		       (double)m->mic_band_high_dbfs);
	} else {
		printk("  sound   : n/a\n");
	}

	if (m->battery_ok) {
		printk("  battery : %.3f V  ~%u%%\n", (double)m->battery_v,
		       (unsigned)m->battery_pct);
	} else {
		printk("  battery : n/a\n");
	}

	printk("----------------------------\n");
}

/* Rollback safety net for MCUboot test images. A healthy image confirms within
 * the first sensor cycle (seconds after boot); if confirmation has not happened
 * within this window the image is deemed unhealthy — radio never came up, the
 * main loop is wedged in a sensor driver, or the trailer write keeps failing —
 * and we reboot so MCUboot reverts to the previous image. The window is a
 * generous multiple of a normal first cycle yet well under MEASURE_INTERVAL_MS,
 * so recovery does not have to wait a full sampling period. */
#define OTA_CONFIRM_DEADLINE_MS 120000U

static void ota_confirm_deadline_expired(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	/* Only ever armed while running an unconfirmed test image with a staged
	 * rollback target, so a reset here reverts to the previous known image. */
	printk("[OTA] health deadline expired without confirmation; rebooting for rollback\n");
	sys_reboot(SYS_REBOOT_COLD);
}

K_TIMER_DEFINE(ota_confirm_deadline, ota_confirm_deadline_expired, NULL);

int main(void)
{
	bool confirmation_pending;
	bool confirmation_attempted = false;
	int beacon_err;

	printk("\n[HiveInside] %s fw %s | USB + HiveHub BLE beacon\n",
	       HIVEINSIDE_BOARD, HIVEINSIDE_FW_VERSION);

	power_init();
	measurement_led_init();
	beacon_err = beacon_init();
	ota_init();
	confirmation_pending = !boot_is_img_confirmed();
	/* Do not confirm a test image merely because main() was reached. Wait until
	 * one complete sensor cycle has run and Bluetooth has successfully published
	 * it; otherwise a regression in either path must remain eligible for MCUboot
	 * rollback on the next reset. */
	if (confirmation_pending && beacon_err != 0) {
		printk("[OTA] image left unconfirmed: Bluetooth init failed (%d)\n",
		       beacon_err);
	}
	/* Arm the rollback deadline only for a genuine test swap (a previous image
	 * is staged for revert). A directly SWD-flashed image reports swap type
	 * NONE and has no rollback target, so it is never armed and cannot boot
	 * loop; it still self-confirms through the normal path below. */
	if (confirmation_pending && mcuboot_swap_type() == BOOT_SWAP_TYPE_REVERT) {
		k_timer_start(&ota_confirm_deadline, K_MSEC(OTA_CONFIRM_DEADLINE_MS),
			      K_NO_WAIT);
	}

	while (true) {
		if (ota_is_active()) {
			k_msleep(100);
			continue;
		}
		struct measurement m = { 0 };

		sht40_read(&m);
		accel_read(&m);
		mic_read(&m);
		battery_read(&m);

		print_readout(&m);
		if (beacon_publish(&m) == 0) {
			measurement_led_blink();
			if (confirmation_pending && !confirmation_attempted &&
			    !ota_is_active()) {
				/* Make at most one trailer-write attempt per boot. A persistent
				 * flash error must not cause periodic writes for the lifetime of
				 * the device; leaving the image unconfirmed preserves rollback. */
				confirmation_attempted = true;
				int confirm_err = boot_write_img_confirmed();

				if (confirm_err == 0) {
					confirmation_pending = false;
					k_timer_stop(&ota_confirm_deadline);
					printk("[OTA] test image confirmed after first complete cycle\n");
				} else {
					printk("[OTA] image confirmation failed (%d); no retry before reset\n",
					       confirm_err);
				}
			}
		}

		k_msleep(MEASURE_INTERVAL_MS);
	}

	return 0;
}

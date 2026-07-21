/*
 * HiveInside — XIAO nRF54LM20A Sense: measurement beacon firmware.
 *
 * Cycle: read SHT40 (temp/humidity) -> capture IMU vibration FFT -> capture
 * PDM microphone acoustic FFT -> read battery (nPM1300) -> encode the
 * 26-byte beacon frame and keep advertising it (see beacon.c).
 *
 * Power model: there is deliberately no deep-sleep / wake-sync machinery
 * here. The nRF54 idles in System ON sleep at single-digit µA with the
 * advertiser running from the radio's own timer, so the ESP32-C6
 * prototype's deep-sleep rendezvous dance (and its pairing window, which
 * only existed to suppress that sleep) is unnecessary: the device is
 * *always* discoverable and HiveHub's passive scan needs no schedule.
 * Between cycles the main thread simply sleeps; the dominant energy cost is
 * the ~3 s sensor capture every MEASURE_INTERVAL_MS.
 *
 * Button (sw0, if the board defines one): short press runs an immediate
 * measurement — handy during installation to verify fresh values arrive.
 * The LED (led0) is lit during a capture and otherwise off.
 */
#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "accel.h"
#include "battery.h"
#include "beacon.h"
#include "hive_config.h"
#include "measurement.h"
#include "mic.h"
#include "power.h"
#include "sht40.h"

static K_SEM_DEFINE(measure_now, 0, 1);

/* ── LED (optional) ── */
#if DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static void led_init(void)
{
	if (gpio_is_ready_dt(&led)) {
		gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	}
}

static void led_set(bool on)
{
	if (gpio_is_ready_dt(&led)) {
		gpio_pin_set_dt(&led, on);
	}
}
#else
static void led_init(void) {}
static void led_set(bool on) { ARG_UNUSED(on); }
#endif

/* ── Button (optional): short press = measure right now ── */
#if DT_NODE_HAS_STATUS(DT_ALIAS(sw0), okay)
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static struct gpio_callback button_cb;

static void on_button(const struct device *dev, struct gpio_callback *cb,
		      uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	k_sem_give(&measure_now);
}

static void button_init(void)
{
	if (!gpio_is_ready_dt(&button)) {
		return;
	}
	gpio_pin_configure_dt(&button, GPIO_INPUT);
	gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
	gpio_init_callback(&button_cb, on_button, BIT(button.pin));
	gpio_add_callback(button.port, &button_cb);
}
#else
static void button_init(void) {}
#endif

static void run_cycle(struct measurement *m)
{
	led_set(true);
	memset(m, 0, sizeof(*m));

	sht40_read(m);
	accel_read(m);
	mic_read(m);
	battery_read(m);

	led_set(false);
}

int main(void)
{
	struct measurement m;

	printk("\n[HiveInside] %s fw %s | BLE beacon transport\n",
	       HIVEINSIDE_BOARD, HIVEINSIDE_FW_VERSION);

	power_init(); /* sensor rail up before anything probes */
	led_init();
	button_init();

	int err = bt_enable(NULL);

	if (err != 0) {
		printk("[BLE] bt_enable failed (%d)\n", err);
		return 0;
	}

	/* First cycle before advertising starts, so the very first frame on
	 * air already carries data instead of an all-absent payload. */
	run_cycle(&m);
	beacon_start(&m);

	while (true) {
		/* Sleep out the interval; a button press cuts it short. */
		if (k_sem_take(&measure_now, K_MSEC(MEASURE_INTERVAL_MS)) == 0) {
			printk("[BTN] manual measurement\n");
		}
		run_cycle(&m);
		beacon_update(&m);
	}

	return 0;
}

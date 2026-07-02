/*
 * HiveInside — XIAO nRF54LM20A Sense bring-up: Step 1, blinky.
 *
 * Goal of this step: prove the toolchain, the board target and the flashing
 * path all work end-to-end before any sensors or BLE are involved. If the
 * on-board user LED blinks, your nRF Connect SDK setup is good.
 *
 * Board target: xiao_nrf54lm20a/nrf54lm20a/cpuapp
 *
 * We drive the LED through the `led0` devicetree alias that the Seeed board
 * files provide. The LED's active-low wiring (XIAO user LEDs are active-low)
 * is encoded as a GPIO_ACTIVE_LOW flag in the board's devicetree, so we toggle
 * *logically* and let Zephyr translate to the right electrical level. That is
 * why this file has no board-specific pin numbers — it is portable by design.
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

#define BLINK_PERIOD_MS 500

/* Resolved at build time from devicetree: DT_ALIAS(led0) -> the board's LED. */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

int main(void)
{
	if (!gpio_is_ready_dt(&led)) {
		printk("LED gpio device not ready\n");
		return 0;
	}

	if (gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE) < 0) {
		printk("Failed to configure LED gpio\n");
		return 0;
	}

	printk("HiveInside nRF54LM20A blinky up — toggling led0 every %d ms\n",
	       BLINK_PERIOD_MS);

	while (1) {
		gpio_pin_toggle_dt(&led);
		k_msleep(BLINK_PERIOD_MS);
	}

	return 0;
}

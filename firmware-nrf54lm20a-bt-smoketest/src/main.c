#include <errno.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#if DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static void blink_once(void)
{
	if (gpio_is_ready_dt(&led)) {
		gpio_pin_set_dt(&led, 1);
		k_msleep(100);
		gpio_pin_set_dt(&led, 0);
	}
}
#else
static void blink_once(void) {}
#endif

int main(void)
{
	int err;

	printk("\n[BT-SMOKE] boot: blink then bt_enable()\n");

#if DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay)
	if (gpio_is_ready_dt(&led)) {
		err = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
		if (err != 0) {
			printk("[BT-SMOKE] LED init failed (%d)\n", err);
		}
	}
#endif

	blink_once();
	printk("[BT-SMOKE] calling bt_enable()\n");
	err = bt_enable(NULL);
	if (err != 0) {
		printk("[BT-SMOKE] bt_enable failed (%d)\n", err);
		return err;
	}

	printk("[BT-SMOKE] PASS: Bluetooth enabled\n");
	while (true) {
		blink_once();
		k_sleep(K_SECONDS(1));
	}
}

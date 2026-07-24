#include <errno.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#if DT_HAS_COMPAT_STATUS_OKAY(nordic_npm1300_regulator) &&                     \
	DT_NODE_EXISTS(                                                        \
		DT_CHILD(DT_COMPAT_GET_ANY_STATUS_OKAY(nordic_npm1300_regulator), \
			 ldo1))
#define LDO1_NODE \
	DT_CHILD(DT_COMPAT_GET_ANY_STATUS_OKAY(nordic_npm1300_regulator), ldo1)
static const struct device *const ldo1 = DEVICE_DT_GET(LDO1_NODE);
#endif

#if DT_HAS_COMPAT_STATUS_OKAY(nordic_npm1300_charger)
static const struct device *const charger =
	DEVICE_DT_GET(DT_COMPAT_GET_ANY_STATUS_OKAY(nordic_npm1300_charger));
#endif

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

static void npm1300_test(void)
{
#if DT_HAS_COMPAT_STATUS_OKAY(nordic_npm1300_regulator) &&                     \
	DT_NODE_EXISTS(                                                        \
		DT_CHILD(DT_COMPAT_GET_ANY_STATUS_OKAY(nordic_npm1300_regulator), \
			 ldo1))
	int err;

	if (!device_is_ready(ldo1)) {
		printk("[PMIC] nPM1300 LDO1 not ready\n");
	} else {
		err = regulator_set_voltage(ldo1, 3300000, 3300000);
		printk("[PMIC] LDO1 set 3.3V: %d\n", err);

		if (!regulator_is_enabled(ldo1)) {
			err = regulator_enable(ldo1);
			printk("[PMIC] LDO1 enable: %d\n", err);
		}

		printk("[PMIC] LDO1 enabled: %s\n",
		       regulator_is_enabled(ldo1) ? "yes" : "no");
	}
#else
	printk("[PMIC] nPM1300 LDO1 node not present\n");
#endif

#if DT_HAS_COMPAT_STATUS_OKAY(nordic_npm1300_charger)
	if (!device_is_ready(charger)) {
		printk("[PMIC] nPM1300 charger not ready\n");
		return;
	}

	struct sensor_value voltage;
	int gauge_err = sensor_sample_fetch(charger);

	if (gauge_err == 0) {
		gauge_err = sensor_channel_get(charger, SENSOR_CHAN_GAUGE_VOLTAGE,
					       &voltage);
	}

	if (gauge_err != 0) {
		printk("[PMIC] fuel-gauge read failed: %d\n", gauge_err);
		return;
	}

	printk("[PMIC] battery gauge: %d.%06d V\n", voltage.val1,
	       voltage.val2 < 0 ? -voltage.val2 : voltage.val2);
#else
	printk("[PMIC] nPM1300 charger node not present\n");
#endif
}

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
	printk("[BT-SMOKE] testing nPM1300\n");
	npm1300_test();
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

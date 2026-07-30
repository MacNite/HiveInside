/* power.c — see power.h. */
#include "power.h"

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

/* The nPM1300 regulator children are named nodes (LDO1, BUCK1, ... in the
 * devicetree) under the node with compatible `nordic,npm1300-regulator`, so
 * LDO1 is resolved structurally — no board-specific label needed.
 *
 * NOTE: DT_CHILD()'s child argument must be the lowercase-and-underscores
 * form of the node name (`ldo1`), even though the DTS spells the node `LDO1`.
 * Zephyr lowercases node names when generating devicetree identifiers, so
 * passing `LDO1` here makes DT_NODE_EXISTS() false and silently compiles the
 * no-op stub below. */
#if DT_HAS_COMPAT_STATUS_OKAY(nordic_npm1300_regulator) &&                     \
	DT_NODE_EXISTS(                                                        \
		DT_CHILD(DT_COMPAT_GET_ANY_STATUS_OKAY(nordic_npm1300_regulator), \
			 ldo1))

#include <zephyr/drivers/regulator.h>

#define LDO1_NODE \
	DT_CHILD(DT_COMPAT_GET_ANY_STATUS_OKAY(nordic_npm1300_regulator), ldo1)

static const struct device *const ldo1 = DEVICE_DT_GET(LDO1_NODE);
static bool sensor_rail_enabled;

/* P1.12 gates the complete Sense-board sensor supply.  LDO1 being enabled is
 * not sufficient when this upstream fixed regulator is off.  Seeed's DMIC
 * reference application explicitly enables both regulators in this order. */
#if DT_NODE_EXISTS(DT_NODELABEL(power_en))
static const struct device *const power_en =
	DEVICE_DT_GET(DT_NODELABEL(power_en));
#endif

static int enable_regulator(const struct device *dev, const char *name)
{
	if (!device_is_ready(dev)) {
		printk("[PWR] %s regulator not ready\n", name);
		return -ENODEV;
	}

	int err = regulator_enable(dev);

	/* Zephyr regulators may report an already-enabled boot-on rail either as
	 * success or -EALREADY, depending on the driver. */
	if (err != 0 && err != -EALREADY) {
		printk("[PWR] %s enable failed (%d)\n", name, err);
		return err;
	}

	return 0;
}

void power_init(void)
{
	/* The board DTS marks this regulator boot-on, but enabling it explicitly
	 * also works with older Seeed board definitions and mirrors their known-
	 * good microphone example. */
#if DT_NODE_EXISTS(DT_NODELABEL(power_en))
	if (enable_regulator(power_en, "sensor power gate") != 0) {
		return;
	}
#endif

	if (!device_is_ready(ldo1)) {
		printk("[PWR] nPM1300 LDO1 not ready — sensor rail unmanaged\n");
		return;
	}

	int err = regulator_set_voltage(ldo1, 3300000, 3300000);

	if (err != 0) {
		printk("[PWR] LDO1 set 3.3V failed (%d)\n", err);
	}
	if (enable_regulator(ldo1, "LDO1") != 0) {
		return;
	}
	sensor_rail_enabled = true;
	/* The IMU and microphone share this rail. Give both parts time to
	 * leave reset before their first I2C/PDM transaction. */
	k_msleep(20);
	printk("[PWR] nPM1300 LDO1 at 3.3V (IMU + mic rail)\n");
}

int power_sensor_rail_enable(void)
{
	if (sensor_rail_enabled) {
		return 0;
	}

	int err = enable_regulator(ldo1, "LDO1");

	if (err != 0) {
		return err;
	}
	sensor_rail_enabled = true;
	/* The IMU and microphone need time to leave reset before use. */
	k_msleep(20);
	return 0;
}

int power_sensor_rail_disable(void)
{
	if (!sensor_rail_enabled) {
		return 0;
	}

	int err = regulator_disable(ldo1);

	if (err != 0 && err != -EALREADY) {
		printk("[PWR] LDO1 disable failed (%d)\n", err);
		return err;
	}
	sensor_rail_enabled = false;
	return 0;
}

#else /* devicetree has no nPM1300 LDO1 */

void power_init(void)
{
	printk("[PWR] no nPM1300 LDO1 node — sensor rail unmanaged\n");
}

int power_sensor_rail_enable(void)
{
	return 0;
}

int power_sensor_rail_disable(void)
{
	return 0;
}

#endif

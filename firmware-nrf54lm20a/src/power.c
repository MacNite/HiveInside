/* power.c — see power.h. */
#include "power.h"

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
 * no-op stub below — see the app.overlay LDO1 node for the DTS-side spelling. */
#if DT_HAS_COMPAT_STATUS_OKAY(nordic_npm1300_regulator) &&                     \
	DT_NODE_EXISTS(                                                        \
		DT_CHILD(DT_COMPAT_GET_ANY_STATUS_OKAY(nordic_npm1300_regulator), \
			 ldo1))

#include <zephyr/drivers/regulator.h>

#define LDO1_NODE \
	DT_CHILD(DT_COMPAT_GET_ANY_STATUS_OKAY(nordic_npm1300_regulator), ldo1)

static const struct device *const ldo1 = DEVICE_DT_GET(LDO1_NODE);

void power_init(void)
{
	if (!device_is_ready(ldo1)) {
		printk("[PWR] nPM1300 LDO1 not ready — sensor rail unmanaged\n");
		return;
	}

	int err = regulator_set_voltage(ldo1, 3300000, 3300000);

	if (err != 0) {
		printk("[PWR] LDO1 set 3.3V failed (%d)\n", err);
	}
	if (!regulator_is_enabled(ldo1)) {
		err = regulator_enable(ldo1);
		if (err != 0) {
			printk("[PWR] LDO1 enable failed (%d)\n", err);
			return;
		}
	}
	/* The IMU and microphone share this rail.  Give both parts time to
	 * leave reset before their first I2C/PDM transaction. */
	k_msleep(20);
	printk("[PWR] nPM1300 LDO1 at 3.3V (IMU + mic rail)\n");
}

#else /* devicetree has no nPM1300 LDO1 */

void power_init(void)
{
}

#endif

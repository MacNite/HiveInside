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
static uint32_t sensor_rail_users;
K_MUTEX_DEFINE(sensor_rail_lock);

/* P1.12 gates the complete Sense-board sensor supply.  LDO1 being enabled is
 * not sufficient when this upstream fixed regulator is off.  Seeed's DMIC
 * reference application explicitly enables both regulators in this order. */
#if DT_NODE_EXISTS(DT_NODELABEL(power_en))
static const struct device *const power_en =
	DEVICE_DT_GET(DT_NODELABEL(power_en));
/* Whether this file holds its one reference on power_en. Tracked apart from
 * sensor_rail_enabled because the two regulators can fail independently: a
 * gate that failed to switch off must not be enabled (and referenced) again. */
static bool gate_held;
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

static int disable_regulator(const struct device *dev, const char *name)
{
	if (!device_is_ready(dev)) {
		printk("[PWR] %s regulator not ready\n", name);
		return -ENODEV;
	}

	int err = regulator_disable(dev);

	/* Symmetric with enable_regulator(): a rail that is already off may be
	 * reported either way depending on the driver. */
	if (err != 0 && err != -EALREADY) {
		printk("[PWR] %s disable failed (%d)\n", name, err);
		return err;
	}

	return 0;
}

/* Hand back the reference Zephyr's regulator core takes during its own init,
 * so that this file holds exactly one reference to each rail.
 *
 * regulator_disable() only reaches the hardware when the reference count drops
 * to zero, and the core starts at one whenever it finds a rail already on:
 * `regulator-boot-on` does that for `power_en` (regulator_fixed_init() drives
 * the GPIO active and counts it), and regulator_npm13xx_init() does it for
 * LDO1 whenever it reads the rail back as enabled. The nPM1300 is a separate
 * chip that no CPU reset touches, so after a watchdog reset taken during a
 * capture, LDO1 is still on and the count starts at one even with boot-on
 * removed from the devicetree.
 *
 * Without this, power_init()'s own enable below would leave the count at two
 * and power_sensor_rail_disable() would only ever decrement it to one — the
 * rail would stay powered for the rest of the node's life, silently, with the
 * firmware reporting success. Releasing the init reference first makes the
 * gating hold in every build and after every reset.
 *
 * A disable at a count of zero is a no-op inside the core, so the ordinary
 * cold-boot path is unaffected. */
static void release_init_reference(const struct device *dev)
{
	if (device_is_ready(dev)) {
		(void)regulator_disable(dev);
	}
}

#if DT_NODE_EXISTS(DT_NODELABEL(power_en))
static int gate_acquire(void)
{
	if (gate_held) {
		return 0;
	}
	int err = enable_regulator(power_en, "sensor power gate");

	if (err == 0) {
		gate_held = true;
	}
	return err;
}

static void gate_release(void)
{
	if (gate_held && disable_regulator(power_en, "sensor power gate") == 0) {
		gate_held = false;
	}
}
#else
static int gate_acquire(void) { return 0; }
static void gate_release(void) { }
#endif

void power_init(void)
{
	/* The board DTS marks this regulator boot-on, but enabling it explicitly
	 * also works with older Seeed board definitions and mirrors their known-
	 * good microphone example. Release first: on a boot-on build that briefly
	 * drops the gate before this function raises it again, which the 20 ms
	 * settle below already covers, and it is what leaves the reference count
	 * where power_sensor_rail_disable() can reach zero. */
#if DT_NODE_EXISTS(DT_NODELABEL(power_en))
	release_init_reference(power_en);
#endif
	if (gate_acquire() != 0) {
		return;
	}

	if (!device_is_ready(ldo1)) {
		printk("[PWR] nPM1300 LDO1 not ready — sensor rail unmanaged\n");
		/* main() drops the boot reference only for a rail that came up,
		 * so a gate left on here would stay on for good. */
		gate_release();
		return;
	}
	release_init_reference(ldo1);

	int err = regulator_set_voltage(ldo1, 3300000, 3300000);

	if (err != 0) {
		printk("[PWR] LDO1 set 3.3V failed (%d)\n", err);
	}
	if (enable_regulator(ldo1, "LDO1") != 0) {
		gate_release();
		return;
	}
	sensor_rail_enabled = true;
	sensor_rail_users = 1;
	/* The IMU and microphone share this rail. Give both parts time to
	 * leave reset before their first I2C/PDM transaction. */
	k_msleep(20);
	printk("[PWR] nPM1300 LDO1 at 3.3V (IMU + mic rail)\n");
}

int power_sensor_rail_enable(void)
{
	int err;
	k_mutex_lock(&sensor_rail_lock, K_FOREVER);

	if (sensor_rail_enabled) {
		sensor_rail_users++;
		k_mutex_unlock(&sensor_rail_lock);
		return 0;
	}

	/* Upstream gate first, in the same order as power_init() and Seeed's
	 * reference application: LDO1 alone does not power the island. */
	err = gate_acquire();
	if (err != 0) {
		k_mutex_unlock(&sensor_rail_lock);
		return err;
	}

	err = enable_regulator(ldo1, "LDO1");
	if (err != 0) {
		/* Undo the gate, or every failed attempt would leave it on with
		 * nobody holding the rail to switch it off again. */
		gate_release();
		k_mutex_unlock(&sensor_rail_lock);
		return err;
	}
	sensor_rail_enabled = true;
	sensor_rail_users = 1;
	/* The IMU and microphone need time to leave reset before use. */
	k_msleep(20);
	k_mutex_unlock(&sensor_rail_lock);
	return 0;
}

int power_sensor_rail_disable(void)
{
	int err;
	k_mutex_lock(&sensor_rail_lock, K_FOREVER);

	/* Every successful enable owns one reference.  Only its matching disable
	 * may drop that reference, and hardware is switched off at zero. */
	if (!sensor_rail_enabled || sensor_rail_users == 0U) {
		k_mutex_unlock(&sensor_rail_lock);
		return 0;
	}
	if (--sensor_rail_users != 0U) {
		k_mutex_unlock(&sensor_rail_lock);
		return 0;
	}

	err = disable_regulator(ldo1, "LDO1");
	if (err != 0) {
		sensor_rail_users = 1;
		k_mutex_unlock(&sensor_rail_lock);
		return err;
	}
	sensor_rail_enabled = false;

	/* Then the upstream gate, so the sensor island is not left sitting in a
	 * powered standby path for the ~99 % of each cycle that takes no
	 * measurement. Reverse order of the enable above. If it fails, gate_held
	 * stays set: the next enable reuses that reference instead of taking a
	 * second one, and the next disable tries again. */
	gate_release();

	k_mutex_unlock(&sensor_rail_lock);
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

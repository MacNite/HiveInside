/* Sensor-rail reference handling in the real power.c (HI-08). */
#include "shim_test.h"

#include "power.c"

#define LDO1 (&shim_dev_ldo1)
#define GATE (&shim_dev_power_en)

/* Boot as main() does: power_init() takes the boot reference, which main()
 * drops again once initialisation is done. */
static void boot(void)
{
	shim_regulator_reset();
	sensor_rail_enabled = false;
	sensor_rail_users = 0;
	gate_held = false;
	shim_dev_ldo1.ready = true;
	power_init();
	(void)power_sensor_rail_disable();
}

static void check_off(void)
{
	CHECK_EQ(shim_regulator_refs(LDO1), 0);
	CHECK_EQ(shim_regulator_refs(GATE), 0);
}

static void test_cycle_balanced(void)
{
	boot();
	check_off();
	for (int i = 0; i < 3; i++) {
		CHECK_EQ(power_sensor_rail_enable(), 0);
		CHECK_EQ(shim_regulator_refs(LDO1), 1);
		CHECK_EQ(shim_regulator_refs(GATE), 1);
		CHECK_EQ(power_sensor_rail_disable(), 0);
		check_off();
	}
}

static void test_nested_users(void)
{
	boot();
	power_sensor_rail_enable();
	power_sensor_rail_enable();
	power_sensor_rail_disable();
	CHECK_EQ(shim_regulator_refs(LDO1), 1);
	power_sensor_rail_disable();
	check_off();
}

/* An LDO1 failure after the gate came on must not leave the gate referenced. */
static void test_ldo_enable_failure_rolls_back_gate(void)
{
	boot();
	shim_regulator_fail_next_enable(LDO1, -EIO);
	CHECK(power_sensor_rail_enable() != 0);
	check_off();
	/* Later cycles are balanced again. */
	CHECK_EQ(power_sensor_rail_enable(), 0);
	CHECK_EQ(power_sensor_rail_disable(), 0);
	check_off();
}

/* A gate that failed to switch off is reused, not referenced twice. */
static void test_gate_disable_failure_no_double_reference(void)
{
	boot();
	power_sensor_rail_enable();
	shim_regulator_fail_next_disable(GATE, -EIO);
	power_sensor_rail_disable();
	CHECK_EQ(shim_regulator_refs(LDO1), 0);
	CHECK_EQ(shim_regulator_refs(GATE), 1);
	power_sensor_rail_enable();
	CHECK_EQ(shim_regulator_refs(GATE), 1);
	power_sensor_rail_disable();
	check_off();
}

static void test_ldo_disable_failure_keeps_rail(void)
{
	boot();
	power_sensor_rail_enable();
	shim_regulator_fail_next_disable(LDO1, -EIO);
	CHECK(power_sensor_rail_disable() != 0);
	CHECK_EQ(shim_regulator_refs(LDO1), 1);
	CHECK_EQ(power_sensor_rail_disable(), 0);
	check_off();
}

/* Boot-time failures must not strand the gate either. */
static void test_init_failures_release_gate(void)
{
	shim_regulator_reset();
	sensor_rail_enabled = false;
	sensor_rail_users = 0;
	gate_held = false;
	shim_regulator_fail_next_enable(LDO1, -EIO);
	power_init();
	(void)power_sensor_rail_disable();
	check_off();

	shim_regulator_reset();
	gate_held = false;
	shim_dev_ldo1.ready = false;
	power_init();
	(void)power_sensor_rail_disable();
	shim_dev_ldo1.ready = true;
	check_off();
}

int main(void)
{
	RUN(test_cycle_balanced);
	RUN(test_nested_users);
	RUN(test_ldo_enable_failure_rolls_back_gate);
	RUN(test_gate_disable_failure_no_double_reference);
	RUN(test_ldo_disable_failure_keeps_rail);
	RUN(test_init_failures_release_gate);
	return shim_failures != 0;
}

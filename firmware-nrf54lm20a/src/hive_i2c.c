/* hive_i2c.c — see hive_i2c.h. */
#include "hive_i2c.h"

#include <zephyr/devicetree.h>
#include <zephyr/sys/printk.h>

#define HIVE_I2C_DEV_COMMA(node_id) DEVICE_DT_GET(node_id),

static const struct device *const buses[] = {
#if DT_HAS_COMPAT_STATUS_OKAY(nordic_nrf_twim)
	DT_FOREACH_STATUS_OKAY(nordic_nrf_twim, HIVE_I2C_DEV_COMMA)
#endif
};

size_t hive_i2c_buses(const struct device **out, size_t max)
{
	size_t n = 0;

	for (size_t i = 0; i < ARRAY_SIZE(buses) && n < max; i++) {
		if (!device_is_ready(buses[i])) {
			printk("[I2C] bus %s not ready\n", buses[i]->name);
			continue;
		}
		out[n++] = buses[i];
	}
	return n;
}

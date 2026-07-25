/* battery.c — see battery.h.
 *
 * The XIAO nRF54LM20A carries an nPM1300 PMIC whose charger block measures the
 * cell voltage directly, exposed by Zephyr as a sensor device (compatible
 * `nordic,npm1300-charger`, channel SENSOR_CHAN_GAUGE_VOLTAGE). A simple LiPo
 * curve turns volts into a rough percentage.
 */
#include "battery.h"

#include "hive_config.h"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/sys/printk.h>

#if ENABLE_BATTERY && DT_HAS_COMPAT_STATUS_OKAY(nordic_npm1300_charger)

static const struct device *const charger =
	DEVICE_DT_GET(DT_COMPAT_GET_ANY_STATUS_OKAY(nordic_npm1300_charger));

void battery_read(struct measurement *m)
{
	m->battery_ok = false;

	if (!device_is_ready(charger)) {
		printk("[BATT] nPM1300 charger not ready\n");
		return;
	}

	struct sensor_value val;

	if (sensor_sample_fetch(charger) != 0 ||
	    sensor_channel_get(charger, SENSOR_CHAN_GAUGE_VOLTAGE, &val) != 0) {
		printk("[BATT] fuel-gauge read failed\n");
		return;
	}

	float v = (float)sensor_value_to_double(&val);
	float pct = (v - VBAT_EMPTY_V) / (VBAT_FULL_V - VBAT_EMPTY_V) * 100.0f;

	if (pct < 0.0f) {
		pct = 0.0f;
	}
	if (pct > 100.0f) {
		pct = 100.0f;
	}

	m->battery_v = v;
	m->battery_pct = (uint8_t)(pct + 0.5f);
	m->battery_ok = true;
}

#else /* !ENABLE_BATTERY or no nPM1300 charger node in the devicetree */

void battery_read(struct measurement *m)
{
	m->battery_ok = false;
}

#endif

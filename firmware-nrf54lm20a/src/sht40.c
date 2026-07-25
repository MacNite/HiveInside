/* sht40.c — SHT40 climate read (raw I²C, no external driver).
 *
 * Command 0xE0 = "measure, lowest precision": the shortest conversion
 * (~1.6 ms) and the lowest per-cycle energy.
 */
#include "sht40.h"

#include "hive_config.h"
#include "hive_i2c.h"

#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#if ENABLE_SHT40

#define SHT40_CMD_MEASURE_LOW 0xE0
#define SHT40_MEASURE_WAIT_MS 5

static const struct device *sht_bus; /* bus the sensor answered on */
static bool probed;

/* CRC-8 as specified in the SHT4x datasheet (poly 0x31, init 0xFF). */
static uint8_t sht40_crc(const uint8_t *data, size_t len)
{
	uint8_t crc = 0xFF;

	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int b = 0; b < 8; b++) {
			crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31)
					   : (uint8_t)(crc << 1);
		}
	}
	return crc;
}

static bool sht40_measure(const struct device *bus, float *temp_c,
			  float *humidity_pct)
{
	uint8_t cmd = SHT40_CMD_MEASURE_LOW;
	uint8_t raw[6];

	if (i2c_write(bus, &cmd, 1, SHT40_ADDR) != 0) {
		return false;
	}
	k_msleep(SHT40_MEASURE_WAIT_MS);
	if (i2c_read(bus, raw, sizeof(raw), SHT40_ADDR) != 0) {
		return false;
	}
	if (sht40_crc(&raw[0], 2) != raw[2] || sht40_crc(&raw[3], 2) != raw[5]) {
		printk("[SHT40] CRC mismatch\n");
		return false;
	}

	uint16_t t_raw = ((uint16_t)raw[0] << 8) | raw[1];
	uint16_t h_raw = ((uint16_t)raw[3] << 8) | raw[4];

	/* Datasheet conversion; RH clamped to the physical 0–100 % range. */
	*temp_c = -45.0f + 175.0f * (float)t_raw / 65535.0f;
	float rh = -6.0f + 125.0f * (float)h_raw / 65535.0f;

	if (rh < 0.0f) {
		rh = 0.0f;
	}
	if (rh > 100.0f) {
		rh = 100.0f;
	}
	*humidity_pct = rh;
	return true;
}

static const struct device *sht40_locate(void)
{
	const struct device *buses[4];
	size_t n = hive_i2c_buses(buses, ARRAY_SIZE(buses));
	float t, h;

	for (size_t i = 0; i < n; i++) {
		if (sht40_measure(buses[i], &t, &h)) {
			printk("[SHT40] present on %s\n", buses[i]->name);
			return buses[i];
		}
	}
	printk("[SHT40] not found on any I2C bus\n");
	return NULL;
}

void sht40_read(struct measurement *m)
{
	m->sht_ok = false;

	if (!probed || sht_bus == NULL) {
		sht_bus = sht40_locate();
		probed = true;
	}
	if (sht_bus == NULL) {
		return;
	}

	if (!sht40_measure(sht_bus, &m->temp_c, &m->humidity_pct)) {
		printk("[SHT40] read failed\n");
		/* Re-probe next cycle in case the sensor was re-wired. */
		sht_bus = NULL;
		return;
	}
	m->sht_ok = true;
}

#else /* !ENABLE_SHT40 */

void sht40_read(struct measurement *m)
{
	m->sht_ok = false;
}

#endif /* ENABLE_SHT40 */

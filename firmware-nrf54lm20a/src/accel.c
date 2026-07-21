/* accel.c — see accel.h.
 *
 * Ported from the ESP32-C6 prototype's accel.cpp. The capture/FFT pipeline
 * is unchanged (DC removal, Hann window, band RMS in mg with the same
 * normalisation); what differs is the register backend, because the Sense
 * module's on-board IMU is an LSM6DS3TR-C, not the prototype's LIS3DH. Both
 * register maps are tiny and probed by WHO_AM_I, so either chip works and
 * the LIS3DH breakout wiring stays usable for bench comparisons.
 *
 * The sensor is powered down between cycles (ODR = 0) — on the Sense module
 * the IMU rail (nPM1300 LDO1) stays up for the mic anyway, so power-down
 * current is the floor either way.
 */
#include "accel.h"

#include "fft.h"
#include "hive_config.h"
#include "hive_i2c.h"

#include <math.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#if ENABLE_ACCEL

enum accel_chip {
	CHIP_NONE = 0,
	CHIP_LIS3DH, /* LIS3DH / LIS2DH12 — identical for everything used */
	CHIP_LSM6,   /* LSM6DS3TR-C / LSM6DS3 / LSM6DSL / LSM6DSO */
};

/* ── LIS3DH register map (as in the prototype) ── */
#define LIS_REG_WHO_AM_I  0x0F /* == 0x33 */
#define LIS_REG_CTRL1     0x20 /* ODR + axis enables */
#define LIS_REG_CTRL4     0x23 /* BDU, full-scale, high-res */
#define LIS_REG_STATUS    0x27 /* ZYXDA = new-data ready */
#define LIS_REG_OUT_X_L   0x28
#define LIS_AUTO_INC      0x80 /* multi-byte read bit */
#define LIS_WHO_AM_I_VAL  0x33
#define LIS_ODR_HZ        400
#define LIS_MG_PER_DIGIT  1.0f /* ±2 g, 12-bit high-resolution */

/* ── LSM6DS3TR-C register map (subset; shared across the LSM6 family) ── */
#define LSM6_REG_WHO_AM_I 0x0F /* 0x69/0x6A/0x6C depending on variant */
#define LSM6_REG_CTRL1_XL 0x10 /* accel ODR + full scale */
#define LSM6_REG_STATUS   0x1E /* bit0 XLDA = accel data ready */
#define LSM6_REG_OUTX_L   0x28 /* auto-increments (IF_INC defaults on) */
#define LSM6_ODR_HZ       416
#define LSM6_CTRL1_416_2G 0x60 /* ODR_XL = 416 Hz, FS = ±2 g */
#define LSM6_MG_PER_LSB   0.061f /* ±2 g sensitivity */

static const struct device *acc_bus;
static uint8_t acc_addr;
static enum accel_chip acc_chip;
static bool probed;

/* FFT scratch — static so a failed cycle can never leak heap. */
static float fft_re[ACCEL_SAMPLE_COUNT];
static float fft_im[ACCEL_SAMPLE_COUNT];

static int rd_regs(uint8_t reg, uint8_t *buf, size_t len)
{
	return i2c_write_read(acc_bus, acc_addr, &reg, 1, buf, len);
}

static int wr_reg(uint8_t reg, uint8_t val)
{
	uint8_t tx[2] = { reg, val };

	return i2c_write(acc_bus, tx, sizeof(tx), acc_addr);
}

static bool probe_addr(const struct device *bus, uint8_t addr,
		       enum accel_chip *chip)
{
	uint8_t reg = LIS_REG_WHO_AM_I; /* same offset on both families */
	uint8_t who = 0;

	if (i2c_write_read(bus, addr, &reg, 1, &who, 1) != 0) {
		return false;
	}
	if (who == LIS_WHO_AM_I_VAL) {
		*chip = CHIP_LIS3DH;
		return true;
	}
	if (who == 0x69 || who == 0x6A || who == 0x6C) {
		*chip = CHIP_LSM6;
		return true;
	}
	return false;
}

static void locate(void)
{
	static const uint8_t addrs[] = { 0x6A, 0x6B, 0x18, 0x19 };
	const struct device *buses[4];
	size_t n = hive_i2c_buses(buses, ARRAY_SIZE(buses));

	for (size_t b = 0; b < n; b++) {
		for (size_t a = 0; a < ARRAY_SIZE(addrs); a++) {
			enum accel_chip chip;

			if (probe_addr(buses[b], addrs[a], &chip)) {
				acc_bus = buses[b];
				acc_addr = addrs[a];
				acc_chip = chip;
				printk("[ACCEL] %s at 0x%02X on %s\n",
				       chip == CHIP_LIS3DH ? "LIS3DH" : "LSM6-class IMU",
				       addrs[a], buses[b]->name);
				return;
			}
		}
	}
	acc_chip = CHIP_NONE;
	printk("[ACCEL] no accelerometer found\n");
}

static void power_down(void)
{
	if (acc_chip == CHIP_LIS3DH) {
		wr_reg(LIS_REG_CTRL1, 0x00);
	} else if (acc_chip == CHIP_LSM6) {
		wr_reg(LSM6_REG_CTRL1_XL, 0x00);
	}
}

/* Configure for the capture; returns the actual sample rate (0 on failure). */
static uint16_t configure(void)
{
	if (acc_chip == CHIP_LIS3DH) {
		/* CTRL1: ODR 400 Hz | XYZ enable. CTRL4: BDU | ±2 g | HR. */
		if (wr_reg(LIS_REG_CTRL1, 0x77) != 0 ||
		    wr_reg(LIS_REG_CTRL4, 0x88) != 0) {
			return 0;
		}
		return LIS_ODR_HZ;
	}
	if (acc_chip == CHIP_LSM6) {
		if (wr_reg(LSM6_REG_CTRL1_XL, LSM6_CTRL1_416_2G) != 0) {
			return 0;
		}
		return LSM6_ODR_HZ;
	}
	return 0;
}

/* Poll data-ready then read one |a| sample in mg. Bounded wait so a wedged
 * bus cannot stall the cycle. */
static bool sample_mg(uint32_t period_us, float *out_mg)
{
	uint8_t status_reg = (acc_chip == CHIP_LIS3DH) ? LIS_REG_STATUS
						       : LSM6_REG_STATUS;
	uint8_t ready_mask = (acc_chip == CHIP_LIS3DH) ? 0x08 : 0x01;
	int64_t deadline = k_uptime_ticks() +
			   k_us_to_ticks_ceil64(period_us * 4 + 2000);
	uint8_t status = 0;

	while (true) {
		if (rd_regs(status_reg, &status, 1) == 0 &&
		    (status & ready_mask)) {
			break;
		}
		if (k_uptime_ticks() > deadline) {
			break;
		}
		k_usleep(200);
	}

	uint8_t raw[6];
	uint8_t data_reg = (acc_chip == CHIP_LIS3DH)
				   ? (LIS_REG_OUT_X_L | LIS_AUTO_INC)
				   : LSM6_REG_OUTX_L;
	if (rd_regs(data_reg, raw, sizeof(raw)) != 0) {
		return false;
	}

	int16_t xr = (int16_t)(((uint16_t)raw[1] << 8) | raw[0]);
	int16_t yr = (int16_t)(((uint16_t)raw[3] << 8) | raw[2]);
	int16_t zr = (int16_t)(((uint16_t)raw[5] << 8) | raw[4]);
	float x, y, z;

	if (acc_chip == CHIP_LIS3DH) {
		/* 12-bit high-resolution, left-justified in the 16-bit word. */
		x = (float)(xr >> 4) * LIS_MG_PER_DIGIT;
		y = (float)(yr >> 4) * LIS_MG_PER_DIGIT;
		z = (float)(zr >> 4) * LIS_MG_PER_DIGIT;
	} else {
		x = (float)xr * LSM6_MG_PER_LSB;
		y = (float)yr * LSM6_MG_PER_LSB;
		z = (float)zr * LSM6_MG_PER_LSB;
	}
	*out_mg = sqrtf(x * x + y * y + z * z);
	return true;
}

void accel_read(struct measurement *m)
{
	m->accel_ok = false;

	if (!probed || acc_chip == CHIP_NONE) {
		locate();
		probed = true;
	}
	if (acc_chip == CHIP_NONE) {
		return;
	}

	uint16_t odr_hz = configure();

	if (odr_hz == 0) {
		printk("[ACCEL] config write failed\n");
		power_down();
		return;
	}
	m->accel_sample_rate_hz = odr_hz;

	/* Let the just-(re)enabled ODR and filter chain settle. */
	k_msleep(20);

	const uint32_t period_us = 1000000UL / odr_hz;
	size_t n = 0;

	for (; n < ACCEL_SAMPLE_COUNT; n++) {
		if (!sample_mg(period_us, &fft_re[n])) {
			break;
		}
	}
	power_down();

	if (n < 64) {
		printk("[ACCEL] only %u samples; skipping\n", (unsigned)n);
		return;
	}

	/* Remove DC (gravity + mounting bias) so bands reflect AC vibration. */
	float sum = 0.0f;

	for (size_t i = 0; i < n; i++) {
		sum += fft_re[i];
	}
	float mean = sum / (float)n;
	float sum_sq = 0.0f, peak_dev = 0.0f;

	for (size_t i = 0; i < n; i++) {
		float ac = fft_re[i] - mean;

		sum_sq += ac * ac;
		float dev = fabsf(ac);

		if (dev > peak_dev) {
			peak_dev = dev;
		}
		fft_re[i] = ac;
		fft_im[i] = 0.0f;
	}
	for (size_t i = n; i < ACCEL_SAMPLE_COUNT; i++) {
		fft_re[i] = 0.0f;
		fft_im[i] = 0.0f;
	}

	m->accel_sample_count = (uint16_t)n;
	m->accel_rms_mg = sqrtf(sum_sq / (float)n);
	m->accel_peak_mg = peak_dev;

	hive_fft_hann(fft_re, ACCEL_SAMPLE_COUNT);
	hive_fft_magnitude(fft_re, fft_im, ACCEL_SAMPLE_COUNT);

	const float norm = ((float)ACCEL_SAMPLE_COUNT / 2.0f) * 0.5f;
	const float fs = (float)odr_hz;

	m->accel_band_swarm_mg = hive_fft_band_amplitude(
		fft_re, ACCEL_SAMPLE_COUNT, fs, ACC_BAND_SWARM_LO,
		ACC_BAND_SWARM_HI, norm, true);
	m->accel_band_fanning_mg = hive_fft_band_amplitude(
		fft_re, ACCEL_SAMPLE_COUNT, fs, ACC_BAND_FANNING_LO,
		ACC_BAND_FANNING_HI, norm, true);
	m->accel_band_activity_mg = hive_fft_band_amplitude(
		fft_re, ACCEL_SAMPLE_COUNT, fs, ACC_BAND_ACTIVITY_LO,
		ACC_BAND_ACTIVITY_HI, norm, true);
	m->accel_ok = true;

	printk("[ACCEL] rms=%.1f peak=%.1f mg | swarm=%.2f fan=%.2f act=%.2f mg (%u@%uHz)\n",
	       (double)m->accel_rms_mg, (double)m->accel_peak_mg,
	       (double)m->accel_band_swarm_mg, (double)m->accel_band_fanning_mg,
	       (double)m->accel_band_activity_mg, (unsigned)n,
	       (unsigned)odr_hz);
}

#else /* !ENABLE_ACCEL */

void accel_read(struct measurement *m)
{
	m->accel_ok = false;
}

#endif /* ENABLE_ACCEL */

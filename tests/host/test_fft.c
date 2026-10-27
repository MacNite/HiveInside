/* Band amplitude scale and length contract of the real fft.c (HI-11, HI-13). */
#include "shim_test.h"

#include <math.h>
#include <stdlib.h>

#include "fft.c"

#define TWO_PI 6.28318530717958647692

static float re[2048], im[2048];

/* Band reading for a tone of peak amplitude amp at freq_hz. */
static float tone_band(size_t n, float fs, double freq_hz, float amp,
		       float lo_hz, float hi_hz)
{
	for (size_t i = 0; i < n; i++) {
		re[i] = amp * (float)sin(TWO_PI * freq_hz * (double)i / fs);
		im[i] = 0.0f;
	}
	hive_fft_hann(re, n);
	hive_fft_magnitude(re, im, n);
	return hive_fft_band_amplitude(re, n, fs, lo_hz, hi_hz,
				       hive_fft_hann_norm(n), false);
}

static bool near(float value, float expected, float rel)
{
	return fabsf(value - expected) <= rel * fabsf(expected);
}

/* The header's promise: a sinusoid in the band reads its peak amplitude,
 * whether it sits on a bin or between two (the audit's 250 Hz case read
 * 1.224 before the window's noise bandwidth was divided out). */
static void test_tone_reads_its_amplitude(void)
{
	const double freqs[] = { 250.0, 253.9, 400.0, 1000.0 };

	for (size_t i = 0; i < ARRAY_SIZE(freqs); i++) {
		float v = tone_band(2048, 16000.0f, freqs[i], 1.0f, 150.0f, 1500.0f);
		if (!near(v, 1.0f, 0.01f)) {
			fprintf(stderr, "  %.1f Hz read %f\n", freqs[i], (double)v);
		}
		CHECK(near(v, 1.0f, 0.01f));
	}
	/* Linear in amplitude. */
	CHECK(near(tone_band(2048, 16000.0f, 440.0, 0.25f, 300.0f, 550.0f),
		   0.25f, 0.01f));
}

/* The vibration path: 1024 points at 400 Hz, a 16.25 Hz swarm-band tone. */
static void test_accel_geometry(void)
{
	float v = tone_band(1024, 400.0f, 16.25, 1.0f, 8.0f, 30.0f);
	CHECK(near(v, 1.0f, 0.01f));
}

/* Two tones in one band add in energy: sqrt(a^2 + b^2). */
static void test_two_tones_add_in_energy(void)
{
	for (size_t i = 0; i < 2048; i++) {
		re[i] = (float)(0.6 * sin(TWO_PI * 200.0 * (double)i / 16000.0) +
				0.8 * sin(TWO_PI * 260.0 * (double)i / 16000.0));
		im[i] = 0.0f;
	}
	hive_fft_hann(re, 2048);
	hive_fft_magnitude(re, im, 2048);
	float v = hive_fft_band_amplitude(re, 2048, 16000.0f, 150.0f, 300.0f,
					  hive_fft_hann_norm(2048), false);
	CHECK(near(v, 1.0f, 0.02f));
}

/* A tone outside the band contributes only window leakage. */
static void test_out_of_band_rejected(void)
{
	float v = tone_band(2048, 16000.0f, 1000.0, 1.0f, 50.0f, 150.0f);
	CHECK(v < 0.001f);
}

static void test_silence(void)
{
	float v = tone_band(2048, 16000.0f, 250.0, 0.0f, 150.0f, 300.0f);
	CHECK_EQ(v, 0.0f);
}

/* HI-13: lengths the radix-2 transform cannot handle never index past the
 * arrays (AddressSanitizer would report it), and produce zeros. */
static void test_invalid_lengths_are_safe(void)
{
	const size_t lengths[] = { 0, 1, 4, 7, 1000 };

	for (size_t i = 0; i < ARRAY_SIZE(lengths); i++) {
		size_t n = lengths[i];
		float *r = calloc(n + 1, sizeof(float));
		float *m = calloc(n + 1, sizeof(float));
		for (size_t k = 0; k < n; k++) {
			r[k] = 1.0f;
		}
		hive_fft_magnitude(r, m, n);
		for (size_t k = 0; k <= n / 2; k++) {
			CHECK_EQ(r[k], 0.0f);
		}
		free(r);
		free(m);
	}
	CHECK(HIVE_FFT_LENGTH_VALID(8));
	CHECK(HIVE_FFT_LENGTH_VALID(2048));
	CHECK(!HIVE_FFT_LENGTH_VALID(1000));
	CHECK(!HIVE_FFT_LENGTH_VALID(4));
}

int main(void)
{
	RUN(test_tone_reads_its_amplitude);
	RUN(test_accel_geometry);
	RUN(test_two_tones_add_in_energy);
	RUN(test_out_of_band_rejected);
	RUN(test_silence);
	RUN(test_invalid_lengths_are_safe);
	return shim_failures != 0;
}

/* fft.c — see fft.h. */
#include "fft.h"

#include <math.h>

/* Not using M_PI: it is hidden behind feature-test macros in strict-C libc
 * configurations, and float precision is all the twiddles need. */
#define FFT_PI 3.14159265358979f

void hive_fft_hann(float *re, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		float w = 0.5f * (1.0f - cosf(2.0f * FFT_PI * (float)i /
					      (float)(n - 1)));
		re[i] *= w;
	}
}

/* Iterative decimation-in-time radix-2 with an initial bit-reversal pass. */
static void fft_forward(float *re, float *im, size_t n)
{
	/* Bit-reversal permutation. */
	for (size_t i = 1, j = 0; i < n; i++) {
		size_t bit = n >> 1;
		for (; j & bit; bit >>= 1) {
			j ^= bit;
		}
		j ^= bit;
		if (i < j) {
			float t = re[i]; re[i] = re[j]; re[j] = t;
			t = im[i]; im[i] = im[j]; im[j] = t;
		}
	}

	for (size_t len = 2; len <= n; len <<= 1) {
		float ang = -2.0f * FFT_PI / (float)len;
		for (size_t i = 0; i < n; i += len) {
			for (size_t j = 0; j < len / 2; j++) {
				float c = cosf(ang * (float)j);
				float s = sinf(ang * (float)j);
				size_t a = i + j;
				size_t b = i + j + len / 2;
				float tr = re[b] * c - im[b] * s;
				float ti = re[b] * s + im[b] * c;
				re[b] = re[a] - tr;
				im[b] = im[a] - ti;
				re[a] += tr;
				im[a] += ti;
			}
		}
	}
}

void hive_fft_magnitude(float *re, float *im, size_t n)
{
	fft_forward(re, im, n);
	for (size_t k = 0; k <= n / 2; k++) {
		re[k] = sqrtf(re[k] * re[k] + im[k] * im[k]);
	}
}

float hive_fft_band_amplitude(const float *mag, size_t n, float sample_rate,
			      float lo_hz, float hi_hz, float norm,
			      bool skip_dc)
{
	float freq_per_bin = sample_rate / (float)n;
	size_t bin_lo = (size_t)ceilf(lo_hz / freq_per_bin);
	size_t bin_hi = (size_t)floorf(hi_hz / freq_per_bin);
	size_t nyquist = n / 2;

	if (skip_dc && bin_lo < 1) {
		bin_lo = 1;
	}
	if (bin_lo >= nyquist) {
		return 0.0f;
	}
	if (bin_hi >= nyquist) {
		bin_hi = nyquist - 1;
	}

	float sum_sq = 0.0f;
	for (size_t b = bin_lo; b <= bin_hi; b++) {
		float m = mag[b] / norm;
		sum_sq += m * m;
	}
	return sum_sq > 0.0f ? sqrtf(sum_sq) : 0.0f;
}

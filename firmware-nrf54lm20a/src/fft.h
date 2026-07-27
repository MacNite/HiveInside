/*
 * fft.h — small in-place radix-2 FFT + band reduction for the vibration and
 * acoustic analyses.
 *
 * A dependency-free float implementation (the nRF54's M33 FPU makes float
 * plenty fast for 2048-point transforms). The Hann window, magnitude
 * normalisation and band summation match the shared ecosystem definitions so a
 * band value means the same thing across the ecosystem.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Apply a Hann window in place. */
void hive_fft_hann(float *re, size_t n);

/* In-place forward FFT (n must be a power of two, n >= 8), then write the
 * magnitudes |X[k]| for k = 0..n/2 into re[]. im[] is scratch and must be
 * zeroed by the caller for a real input signal. */
void hive_fft_magnitude(float *re, float *im, size_t n);

/* Root-sum-square amplitude of all bins whose centre frequency falls in
 * [lo_hz, hi_hz]. norm undoes the FFT + Hann window gain — pass
 * (n/2) * 0.5 (0.5 = Hann coherent gain) so a full-scale in-band sinusoid
 * reads ~1.0 in input units. skip_dc excludes bin 0 (used by the vibration
 * path, where DC is gravity). Returns 0 for an empty/out-of-range band. */
float hive_fft_band_amplitude(const float *mag, size_t n, float sample_rate,
			      float lo_hz, float hi_hz, float norm,
			      bool skip_dc);

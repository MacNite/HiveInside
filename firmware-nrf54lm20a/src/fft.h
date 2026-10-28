/*
 * fft.h — small in-place radix-2 FFT + band reduction for the vibration and
 * acoustic analyses.
 *
 * A dependency-free float implementation (the nRF54's M33 FPU makes float
 * plenty fast for 2048-point transforms). The window, the band edges and the
 * band summation follow HiveScale. The normalisation does not quite: since
 * firmware 0.7.0 it also divides out the Hann window's noise bandwidth (see
 * hive_fft_hann_norm()), so a band reads the physical amplitude the header
 * promises. Band values are 1.76 dB (a factor 0.816) lower than HiveScale's
 * and earlier HiveInside firmware for the same signal; a consumer comparing
 * them keys on the firmware version in the beacon's identity record.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Apply a Hann window in place. */
void hive_fft_hann(float *re, size_t n);

/* True for the transform lengths hive_fft_magnitude() accepts: a power of two,
 * at least 8. Usable in BUILD_ASSERT. */
#define HIVE_FFT_LENGTH_VALID(n) ((n) >= 8 && (((n) & ((n) - 1)) == 0))

/* In-place forward FFT (n must satisfy HIVE_FFT_LENGTH_VALID), then write the
 * magnitudes |X[k]| for k = 0..n/2 into re[]. im[] is scratch and must be
 * zeroed by the caller for a real input signal. An invalid n touches nothing
 * beyond re[0..n/2], which it zeroes. */
void hive_fft_magnitude(float *re, float *im, size_t n);

/* The norm to pass to hive_fft_band_amplitude() for an n-point Hann-windowed
 * transform: (n/2) * 0.5 undoes the FFT scaling and the window's coherent
 * gain, and sqrt(1.5) its equivalent noise bandwidth. The window spreads a
 * tone's energy over neighbouring bins, so summing a band's bins without that
 * last factor read a unit tone as 1.22 (+1.76 dB). */
float hive_fft_hann_norm(size_t n);

/* Root-sum-square amplitude of all bins whose centre frequency falls in
 * [lo_hz, hi_hz], divided by norm. With hive_fft_hann_norm(n), a sinusoid of
 * peak amplitude A inside the band reads A, wherever it falls between bins,
 * and broadband content reads its in-band energy in the same units (Parseval).
 * skip_dc excludes bin 0 (used by the vibration path, where DC is gravity).
 * Returns 0 for an empty/out-of-range band. */
float hive_fft_band_amplitude(const float *mag, size_t n, float sample_rate,
			      float lo_hz, float hi_hz, float norm,
			      bool skip_dc);

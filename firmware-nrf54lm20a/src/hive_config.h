/*
 * hive_config.h — HiveInside XIAO nRF54LM20A Sense: identity, timing and
 * per-sensor settings.
 *
 * Target 1 (this firmware): read every sensor once per cycle and print the
 * result to the USB serial console. No BLE, no FFT — just a clean sensor
 * readout to prove the hardware and the toolchain end to end. Later targets
 * (BLE measurement beacon, FFT band analysis) build on top of this.
 *
 * Hardware (see docs/wiring.md):
 *
 *   * Seeed XIAO nRF54LM20A Sense — nRF54LM20A (Cortex-M33 @ 128 MHz),
 *     on-board LSM6DS3TR-C 6-axis IMU, MSM261DGT006 PDM microphone and
 *     nPM1300 PMIC (LiPo charging + fuel gauge).
 *   * SHT40 — external temperature + humidity sensor on the XIAO I²C header.
 *
 * Every value can be overridden from the build (e.g. PlatformIO build_flags
 * or -DEXTRA_CFLAGS) without editing this file.
 */
#pragma once

/* ── Identity ──────────────────────────────────────────────────────────── */

#ifndef HIVEINSIDE_FW_VERSION
#define HIVEINSIDE_FW_VERSION "0.1.0"
#endif
#ifndef HIVEINSIDE_BOARD
#define HIVEINSIDE_BOARD "nrf54lm20a"
#endif

/* ── Measurement cadence ───────────────────────────────────────────────── */

/* How often to run a full sensor cycle and print the readout. Short by
 * default so a bench operator sees fresh values quickly; between cycles the
 * CPU sleeps. */
#ifndef MEASURE_INTERVAL_MS
#define MEASURE_INTERVAL_MS 5000
#endif

/* ── Sensor enables ────────────────────────────────────────────────────── */

#ifndef ENABLE_SHT40
#define ENABLE_SHT40 1
#endif
#ifndef ENABLE_ACCEL
#define ENABLE_ACCEL 1
#endif
#ifndef ENABLE_MIC
#define ENABLE_MIC 1
#endif
#ifndef ENABLE_BATTERY
#define ENABLE_BATTERY 1
#endif

/* ── SHT40 (external, XIAO I²C header) ─────────────────────────────────── */

/* 0x44 is the fixed address for SHT40-AD1B; the -BD1B variant is 0x45. */
#ifndef SHT40_ADDR
#define SHT40_ADDR 0x44
#endif

/* ── Microphone (PDM) ──────────────────────────────────────────────────── */

/* PCM sample rate requested from the nRF PDM peripheral (hardware filtered —
 * no software decimation needed). */
#ifndef MIC_SAMPLE_RATE
#define MIC_SAMPLE_RATE 16000
#endif
/* PCM frames used for the RMS/peak level each cycle (~0.5 s at 16 kHz). */
#ifndef MIC_SAMPLE_FRAMES
#define MIC_SAMPLE_FRAMES 8000
#endif
/* 100 ms capture blocks; the first MIC_WARMUP_BLOCKS are discarded so the
 * microphone and PDM filter settle before the level is accumulated. */
#ifndef MIC_BLOCK_SAMPLES
#define MIC_BLOCK_SAMPLES 1600
#endif
#ifndef MIC_WARMUP_BLOCKS
#define MIC_WARMUP_BLOCKS 2
#endif

/* ── Battery (nPM1300 fuel gauge) ──────────────────────────────────────── */

/* Single-cell LiPo curve endpoints for the rough percentage estimate. */
#ifndef VBAT_FULL_V
#define VBAT_FULL_V 4.20f
#endif
#ifndef VBAT_EMPTY_V
#define VBAT_EMPTY_V 3.30f
#endif

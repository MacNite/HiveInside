/*
 * hive_config.h — HiveInside XIAO nRF54LM20A Sense: identity, timing, radio
 * and analysis settings.
 *
 * This is the primary HiveInside firmware target. Hardware (see docs/wiring.md):
 *
 *   * Seeed XIAO nRF54LM20A Sense — nRF54LM20A (Cortex-M33 @ 128 MHz, BLE),
 *     on-board LSM6DS3TR-C 6-axis IMU, MSM261DGT006 PDM microphone and
 *     nPM1300 PMIC (LiPo charging + fuel gauge)
 *   * SHT40 — external temperature + humidity sensor on the XIAO I²C header
 *
 * Unlike the deprecated ESP32-C6 prototype there is no deep-sleep / wake-sync
 * machinery here: the nRF54 idles at µA levels with the radio advertising, so
 * the device simply measures every MEASURE_INTERVAL_MS and broadcasts the
 * result continuously as a BLE beacon (see beacon.c). HiveHub ingests it with
 * a passive scan — no connection, no schedule to synchronise.
 *
 * Every value can be overridden from the build (e.g. PlatformIO build_flags
 * or -DEXTRA_CFLAGS) without editing this file.
 */
#pragma once

/* ── Identity ──────────────────────────────────────────────────────────── */

/* Reported in the version GATT characteristic as {"fw":...,"board":...} so
 * HiveHub can forward them to the backend for board-matched OTA selection. */
#ifndef HIVEINSIDE_FW_VERSION
#define HIVEINSIDE_FW_VERSION "1.0.0"
#endif
#ifndef HIVEINSIDE_BOARD
#define HIVEINSIDE_BOARD "nrf54lm20a"
#endif

/* ── Measurement cadence ───────────────────────────────────────────────── */

/* How often to run a full sensor cycle and refresh the beacon payload.
 * Between cycles the CPU sleeps and only the advertiser runs. */
#ifndef MEASURE_INTERVAL_MS
#define MEASURE_INTERVAL_MS (5 * 60 * 1000)
#endif

/* ── BLE beacon (see beacon.c for the frame layout) ────────────────────── */

/* 16-bit Bluetooth SIG company identifier in the manufacturer-specific AD.
 * Must match HIVEINSIDE_COMPANY_ID in HiveHub firmware/include/config.h —
 * both boards keep the Espressif id the ESP32-C6 prototype introduced so
 * existing HiveHubs decode either device unchanged. Flip both together if a
 * distinct identity is ever wanted. */
#ifndef BEACON_COMPANY_ID
#define BEACON_COMPANY_ID 0x02E5
#endif

/* Advertising interval, in 0.625 ms units (BT spec). Default 1.0–1.2 s.
 *
 * Why this rate: HiveHub runs one passive scan of HOLYIOT_BLE_SCAN_SECONDS
 * (6 s) per upload cycle, so the beacon must land several advertising events
 * inside any 6 s window — at 1.0–1.2 s that is ~5 events across the three
 * advertising channels, which makes a missed cycle vanishingly unlikely while
 * costing only a few µA of average radio current on the nRF54 (roughly a
 * 10× power saving over the 100 ms intervals used for app-discovery UX).
 * Slower than ~2 s starts to risk whole scan windows with 0–1 events (a
 * collision or busy channel then loses the cycle), which saves little
 * because the sensor cycle — not the radio — dominates the energy budget.
 * The small min/max spread lets the controller dither to avoid persistent
 * collisions with other beacons. */
#ifndef BEACON_ADV_INT_MIN
#define BEACON_ADV_INT_MIN 0x0640 /* 1000 ms */
#endif
#ifndef BEACON_ADV_INT_MAX
#define BEACON_ADV_INT_MAX 0x0780 /* 1200 ms */
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

/* ── Accelerometer (vibration FFT) ─────────────────────────────────────────
 *
 * accel.c auto-detects the sensor on any enabled I²C bus:
 *   - LSM6DS3TR-C / LSM6DS3 / LSM6DSL / LSM6DSO at 0x6A/0x6B (the Sense
 *     module's on-board IMU), or
 *   - LIS3DH / LIS2DH12 at 0x18/0x19 (the prototype's external breakout).
 * Both are sampled at ~400 Hz into the same magnitude/FFT pipeline.
 */

/* Samples fed into the vibration FFT (power of two). 1024 @ ~400 Hz ≈ 2.5 s,
 * ~0.4 Hz/bin — fine resolution for the 8–30 Hz swarm band. */
#ifndef ACCEL_SAMPLE_COUNT
#define ACCEL_SAMPLE_COUNT 1024
#endif

/* Vibration analysis bands (Hz), identical to HiveScale/HiveHub so a value
 * means the same thing across the ecosystem. */
#define ACC_BAND_SWARM_LO 8     /*  8–30 Hz   Ramsey et al. 2020 pre-swarm */
#define ACC_BAND_SWARM_HI 30
#define ACC_BAND_FANNING_LO 30  /* 30–100 Hz  ventilation / fanning */
#define ACC_BAND_FANNING_HI 100
#define ACC_BAND_ACTIVITY_LO 100 /* 100–200 Hz general worker activity */
#define ACC_BAND_ACTIVITY_HI 200

/* ── PDM microphone (acoustic FFT) ─────────────────────────────────────── */

/* PCM sample rate requested from the nRF PDM peripheral (hardware filter —
 * no software decimation needed, unlike the ESP32-C6 prototype). */
#ifndef MIC_SAMPLE_RATE
#define MIC_SAMPLE_RATE 16000
#endif
/* PCM frames captured for RMS/peak each cycle (0.5 s at 16 kHz). */
#ifndef MIC_SAMPLE_FRAMES
#define MIC_SAMPLE_FRAMES 8000
#endif
/* Samples fed into the acoustic FFT (power of two). 2048 @ 16 kHz ≈ 7.8 Hz/bin. */
#ifndef MIC_FFT_SAMPLE_COUNT
#define MIC_FFT_SAMPLE_COUNT 2048
#endif
/* 100 ms capture blocks; the first MIC_WARMUP_BLOCKS are discarded so the
 * microphone and PDM filter settle before statistics are accumulated. */
#ifndef MIC_BLOCK_SAMPLES
#define MIC_BLOCK_SAMPLES 1600
#endif
#ifndef MIC_WARMUP_BLOCKS
#define MIC_WARMUP_BLOCKS 2
#endif

/* Acoustic FFT bands (Hz) — kept aligned with HiveScale/HiveHub insights. */
#define MIC_BAND_SUBBASS_LO 50   /*   50–150 Hz structural / low rumble */
#define MIC_BAND_SUBBASS_HI 150
#define MIC_BAND_HUM_LO 150      /*  150–300 Hz normal colony hum */
#define MIC_BAND_HUM_HI 300
#define MIC_BAND_PIPING_LO 300   /*  300–550 Hz queen piping / tooting */
#define MIC_BAND_PIPING_HI 550
#define MIC_BAND_STRESS_LO 550   /*  550–1500 Hz agitated / robbing */
#define MIC_BAND_STRESS_HI 1500
#define MIC_BAND_HIGH_LO 1500    /* 1500–3000 Hz harmonic overtones */
#define MIC_BAND_HIGH_HI 3000

/* ── Battery (nPM1300 fuel gauge) ──────────────────────────────────────── */

/* Single-cell LiPo curve endpoints for the rough percentage estimate —
 * same endpoints as the ESP32-C6 prototype so percentages are comparable. */
#ifndef VBAT_FULL_V
#define VBAT_FULL_V 4.20f
#endif
#ifndef VBAT_EMPTY_V
#define VBAT_EMPTY_V 3.30f
#endif

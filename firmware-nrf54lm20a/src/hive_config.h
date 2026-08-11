/*
 * hive_config.h — HiveInside XIAO nRF54LM20A Sense: identity, timing and
 * per-sensor settings.
 *
 * This firmware reads every sensor once per cycle and prints the result to the
 * USB serial console. Alongside the plain readings it runs the same vibration
 * and acoustic FFT band analysis as HiveScale, so a band value means the same
 * thing across the ecosystem. Each completed measurement is also broadcast in
 * the manufacturer-data format consumed by HiveHub.
 *
 * Hardware (see docs/wiring.md):
 *
 *   * Seeed XIAO nRF54LM20A Sense — nRF54LM20A (Cortex-M33 @ 128 MHz),
 *     on-board LSM6DS3TR-C 6-axis IMU, MSM261DGT006 PDM microphone and
 *     nPM1300 PMIC (LiPo charging + fuel gauge).
 *   * SHT40 — external temperature + humidity sensor on the XIAO I²C header.
 *
 * Every value can be overridden from the build (e.g. -DEXTRA_CFLAGS)
 * without editing this file. The firmware version is the
 * one deliberate exception — see the comment on it below.
 */
#pragma once

/* ── Identity ──────────────────────────────────────────────────────────── */

/* Firmware version — single source of truth.
 *
 * These three numbers are the version that actually leaves the device: beacon.c
 * puts them in the scan-response identity record, and that record is the only
 * way a relay or the backend can tell which firmware a node is running. The
 * console banner string and the release artifact name (see ../CMakeLists.txt)
 * are both derived from them.
 *
 * They used to be maintained alongside a separate "0.4.2" string literal.
 * Bumping only the string produced firmware that installed correctly over the
 * air and then went on advertising the old version — from the backend that is
 * indistinguishable from an OTA that silently did nothing, because there is no
 * error anywhere to report. Deriving the string removes that failure mode.
 *
 * This is the one setting in this file deliberately *not* overridable from the
 * build: a release version is a committed fact, and a -D override would let the
 * stamped artifact name disagree with the bytes on the air. Bump it here.
 */
#define HIVEINSIDE_FW_VERSION_MAJOR 0
#define HIVEINSIDE_FW_VERSION_MINOR 4
#define HIVEINSIDE_FW_VERSION_PATCH 7

#define HIVEINSIDE_STRINGIFY_(value) #value
#define HIVEINSIDE_STRINGIFY(value) HIVEINSIDE_STRINGIFY_(value)
#define HIVEINSIDE_FW_VERSION                                 \
	HIVEINSIDE_STRINGIFY(HIVEINSIDE_FW_VERSION_MAJOR) "." \
	HIVEINSIDE_STRINGIFY(HIVEINSIDE_FW_VERSION_MINOR) "." \
	HIVEINSIDE_STRINGIFY(HIVEINSIDE_FW_VERSION_PATCH)
#ifndef HIVEINSIDE_BOARD
#define HIVEINSIDE_BOARD "nrf54lm20a"
#endif
/* Stable wire identifier for the Seeed XIAO nRF54LM20A board. */
#define HIVEINSIDE_BOARD_ID 2U

/* Human-readable identity shown by active BLE scanners during setup. The
 * manufacturer name is also kept here as explicit product metadata; BLE
 * manufacturer-data packets carry a numeric company ID, not a name string. */
#ifndef HIVEINSIDE_DEVICE_NAME
#define HIVEINSIDE_DEVICE_NAME "HiveInside"
#endif
#ifndef HIVEINSIDE_MANUFACTURER_NAME
#define HIVEINSIDE_MANUFACTURER_NAME "HiveInside"
#endif

/* Manufacturer/company ID at bytes 0..1 of the manufacturer-specific data.
 * 0x02E5 is retained for wire compatibility with the existing HiveHub decoder;
 * the following 'H' magic and format version identify this as HiveInside. */
#ifndef HIVEINSIDE_COMPANY_ID
#define HIVEINSIDE_COMPANY_ID 0x02E5
#endif

/* A one-second non-connectable advertising interval is short enough to be
 * heard reliably in HiveHub's scan window while allowing the controller to
 * sleep between three brief advertising-channel transmissions. BLE intervals
 * use 0.625 ms units. */
#ifndef BLE_ADV_INTERVAL_MS
#define BLE_ADV_INTERVAL_MS 1000
#endif
#define BLE_ADV_INTERVAL_UNITS ((BLE_ADV_INTERVAL_MS * 8U) / 5U)

/* ── Measurement cadence ───────────────────────────────────────────────── */

/* How often to run a full sensor cycle and print the readout. The last result
 * keeps advertising between cycles, so this interval does not have to overlap
 * HiveHub's scan window. Five minutes is a conservative field default that
 * still produces two fresh measurements per ten-minute reporting period. */
#ifndef MEASURE_INTERVAL_MS
#define MEASURE_INTERVAL_MS (5U * 60U * 1000U)
#endif

/* Briefly light the board's built-in blue LED after a freshly encoded
 * measurement has successfully been handed to the Bluetooth controller. */
#ifndef MEASUREMENT_LED_BLINK_MS
#define MEASUREMENT_LED_BLINK_MS 100
#endif

/* ── Watchdog ──────────────────────────────────────────────────────────────
 *
 * The timeout has to clear the longest stretch the main thread can legitimately
 * spend inside one blocking call — the ~2.5 s vibration capture, the ~0.7 s
 * microphone capture, plus per-transfer I²C timeouts — with enough margin that
 * a slow but healthy cycle is never mistaken for a hang. One minute is roughly
 * fifteen times a normal complete cycle.
 *
 * It is deliberately far shorter than MEASURE_INTERVAL_MS. main.c splits the
 * idle wait between cycles into HIVE_WDT_FEED_INTERVAL_MS slices and feeds
 * after each, so the timeout is sized against a single stuck driver call rather
 * than against the sampling period. The cost is a handful of extra wake-ups per
 * interval, which is small next to the ~300 advertising events in the same
 * window.
 */
#ifndef HIVE_WDT_TIMEOUT_MS
#define HIVE_WDT_TIMEOUT_MS 60000U
#endif
#ifndef HIVE_WDT_FEED_INTERVAL_MS
#define HIVE_WDT_FEED_INTERVAL_MS 20000U
#endif

/* ── OTA stall timeout ─────────────────────────────────────────────────────
 *
 * How long a transfer may go without a single DATA write before the node
 * abandons it. An in-progress transfer holds the sensor loop (ota_is_active()
 * gates it) and, because the node is connected, stops it advertising — so a
 * relay that opens a session and then goes quiet takes the node off the air
 * completely. The watchdog cannot help: the OTA path feeds it deliberately,
 * since a genuine upload takes minutes.
 *
 * This is an inactivity timeout, not a deadline for the whole transfer, so a
 * slow but progressing upload is never cut short. A relay streaming from HTTPS
 * normally writes several times a second; thirty seconds of complete silence
 * means the far end is gone or wedged. Aborting costs a retry, which is cheap
 * next to a hive that stops reporting until someone notices.
 */
#ifndef HIVE_OTA_STALL_TIMEOUT_MS
#define HIVE_OTA_STALL_TIMEOUT_MS 30000U
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
 *   - LIS3DH / LIS2DH12 at 0x18/0x19 (an external breakout, for bench work).
 * Both are sampled at ~400 Hz into the same magnitude/FFT pipeline.
 */

/* Samples fed into the vibration FFT (power of two). 1024 @ ~400 Hz ≈ 2.5 s,
 * ~0.4 Hz/bin — fine resolution for the 8–30 Hz swarm band. */
#ifndef ACCEL_SAMPLE_COUNT
#define ACCEL_SAMPLE_COUNT 1024
#endif

/* Vibration analysis bands (Hz), identical to HiveScale/HiveHub so a value
 * means the same thing across the ecosystem. */
#define ACC_BAND_SWARM_LO 8      /*   8–30 Hz   Ramsey et al. 2020 pre-swarm */
#define ACC_BAND_SWARM_HI 30
#define ACC_BAND_FANNING_LO 30   /*  30–100 Hz  ventilation / fanning */
#define ACC_BAND_FANNING_HI 100
#define ACC_BAND_ACTIVITY_LO 100 /* 100–200 Hz  general worker activity */
#define ACC_BAND_ACTIVITY_HI 200

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
/* Samples fed into the acoustic FFT (power of two). 2048 @ 16 kHz ≈ 7.8 Hz/bin. */
#ifndef MIC_FFT_SAMPLE_COUNT
#define MIC_FFT_SAMPLE_COUNT 2048
#endif
/* 100 ms capture blocks; the first MIC_WARMUP_BLOCKS are discarded so the
 * microphone and PDM filter settle before the level is accumulated. */
#ifndef MIC_BLOCK_SAMPLES
#define MIC_BLOCK_SAMPLES 1600
#endif
#ifndef MIC_WARMUP_BLOCKS
#define MIC_WARMUP_BLOCKS 2
#endif

/* Acoustic FFT bands (Hz), identical to HiveScale/HiveHub. */
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

/* Single-cell LiPo curve endpoints for the rough percentage estimate. */
#ifndef VBAT_FULL_V
#define VBAT_FULL_V 4.20f
#endif
#ifndef VBAT_EMPTY_V
#define VBAT_EMPTY_V 3.30f
#endif

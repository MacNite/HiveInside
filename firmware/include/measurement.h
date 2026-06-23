// measurement.h — one full sensor snapshot, shared by every module.
//
// The sensor modules fill this struct; the BLE module serialises it (as a JSON
// GATT characteristic, plus the standard Battery / Environmental-Sensing
// characteristics). Keeping one plain struct means the FFT / capture code is
// completely independent of the BLE transport.
#pragma once

#include <Arduino.h>

// Five acoustic FFT bands, in dBFS (full-scale reference), matching HiveScale.
struct MicBands {
  float sub_bass_dbfs = NAN; //   50–150 Hz
  float hum_dbfs = NAN;      //  150–300 Hz
  float piping_dbfs = NAN;   //  300–550 Hz
  float stress_dbfs = NAN;   //  550–1500 Hz
  float high_dbfs = NAN;     // 1500–3000 Hz
};

// Three vibration FFT bands, in milli-g (AC, gravity removed), matching HiveScale.
struct AccelBands {
  float swarm_mg = NAN;    //  8–30 Hz pre-swarm vibration
  float fanning_mg = NAN;  // 30–100 Hz ventilation
  float activity_mg = NAN; // 100–200 Hz general activity
};

struct Measurement {
  // ── Climate (SHT40) ──
  bool sht_ok = false;
  float temp_c = NAN;
  float humidity_pct = NAN;

  // ── Vibration (LIS3DH) ──
  bool accel_ok = false;
  uint16_t accel_sample_rate_hz = 0;
  uint16_t accel_sample_count = 0;
  float accel_rms_mg = NAN;  // broadband AC RMS of |a|
  float accel_peak_mg = NAN; // peak |a − mean|
  AccelBands accel_bands;

  // ── Acoustics (MP34DT01 / MP34DT06 PDM) ──
  bool mic_ok = false;
  uint32_t mic_sample_rate_hz = 0;
  float mic_rms_dbfs = NAN;
  float mic_peak_dbfs = NAN;
  float mic_rms_normalized = NAN; // 0..1 fraction of full scale
  MicBands mic_bands;

  // ── Power ──
  bool battery_ok = false;
  float battery_v = NAN;
  uint8_t battery_pct = 0;

  // Monotonically increasing per boot — packet id for de-duplication.
  uint8_t packet_id = 0;
};

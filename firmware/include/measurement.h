// measurement.h — shared measurement payload produced each wake cycle and
// consumed by the BTHome advertiser.
#pragma once

#include <Arduino.h>

struct MicBands {
  bool  ok        = false;
  float sub_bass  = NAN;   //   50 - 150 Hz
  float hum       = NAN;   //  150 - 300 Hz
  float piping    = NAN;   //  300 - 550 Hz  (queen piping / pre-swarm)
  float stress    = NAN;   //  550 - 1500 Hz
  float high      = NAN;   // 1500 - 3000 Hz
  float rms_dbfs  = NAN;
};

struct Measurement {
  // Climate
  bool  sht_ok        = false;
  float temperature_c = NAN;
  float humidity_pct  = NAN;

  // Pressure
  bool  lps_ok        = false;
  float pressure_hpa  = NAN;

  // Vibration (RMS magnitude + low-frequency band energy for swarm signal)
  bool  accel_ok      = false;
  float accel_rms_mg  = NAN;     // broadband RMS
  float accel_lf_mg   = NAN;     // ~10-30 Hz band (swarm-relevant)

  // Acoustics
  MicBands mic;

  // Housekeeping
  float battery_v     = NAN;
  uint8_t battery_pct = 0;
};

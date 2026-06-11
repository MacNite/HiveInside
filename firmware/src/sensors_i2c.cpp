// sensors_i2c.cpp — SHT40 / LIS2DH12 / LPS22HB on the shared I2C bus.
//
// Each sensor is optional at compile time (ENABLE_* flags) and degrades
// gracefully at runtime: if a device does not ACK, its fields stay NAN and the
// corresponding *_ok flag stays false.
#include "sensors_i2c.h"
#include "config.h"

#include <Wire.h>

#if ENABLE_ACCEL
#include <Adafruit_LIS2DH.h>
#include <Adafruit_Sensor.h>
static Adafruit_LIS2DH lis;
static bool lisReady = false;
#endif

#if ENABLE_SHT4X
#include <Adafruit_SHT4x.h>
static Adafruit_SHT4x sht;
static bool shtReady = false;
#endif

#if ENABLE_LPS22
#include <Adafruit_LPS2X.h>
static Adafruit_LPS22 lps;
static bool lpsReady = false;
#endif

bool sensorsInit() {
  Wire.begin();
  Wire.setClock(400000);

  bool any = false;

#if ENABLE_SHT4X
  shtReady = sht.begin(&Wire);
  if (shtReady) {
    sht.setPrecision(SHT4X_HIGH_PRECISION);
    sht.setHeater(SHT4X_NO_HEATER);
    any = true;
  }
#endif

#if ENABLE_LPS22
  lpsReady = lps.begin_I2C(ADDR_LPS22HB, &Wire);
  if (lpsReady) any = true;
#endif

#if ENABLE_ACCEL
  lisReady = lis.begin(ADDR_LIS2DH12, &Wire);
  if (lisReady) {
    lis.setRange(LIS2DH_RANGE_2_G);
    // Low data rate keeps current down; we only need a short RMS burst.
    lis.setDataRate(LIS2DH_DATARATE_100_HZ);
    any = true;
  }
#endif

  return any;
}

void sensorsRead(Measurement& m) {
#if ENABLE_SHT4X
  if (shtReady) {
    sensors_event_t hum, temp;
    if (sht.getEvent(&hum, &temp)) {
      m.temperature_c = temp.temperature;
      m.humidity_pct  = hum.relative_humidity;
      m.sht_ok = true;
    }
  }
#endif

#if ENABLE_LPS22
  if (lpsReady) {
    sensors_event_t pressure, temp;
    if (lps.getEvent(&pressure, &temp)) {
      m.pressure_hpa = pressure.pressure;  // hPa
      m.lps_ok = true;
    }
  }
#endif

#if ENABLE_ACCEL
  if (lisReady) {
    // Collect a short window and compute broadband RMS plus a crude
    // low-frequency estimate. A proper band-limited filter for the ~20 Hz
    // swarm signal is a TODO; for now we report broadband RMS and a
    // running-mean-subtracted low-frequency proxy.
    const int N = 64;
    double sumSq = 0.0;
    double prev = 0.0, lfSumSq = 0.0;
    for (int i = 0; i < N; i++) {
      sensors_event_t e;
      lis.getEvent(&e);
      // magnitude of (x,y,z) in m/s^2 -> mg
      double mag = sqrt(e.acceleration.x * e.acceleration.x +
                        e.acceleration.y * e.acceleration.y +
                        e.acceleration.z * e.acceleration.z);
      double mg = (mag / 9.80665) * 1000.0;
      sumSq += mg * mg;
      // simple low-pass difference as a low-frequency energy proxy
      double lp = 0.9 * prev + 0.1 * mg;
      lfSumSq += lp * lp;
      prev = lp;
      delayMicroseconds(1000);  // ~1 kHz sampling
    }
    m.accel_rms_mg = sqrt(sumSq / N);
    m.accel_lf_mg  = sqrt(lfSumSq / N);
    m.accel_ok = true;
  }
#endif
}

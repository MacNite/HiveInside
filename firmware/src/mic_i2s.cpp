// mic_i2s.cpp — ICS-43434 capture + FFT band energy.
//
// NOTE: This is a structural stub. The nRF52840 I2S peripheral is driven via
// the Adafruit nRF52 core's I2S support (or nrfx_i2s directly). The FFT math
// mirrors the HiveScale implementation so the band definitions and dBFS scaling
// stay consistent across the ecosystem. Porting the actual I2S DMA capture from
// the ESP-IDF driver used in HiveScale is the next implementation step.
#include "mic_i2s.h"
#include "config.h"

#include <Arduino.h>
#include <arduinoFFT.h>
#include <math.h>

#if ENABLE_MIC

// Helper: enable / disable the power-gated microphone rail.
static void micPower(bool on) {
  pinMode(PIN_MIC_EN, OUTPUT);
  // MOSFET gate is active-LOW (LOW = mic powered).
  digitalWrite(PIN_MIC_EN, on ? LOW : HIGH);
}

// Tristate the I2S lines so a powered-down mic is not back-fed via its pins.
static void micTristateBus() {
  pinMode(PIN_I2S_SCK, INPUT);
  pinMode(PIN_I2S_WS, INPUT);
  pinMode(PIN_I2S_SD, INPUT);
}

// Compute average band energy (dBFS) over [loHz, hiHz] from FFT magnitudes.
static float bandDbfs(const double* mag, int n, int sr,
                      int loHz, int hiHz, double normScale) {
  int loBin = (int)((double)loHz * n / sr);
  int hiBin = (int)((double)hiHz * n / sr);
  if (loBin < 1) loBin = 1;
  if (hiBin > n / 2) hiBin = n / 2;
  if (hiBin <= loBin) return NAN;

  double acc = 0.0;
  for (int i = loBin; i < hiBin; i++) acc += mag[i];
  double avg = acc / (hiBin - loBin);
  double frac = avg / normScale;
  if (frac <= 1e-9) return -180.0f;
  return (float)(20.0 * log10(frac));
}

void micCaptureBands(MicBands& out) {
  out = MicBands();  // reset to NAN / not-ok

  micPower(true);
  delay(MIC_STARTUP_MS);

  // -----------------------------------------------------------------------
  // TODO: capture FFT_SAMPLE_COUNT samples from the ICS-43434 over I2S into a
  // heap buffer here (nRF52840 I2S DMA). For now we bail out cleanly so the
  // rest of the pipeline (climate + accel + BLE) runs end-to-end.
  // -----------------------------------------------------------------------
  bool captured = false;
  // double* samples = ... (heap-allocated, mean-subtracted, scaled to ±1.0)

  if (!captured) {
    micPower(false);
    micTristateBus();
    return;  // out.ok stays false
  }

  // --- FFT path (runs once real capture is wired up) ---------------------
  // static double vReal[FFT_SAMPLE_COUNT];
  // static double vImag[FFT_SAMPLE_COUNT];
  // ArduinoFFT<double> fft(vReal, vImag, FFT_SAMPLE_COUNT, (double)MIC_SAMPLE_RATE_HZ);
  // fft.windowing(FFTWindow::Hann, FFTDirection::Forward);
  // fft.compute(FFTDirection::Forward);
  // fft.complexToMagnitude();
  // const double HANN_GAIN = 0.5;
  // const double normScale = (FFT_SAMPLE_COUNT / 2.0) * HANN_GAIN;
  // out.sub_bass = bandDbfs(vReal, FFT_SAMPLE_COUNT, MIC_SAMPLE_RATE_HZ,
  //                         BAND_SUBBASS_LO, BAND_SUBBASS_HI, normScale);
  // out.hum      = bandDbfs(..., BAND_HUM_LO, BAND_HUM_HI, ...);
  // out.piping   = bandDbfs(..., BAND_PIPING_LO, BAND_PIPING_HI, ...);
  // out.stress   = bandDbfs(..., BAND_STRESS_LO, BAND_STRESS_HI, ...);
  // out.high     = bandDbfs(..., BAND_HIGH_LO, BAND_HIGH_HI, ...);
  // out.ok = true;

  micPower(false);
  micTristateBus();
}

#else  // ENABLE_MIC == 0

void micCaptureBands(MicBands& out) { out = MicBands(); }

#endif

// mic.h — single MP34DT01-style PDM microphone capture + per-band acoustic FFT.
//
// The ESP32-C6 captures raw PDM using its I2S PDM RX peripheral. mic.cpp then
// decimates the raw bitstream to 16-bit PCM before computing broadband RMS,
// peak and the five acoustic FFT bands.
#pragma once

#include "config.h"
#include "measurement.h"

#if ENABLE_MIC

namespace mic {
// Install and start the PDM RX channel. Safe to call repeatedly (no-op once up).
bool begin();
// Stop PDM RX and remove the clock from the microphone.
void end();
// Capture, compute broadband RMS/peak + the five FFT bands into m.mic_*.
void read(Measurement& m);
} // namespace mic

#endif // ENABLE_MIC

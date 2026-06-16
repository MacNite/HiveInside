// mic.h — single INMP441 I2S microphone capture + per-band acoustic FFT.
// Ported from HiveScale firmware/src/mics.cpp, reduced from stereo to mono
// (one INMP441 with L/R tied to GND -> left slot).
#pragma once

#include "config.h"
#include "measurement.h"

#if ENABLE_MIC

namespace mic {
// Install the I2S RX channel. Safe to call repeatedly (no-op once up).
bool begin();
void end();
// Capture, compute broadband RMS/peak + the five FFT bands into m.mic_*.
void read(Measurement& m);
} // namespace mic

#endif // ENABLE_MIC

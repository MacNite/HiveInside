// mic.h — single MP34DT01 / MP34DT06 PDM microphone capture + per-band acoustic
// FFT. Mono: one PDM mic with SEL/LR tied to GND -> left slot. The ESP32-C6 I2S
// peripheral's hardware PDM receiver decimates the 1-bit stream to 16-bit PCM
// (see mic.cpp); the band/FFT analysis matches HiveScale's INMP441 path.
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

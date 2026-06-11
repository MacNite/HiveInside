// mic_i2s.h — ICS-43434 I2S capture + per-band FFT analysis.
//
// Power-gated: the mic VDD is switched by a P-MOSFET on PIN_MIC_EN. The capture
// routine enables the mic, waits MIC_STARTUP_MS, captures, then powers it down
// and tristates the I2S pins to avoid phantom-powering the mic through its ESD
// diodes (series resistors on SCK/WS/SD provide a second line of defence).
#pragma once

#include "measurement.h"

// One-shot: power up mic, capture, compute bands into `out`, power down.
void micCaptureBands(MicBands& out);

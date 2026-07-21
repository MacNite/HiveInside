/* mic.h — on-board PDM microphone capture + acoustic FFT bands. */
#pragma once

#include "measurement.h"

/* Capture ~0.5 s of PCM from the PDM microphone and reduce it to RMS/peak
 * dBFS plus the five shared acoustic bands (sets m->mic_ok). A no-op that
 * leaves mic_ok false when the devicetree has no enabled nRF PDM node. */
void mic_read(struct measurement *m);

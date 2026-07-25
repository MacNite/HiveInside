/* mic.h — on-board PDM microphone sound-level readout. */
#pragma once

#include "measurement.h"

/* Capture ~0.5 s of PCM from the PDM microphone and reduce it to an RMS and
 * peak level in dBFS (sets m->mic_ok). A no-op that leaves mic_ok false when
 * the devicetree has no enabled nRF PDM node. */
void mic_read(struct measurement *m);

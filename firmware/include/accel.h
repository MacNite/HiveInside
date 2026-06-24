// accel.h — low-frequency hive-vibration capture from a single LIS3DH.
//
// Ported from the HiveScale firmware (firmware/src/accel.cpp). The LIS3DH and
// the final-board LIS2DH12 share WHO_AM_I (0x33) and the full register map, so
// this register-level driver drives either unchanged. We capture a block of
// |a| samples at a fixed ODR, remove DC (gravity + mounting bias) and report a
// broadband AC RMS/peak plus the energy in three sub-audible bands — the
// ~20 Hz pre-swarm vibration (Ramsey et al. 2020) the microphone cannot hear.
#pragma once

#include "config.h"
#include "measurement.h"

#if ENABLE_ACCEL

namespace accel {

// Expected WHO_AM_I for LIS3DH / LIS2DH12.
constexpr uint8_t WHO_AM_I_VALUE = 0x33;

// Probe + configure the device. Returns true when WHO_AM_I matched.
bool begin();

// Capture LIS3DH_SAMPLE_COUNT samples and fill m.accel_* (RMS, peak, bands).
// Wakes the sensor, captures the block, then powers it down again.
void read(Measurement& m);

// Put the LIS3DH/LIS2DH12 in power-down mode (CTRL_REG1 ODR = 0).
void sleep();

} // namespace accel

#endif // ENABLE_ACCEL

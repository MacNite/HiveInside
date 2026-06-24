// accel.cpp — LIS3DH low-frequency vibration capture + FFT.
// Ported from HiveScale firmware/src/accel.cpp and reduced to a single device.
#include "accel.h"

#if ENABLE_ACCEL

#include <Wire.h>
#include <math.h>
#include <arduinoFFT.h>

namespace accel {

// ── Register map (LIS3DH and LIS2DH12 identical for everything used here) ──
static constexpr uint8_t REG_WHO_AM_I  = 0x0F;
static constexpr uint8_t REG_CTRL_REG1 = 0x20; // ODR + axis enables
static constexpr uint8_t REG_CTRL_REG4 = 0x23; // BDU, full-scale, high-res
static constexpr uint8_t REG_STATUS    = 0x27; // ZYXDA = new-data ready
static constexpr uint8_t REG_OUT_X_L   = 0x28; // first data register
static constexpr uint8_t AUTO_INCREMENT = 0x80; // multi-byte read bit

static constexpr size_t MAX_FFT_SIZE = 2048;

static bool present = false;

// ── Low-level I2C helpers ──
static bool writeReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(LIS3DH_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

static bool readRegs(uint8_t reg, uint8_t* buf, size_t len) {
  Wire.beginTransmission(LIS3DH_ADDR);
  Wire.write(len > 1 ? (uint8_t)(reg | AUTO_INCREMENT) : reg);
  if (Wire.endTransmission(false) != 0) return false; // repeated start
  size_t got = Wire.requestFrom((int)LIS3DH_ADDR, (int)len);
  if (got != len) return false;
  for (size_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

static uint8_t odrCodeFor(uint16_t hz) {
  switch (hz) {
    case 10:  return 0x2;
    case 25:  return 0x3;
    case 50:  return 0x4;
    case 100: return 0x5;
    case 200: return 0x6;
    case 400: return 0x7;
    default:  return 0x7; // 400 Hz
  }
}

static uint16_t odrHzFor(uint8_t code) {
  switch (code) {
    case 0x2: return 10;
    case 0x3: return 25;
    case 0x4: return 50;
    case 0x5: return 100;
    case 0x6: return 200;
    case 0x7: return 400;
    default:  return 400;
  }
}

static uint8_t fsBitsFor(uint8_t range_g) {
  switch (range_g) {
    case 4:  return 0x1 << 4;
    case 8:  return 0x2 << 4;
    case 16: return 0x3 << 4;
    default: return 0x0 << 4; // ±2 g
  }
}

static float mgPerDigitFor(uint8_t range_g) {
  switch (range_g) {
    case 4:  return 2.0f;
    case 8:  return 4.0f;
    case 16: return 12.0f;
    default: return 1.0f; // ±2 g, high-resolution
  }
}

// RMS (mg) of all FFT bins whose centre frequency falls in [loHz, hiHz].
// normScale undoes the FFT + Hann window gain so a 1 mg in-band sinusoid reads
// ~1 mg.
static float bandRmsMg(const double* mag, size_t fftSize, uint16_t sampleRate,
                       uint16_t loHz, uint16_t hiHz, double normScale) {
  double freqPerBin = (double)sampleRate / (double)fftSize;
  size_t binLo = (size_t)ceil((double)loHz / freqPerBin);
  size_t binHi = (size_t)floor((double)hiHz / freqPerBin);
  size_t nyquist = fftSize / 2;
  if (binLo < 1) binLo = 1; // skip DC bin
  if (binLo >= nyquist) return 0.0f;
  if (binHi >= nyquist) binHi = nyquist - 1;
  double sumSq = 0.0;
  for (size_t b = binLo; b <= binHi; b++) {
    double m = mag[b] / normScale;
    sumSq += m * m;
  }
  if (sumSq <= 0.0) return 0.0f;
  return (float)sqrt(sumSq);
}

void sleep() {
  if (!present) return;
  // LIS3DH/LIS2DH12 power-down: CTRL_REG1 ODR[3:0] = 0000. Axis bits are
  // cleared too, so the sensor draws its power-down current until read()
  // reprograms the ODR before the next capture.
  if (!writeReg(REG_CTRL_REG1, 0x00)) {
    Serial.println("[ACCEL] sleep failed");
  }
}

bool begin() {
  uint8_t who = 0;
  present = readRegs(REG_WHO_AM_I, &who, 1) && who == WHO_AM_I_VALUE;
  Serial.printf("[ACCEL] LIS3DH 0x%02X %s (who=0x%02X)\n", LIS3DH_ADDR,
                present ? "present" : "NOT found", who);
  if (present) sleep();
  return present;
}

void read(Measurement& m) {
  m.accel_ok = false;
  if (!present && !begin()) return;

  const uint8_t odrCode = odrCodeFor(LIS3DH_ODR_HZ);
  const uint16_t odrHz = odrHzFor(odrCode);
  const uint8_t range_g = LIS3DH_RANGE_G;
  const float mgPerDigit = mgPerDigitFor(range_g);

  // Wake for this capture: CTRL_REG1 = ODR | XYZ enabled (normal mode).
  // CTRL_REG4: BDU | FS | HR. The sensor is powered down again before return.
  if (!writeReg(REG_CTRL_REG1, (uint8_t)((odrCode << 4) | 0x07)) ||
      !writeReg(REG_CTRL_REG4, (uint8_t)(0x80 | fsBitsFor(range_g) | 0x08))) {
    Serial.println("[ACCEL] config write failed");
    sleep();
    return;
  }

  m.accel_sample_rate_hz = odrHz;

  size_t want = LIS3DH_SAMPLE_COUNT;
  size_t fftSize = 64;
  while (fftSize * 2 <= want && fftSize * 2 <= MAX_FFT_SIZE) fftSize *= 2;

  double* magnitude = (double*)malloc(fftSize * sizeof(double));
  double* vImag = (double*)malloc(fftSize * sizeof(double));
  if (!magnitude || !vImag) {
    free(magnitude); free(vImag);
    Serial.println("[ACCEL] FFT heap alloc failed");
    sleep();
    return;
  }

  const uint32_t samplePeriodUs = 1000000UL / odrHz;
  delay(20); // let the just-(re)configured ODR settle

  double sum = 0.0;
  size_t n = 0;
  for (; n < fftSize; n++) {
    uint32_t startUs = micros();
    uint8_t status = 0;
    while (true) {
      if (readRegs(REG_STATUS, &status, 1) && (status & 0x08)) break; // ZYXDA
      if (micros() - startUs > samplePeriodUs * 4 + 2000) break;      // bounded
    }
    uint8_t raw[6];
    if (!readRegs(REG_OUT_X_L, raw, 6)) break;
    int16_t xr = (int16_t)((raw[1] << 8) | raw[0]);
    int16_t yr = (int16_t)((raw[3] << 8) | raw[2]);
    int16_t zr = (int16_t)((raw[5] << 8) | raw[4]);
    // 12-bit high-resolution: value left-justified in the 16-bit word.
    float xmg = (float)(xr >> 4) * mgPerDigit;
    float ymg = (float)(yr >> 4) * mgPerDigit;
    float zmg = (float)(zr >> 4) * mgPerDigit;
    double mag = sqrt((double)xmg * xmg + (double)ymg * ymg + (double)zmg * zmg);
    magnitude[n] = mag;
    sum += mag;
  }

  if (n < 64) {
    free(magnitude); free(vImag);
    Serial.printf("[ACCEL] only %u samples; skipping\n", (unsigned)n);
    sleep();
    return;
  }

  // Remove DC (gravity + mounting bias) so bands reflect AC vibration only.
  double mean = sum / (double)n;
  double sumSq = 0.0, peakDev = 0.0;
  for (size_t i = 0; i < n; i++) {
    double ac = magnitude[i] - mean;
    sumSq += ac * ac;
    double dev = fabs(ac);
    if (dev > peakDev) peakDev = dev;
    magnitude[i] = ac;
    vImag[i] = 0.0;
  }
  for (size_t i = n; i < fftSize; i++) { magnitude[i] = 0.0; vImag[i] = 0.0; }

  m.accel_sample_count = (uint16_t)n;
  m.accel_rms_mg = (float)sqrt(sumSq / (double)n);
  m.accel_peak_mg = (float)peakDev;

  ArduinoFFT<double> fft(magnitude, vImag, fftSize, (double)odrHz);
  fft.windowing(FFTWindow::Hann, FFTDirection::Forward);
  fft.compute(FFTDirection::Forward);
  fft.complexToMagnitude();

  const double HANN_COHERENT_GAIN = 0.5;
  const double normScale = (fftSize / 2.0) * HANN_COHERENT_GAIN;

  m.accel_bands.swarm_mg    = bandRmsMg(magnitude, fftSize, odrHz, ACC_BAND_SWARM_LO,    ACC_BAND_SWARM_HI,    normScale);
  m.accel_bands.fanning_mg  = bandRmsMg(magnitude, fftSize, odrHz, ACC_BAND_FANNING_LO,  ACC_BAND_FANNING_HI,  normScale);
  m.accel_bands.activity_mg = bandRmsMg(magnitude, fftSize, odrHz, ACC_BAND_ACTIVITY_LO, ACC_BAND_ACTIVITY_HI, normScale);

  free(magnitude);
  free(vImag);
  m.accel_ok = true;

  Serial.printf("[ACCEL] rms=%.1f peak=%.1f mg | swarm=%.2f fan=%.2f act=%.2f mg (%u@%uHz)\n",
                m.accel_rms_mg, m.accel_peak_mg, m.accel_bands.swarm_mg,
                m.accel_bands.fanning_mg, m.accel_bands.activity_mg,
                (unsigned)m.accel_sample_count, (unsigned)odrHz);

  sleep();
}

} // namespace accel

#endif // ENABLE_ACCEL

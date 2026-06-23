// mic.cpp — MP34DT01 / MP34DT06 PDM microphone capture + FFT (mono).
//
// Replaces the previous INMP441 I2S path. An I2S mic clocked 24-bit samples out
// on a BCLK/WS/SD bus; a PDM mic instead takes a single clock from the MCU and
// returns a 1-bit pulse-density stream on one data line. The ESP32-C6 I2S
// peripheral has a hardware PDM receiver (driver/i2s_pdm.h): it generates the
// PDM clock and runs the incoming bitstream through a CIC decimation filter,
// handing us ready-made 16-bit PCM at MIC_SAMPLE_RATE. So the FFT/band code is
// unchanged apart from the full-scale reference (16-bit now, was 24-bit) and the
// sample width — no per-sample >>8 shift is needed any more.
#include "mic.h"

#if ENABLE_MIC

#include <math.h>
#include <arduinoFFT.h>
#include <driver/i2s_pdm.h>

namespace mic {

static i2s_chan_handle_t rxChan = nullptr;
static bool installed = false;

static constexpr float FULL_SCALE = 32768.0f; // 2^15 — PDM RX delivers 16-bit PCM
static constexpr float SILENCE_DBFS = -200.0f;

// Band energy in dBFS: total power of all bins in [loHz, hiHz] (Parseval sum,
// no /count) so the value reflects real in-band acoustic energy and is
// comparable between bands of different widths. normScale undoes the FFT+Hann
// gain (samples are pre-scaled by full scale in computeBands).
static float bandEnergyDbfs(const double* mag, size_t fftSize, uint32_t sampleRate,
                            uint32_t loHz, uint32_t hiHz, double normScale) {
  double freqPerBin = (double)sampleRate / (double)fftSize;
  size_t binLo = (size_t)ceil((double)loHz / freqPerBin);
  size_t binHi = (size_t)floor((double)hiHz / freqPerBin);
  size_t nyquist = fftSize / 2;
  if (binLo >= nyquist) return SILENCE_DBFS;
  if (binHi >= nyquist) binHi = nyquist - 1;
  double sumSq = 0.0;
  for (size_t b = binLo; b <= binHi; b++) {
    double m = mag[b] / normScale;
    sumSq += m * m;
  }
  if (sumSq <= 0.0) return SILENCE_DBFS;
  return (float)(20.0 * log10(sqrt(sumSq)));
}

static void computeBands(const int16_t* samples, size_t count, MicBands& out) {
  double* vReal = (double*)malloc(MIC_FFT_SAMPLE_COUNT * sizeof(double));
  double* vImag = (double*)malloc(MIC_FFT_SAMPLE_COUNT * sizeof(double));
  if (!vReal || !vImag) {
    free(vReal); free(vImag);
    Serial.println("[MIC] FFT heap alloc failed");
    return;
  }
  size_t n = min(count, (size_t)MIC_FFT_SAMPLE_COUNT);
  // Centre the block on its mean so the PDM mic's residual DC offset doesn't leak
  // through the Hann window side-lobes into the low bands.
  double mean = 0.0;
  for (size_t i = 0; i < n; i++) mean += (double)samples[i];
  if (n > 0) mean /= (double)n;
  for (size_t i = 0; i < n; i++) {
    vReal[i] = ((double)samples[i] - mean) / FULL_SCALE;
    vImag[i] = 0.0;
  }
  for (size_t i = n; i < MIC_FFT_SAMPLE_COUNT; i++) { vReal[i] = 0.0; vImag[i] = 0.0; }

  ArduinoFFT<double> fft(vReal, vImag, MIC_FFT_SAMPLE_COUNT, (double)MIC_SAMPLE_RATE);
  fft.windowing(FFTWindow::Hann, FFTDirection::Forward);
  fft.compute(FFTDirection::Forward);
  fft.complexToMagnitude();

  const double HANN_COHERENT_GAIN = 0.5;
  const double normScale = (MIC_FFT_SAMPLE_COUNT / 2.0) * HANN_COHERENT_GAIN;

  out.sub_bass_dbfs = bandEnergyDbfs(vReal, MIC_FFT_SAMPLE_COUNT, MIC_SAMPLE_RATE, MIC_BAND_SUBBASS_LO, MIC_BAND_SUBBASS_HI, normScale);
  out.hum_dbfs      = bandEnergyDbfs(vReal, MIC_FFT_SAMPLE_COUNT, MIC_SAMPLE_RATE, MIC_BAND_HUM_LO,     MIC_BAND_HUM_HI,     normScale);
  out.piping_dbfs   = bandEnergyDbfs(vReal, MIC_FFT_SAMPLE_COUNT, MIC_SAMPLE_RATE, MIC_BAND_PIPING_LO,  MIC_BAND_PIPING_HI,  normScale);
  out.stress_dbfs   = bandEnergyDbfs(vReal, MIC_FFT_SAMPLE_COUNT, MIC_SAMPLE_RATE, MIC_BAND_STRESS_LO,  MIC_BAND_STRESS_HI,  normScale);
  out.high_dbfs     = bandEnergyDbfs(vReal, MIC_FFT_SAMPLE_COUNT, MIC_SAMPLE_RATE, MIC_BAND_HIGH_LO,    MIC_BAND_HIGH_HI,    normScale);

  free(vReal);
  free(vImag);
}

bool begin() {
  if (installed) return true;

  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_PDM_PORT, I2S_ROLE_MASTER);
  chanCfg.dma_desc_num = 4;
  chanCfg.dma_frame_num = 256;
  chanCfg.auto_clear = false;
  if (i2s_new_channel(&chanCfg, nullptr, &rxChan) != ESP_OK) {
    Serial.println("[MIC] i2s_new_channel failed");
    rxChan = nullptr;
    return false;
  }

  // Mono PDM RX: one mic with SEL/LR -> GND drives the LEFT slot (it outputs its
  // sample while the clock is low). The default PDM-RX clock is
  // MIC_SAMPLE_RATE × 64 (1.024 MHz @ 16 kHz) — inside the MP34DT01/06's
  // 1–3.25 MHz range — and the decimator yields 16-bit PCM.
  i2s_pdm_rx_config_t pdmCfg = {
    .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
    .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .clk = (gpio_num_t)PIN_PDM_CLK,
      .din = (gpio_num_t)PIN_PDM_DIN,
      .invert_flags = { .clk_inv = false },
    },
  };
  // SEL/LR = GND emits the sample in the LEFT slot. Tie SEL to VDD and switch
  // this to I2S_PDM_SLOT_RIGHT to read the other channel.
  pdmCfg.slot_cfg.slot_mask = I2S_PDM_SLOT_LEFT;

  if (i2s_channel_init_pdm_rx_mode(rxChan, &pdmCfg) != ESP_OK) {
    Serial.println("[MIC] init_pdm_rx_mode failed");
    i2s_del_channel(rxChan); rxChan = nullptr;
    return false;
  }
  if (i2s_channel_enable(rxChan) != ESP_OK) {
    Serial.println("[MIC] channel_enable failed");
    i2s_del_channel(rxChan); rxChan = nullptr;
    return false;
  }
  installed = true;
  Serial.printf("[MIC] PDM up: CLK=%d DIN=%d rate=%d\n",
                PIN_PDM_CLK, PIN_PDM_DIN, MIC_SAMPLE_RATE);
  return true;
}

void end() {
  if (!installed) return;
  i2s_channel_disable(rxChan);
  i2s_del_channel(rxChan);
  rxChan = nullptr;
  installed = false;
}

void read(Measurement& m) {
  m.mic_ok = false;
  if (!begin()) return;
  m.mic_sample_rate_hz = MIC_SAMPLE_RATE;

  // Settling: discard the first frames so the PDM decimation filter and the
  // mic's internal high-pass stabilise.
  const size_t WARMUP_FRAMES = 256;
  {
    int16_t* warmup = (int16_t*)malloc(WARMUP_FRAMES * sizeof(int16_t));
    if (warmup) {
      size_t got = 0;
      i2s_channel_read(rxChan, warmup, WARMUP_FRAMES * sizeof(int16_t), &got, 500);
      free(warmup);
    }
  }

  int16_t* fftBuf = (int16_t*)malloc(MIC_FFT_SAMPLE_COUNT * sizeof(int16_t));
  const size_t CHUNK_FRAMES = 512;
  int16_t* chunk = (int16_t*)malloc(CHUNK_FRAMES * sizeof(int16_t));
  if (!chunk) {
    Serial.println("[MIC] chunk alloc failed");
    free(fftBuf);
    return;
  }

  double sum = 0.0, sumSq = 0.0;
  int32_t mn = INT32_MAX, mx = INT32_MIN;
  uint32_t count = 0;
  size_t fftCount = 0;
  uint32_t framesRemaining = MIC_SAMPLE_FRAMES;

  while (framesRemaining > 0) {
    size_t frames = framesRemaining > CHUNK_FRAMES ? CHUNK_FRAMES : framesRemaining;
    size_t bytesRead = 0;
    if (i2s_channel_read(rxChan, chunk, frames * sizeof(int16_t), &bytesRead, 1000) != ESP_OK || bytesRead == 0)
      break;
    size_t framesRead = bytesRead / sizeof(int16_t);
    for (size_t i = 0; i < framesRead; i++) {
      int32_t s = chunk[i]; // 16-bit signed PCM straight from the PDM decimator
      double f = (double)s;
      sum += f;
      sumSq += f * f;
      if (s > mx) mx = s;
      if (s < mn) mn = s;
      count++;
      if (fftBuf && fftCount < MIC_FFT_SAMPLE_COUNT) fftBuf[fftCount++] = (int16_t)s;
    }
    framesRemaining -= framesRead;
    if (framesRead == 0) break;
  }
  free(chunk);

  if (count == 0) {
    free(fftBuf);
    Serial.println("[MIC] no samples");
    return;
  }

  // Remove DC offset; AC power = variance = mean-of-squares − square-of-mean.
  double mean = sum / (double)count;
  double variance = sumSq / (double)count - mean * mean;
  if (variance < 0.0) variance = 0.0;
  double rms = sqrt(variance);
  m.mic_rms_normalized = (float)(rms / FULL_SCALE);
  m.mic_rms_dbfs = m.mic_rms_normalized > 0.0f
                   ? (float)(20.0 * log10((double)m.mic_rms_normalized))
                   : SILENCE_DBFS;
  // Peak relative to the DC mean, at one of the signal extremes.
  double devHi = (double)mx - mean;
  double devLo = mean - (double)mn;
  double peakDev = devHi > devLo ? devHi : devLo;
  m.mic_peak_dbfs = peakDev > 0.0 ? (float)(20.0 * log10(peakDev / FULL_SCALE)) : SILENCE_DBFS;
  m.mic_ok = true;

  if (fftBuf && fftCount >= 64) computeBands(fftBuf, fftCount, m.mic_bands);
  free(fftBuf);

  Serial.printf("[MIC] rms=%.1f peak=%.1f dBFS | sub=%.1f hum=%.1f pipe=%.1f stress=%.1f hi=%.1f\n",
                m.mic_rms_dbfs, m.mic_peak_dbfs, m.mic_bands.sub_bass_dbfs,
                m.mic_bands.hum_dbfs, m.mic_bands.piping_dbfs,
                m.mic_bands.stress_dbfs, m.mic_bands.high_dbfs);
}

} // namespace mic

#endif // ENABLE_MIC

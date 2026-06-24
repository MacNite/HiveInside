// mic.cpp — MP34DT01-style PDM microphone capture + acoustic FFT (mono).
//
// The ESP32-C6 exposes PDM through the IDF 5.x channel-based I2S PDM driver
// (driver/i2s_pdm.h). Unlike ESP32/S3, the C6 has no PDM-to-PCM RX converter,
// so the peripheral captures raw PDM words and this file runs a small sinc^3
// (CIC) decimator in software before the existing dBFS + FFT band analysis.
#include "mic.h"

#if ENABLE_MIC

#include <math.h>
#include <stdint.h>
#include <arduinoFFT.h>
#include <driver/i2s_pdm.h>
#include <esp_err.h>
#include <soc/soc_caps.h>

#ifndef SOC_I2S_SUPPORTS_PDM_RX
#define SOC_I2S_SUPPORTS_PDM_RX 0
#endif

#if !SOC_I2S_SUPPORTS_PDM_RX
#error "This firmware requires ESP-IDF / Arduino-ESP32 with I2S PDM RX support."
#endif

namespace mic {

static i2s_chan_handle_t rxChan = nullptr;
static bool installed = false;

static constexpr float FULL_SCALE = 32768.0f; // 16-bit PCM after PDM decimation
static constexpr float SILENCE_DBFS = -200.0f;
static constexpr uint32_t PCM_RATE_HZ = MIC_PDM_RAW_SAMPLE_RATE / MIC_PDM_DECIMATION;
static constexpr int64_t CIC_FULL_SCALE =
    (int64_t)MIC_PDM_DECIMATION * (int64_t)MIC_PDM_DECIMATION * (int64_t)MIC_PDM_DECIMATION;

static_assert(MIC_PDM_DECIMATION > 0, "MIC_PDM_DECIMATION must be positive");
static_assert((MIC_PDM_DECIMATION % 16) == 0,
              "MIC_PDM_DECIMATION should be a multiple of 16 raw PDM bits");
static_assert(PCM_RATE_HZ == MIC_SAMPLE_RATE,
              "MIC_PDM_RAW_SAMPLE_RATE must equal MIC_SAMPLE_RATE * MIC_PDM_DECIMATION");

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
  // Centre the block on its mean so microphone DC / PDM duty-cycle bias does
  // not leak through the Hann window side-lobes into the low bands.
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

// Third-order CIC/sinc decimator. It is intentionally tiny: raw PDM is read by
// the ESP32-C6 I2S PDM RX hardware, then every MIC_PDM_DECIMATION one-bit
// samples become one signed 16-bit PCM sample for the existing FFT path.
class PdmCicDecimator {
 public:
  bool pushBit(bool one, int16_t& out) {
#if MIC_PDM_INVERT
    one = !one;
#endif
    const int32_t x = one ? 1 : -1;
    i1 += x;
    i2 += i1;
    i3 += i2;
    if (++phase < MIC_PDM_DECIMATION) return false;
    phase = 0;

    const int64_t c1 = i3 - z1; z1 = i3;
    const int64_t c2 = c1 - z2; z2 = c1;
    const int64_t c3 = c2 - z3; z3 = c2;

    int64_t scaled = (c3 * 32767LL * (int64_t)MIC_PDM_GAIN_Q8) /
                     (CIC_FULL_SCALE * 256LL);
    if (scaled > INT16_MAX) scaled = INT16_MAX;
    if (scaled < INT16_MIN) scaled = INT16_MIN;
    out = (int16_t)scaled;
    return true;
  }

 private:
  uint16_t phase = 0;
  int64_t i1 = 0, i2 = 0, i3 = 0;
  int64_t z1 = 0, z2 = 0, z3 = 0;
};

static inline void pushWord(PdmCicDecimator& decim, uint16_t word,
                            uint32_t& warmupLeft, int16_t* fftBuf,
                            size_t& fftCount, double& sum, double& sumSq,
                            int16_t& mn, int16_t& mx, uint32_t& count) {
#if MIC_PDM_MSB_FIRST
  for (int bit = 15; bit >= 0; bit--) {
#else
  for (int bit = 0; bit < 16; bit++) {
#endif
    int16_t s = 0;
    if (!decim.pushBit(((word >> bit) & 0x1) != 0, s)) continue;

    if (warmupLeft > 0) {
      warmupLeft--;
      continue;
    }

    double f = (double)s;
    sum += f;
    sumSq += f * f;
    if (s > mx) mx = s;
    if (s < mn) mn = s;
    if (fftBuf && fftCount < MIC_FFT_SAMPLE_COUNT) fftBuf[fftCount++] = s;
    count++;
  }
}

bool begin() {
  if (installed) return true;

  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_PORT, I2S_ROLE_MASTER);
  chanCfg.dma_desc_num = 8;
  chanCfg.dma_frame_num = 512;
  chanCfg.auto_clear = false;

  esp_err_t err = i2s_new_channel(&chanCfg, nullptr, &rxChan);
  if (err != ESP_OK) {
    Serial.printf("[MIC] i2s_new_channel failed: %s\n", esp_err_to_name(err));
    rxChan = nullptr;
    return false;
  }

  i2s_pdm_rx_config_t pdmCfg = {
    .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(MIC_PDM_RAW_SAMPLE_RATE),
    .slot_cfg = I2S_PDM_RX_SLOT_RAW_FMT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .clk = (gpio_num_t)PIN_PDM_CLK,
#if SOC_I2S_PDM_MAX_RX_LINES == 4
      .dins = {(gpio_num_t)PIN_PDM_DATA, I2S_GPIO_UNUSED, I2S_GPIO_UNUSED, I2S_GPIO_UNUSED},
#else
      .din = (gpio_num_t)PIN_PDM_DATA,
#endif
      .invert_flags = {.clk_inv = MIC_PDM_CLK_INVERT},
    },
  };
  pdmCfg.slot_cfg.slot_mask = MIC_PDM_SLOT_MASK;

  err = i2s_channel_init_pdm_rx_mode(rxChan, &pdmCfg);
  if (err != ESP_OK) {
    Serial.printf("[MIC] init_pdm_rx_mode failed: %s\n", esp_err_to_name(err));
    i2s_del_channel(rxChan); rxChan = nullptr;
    return false;
  }
  err = i2s_channel_enable(rxChan);
  if (err != ESP_OK) {
    Serial.printf("[MIC] channel_enable failed: %s\n", esp_err_to_name(err));
    i2s_del_channel(rxChan); rxChan = nullptr;
    return false;
  }

  installed = true;
  Serial.printf("[MIC] PDM RX up: CLK=%d DATA=%d raw=%luHz decim=%u pcm=%luHz\n",
                PIN_PDM_CLK, PIN_PDM_DATA,
                (unsigned long)MIC_PDM_RAW_SAMPLE_RATE,
                (unsigned)MIC_PDM_DECIMATION,
                (unsigned long)MIC_SAMPLE_RATE);
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

  int16_t* fftBuf = (int16_t*)malloc(MIC_FFT_SAMPLE_COUNT * sizeof(int16_t));
  const size_t CHUNK_WORDS = 1024;
  uint16_t* chunk = (uint16_t*)malloc(CHUNK_WORDS * sizeof(uint16_t));
  if (!chunk) {
    Serial.println("[MIC] raw PDM chunk alloc failed");
    free(fftBuf);
    end();
    return;
  }

  PdmCicDecimator decim;
  uint32_t warmupLeft = MIC_PDM_WARMUP_FRAMES;
  double sum = 0.0, sumSq = 0.0;
  int16_t mn = INT16_MAX, mx = INT16_MIN;
  uint32_t count = 0;
  size_t fftCount = 0;

  while (count < MIC_SAMPLE_FRAMES) {
    size_t bytesRead = 0;
    esp_err_t err = i2s_channel_read(rxChan, chunk, CHUNK_WORDS * sizeof(uint16_t),
                                     &bytesRead, 1000);
    if (err != ESP_OK || bytesRead == 0) {
      Serial.printf("[MIC] raw PDM read stopped: %s, %u bytes\n",
                    esp_err_to_name(err), (unsigned)bytesRead);
      break;
    }

    size_t wordsRead = bytesRead / sizeof(uint16_t);
    for (size_t i = 0; i < wordsRead && count < MIC_SAMPLE_FRAMES; i++) {
      pushWord(decim, chunk[i], warmupLeft, fftBuf, fftCount,
               sum, sumSq, mn, mx, count);
    }
  }
  free(chunk);

  // Stop PDM CLK between captures; the MP34DT01 consumes power whenever it is
  // clocked, and the next read() will re-enable the channel.
  end();

  if (count == 0) {
    free(fftBuf);
    Serial.println("[MIC] no decoded PCM samples");
    return;
  }

  // Remove DC offset; AC power = variance = mean-of-squares - square-of-mean.
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

  Serial.printf("[MIC] rms=%.1f peak=%.1f dBFS | sub=%.1f hum=%.1f pipe=%.1f stress=%.1f hi=%.1f (%lu decoded)\n",
                m.mic_rms_dbfs, m.mic_peak_dbfs, m.mic_bands.sub_bass_dbfs,
                m.mic_bands.hum_dbfs, m.mic_bands.piping_dbfs,
                m.mic_bands.stress_dbfs, m.mic_bands.high_dbfs,
                (unsigned long)count);
}

} // namespace mic

#endif // ENABLE_MIC

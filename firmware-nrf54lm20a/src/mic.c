/* mic.c — see mic.h.
 *
 * The nRF54's PDM peripheral filters PDM to 16-bit PCM in hardware, so this
 * module only gathers PCM blocks through Zephyr's dmic API and reduces them to
 * an AC RMS and peak level (dBFS). No FFT — target 1 just needs a sound level
 * to prove the microphone works.
 *
 * The PDM clock only runs between DMIC_TRIGGER_START and _STOP, so the
 * microphone (powered from nPM1300 LDO1 — see power.c) draws its idle current
 * outside the ~0.7 s capture.
 *
 * The PDM controller is resolved by compatible, not by node label: any enabled
 * `nordic,nrf-pdm` node is used (the Sense board wires it to the on-board
 * MSM261DGT006). If the devicetree has none enabled, the module compiles to a
 * stub and the readout reports the mic absent.
 */
#include "mic.h"

#include "hive_config.h"

#include <math.h>
#include <zephyr/audio/dmic.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#if ENABLE_MIC && DT_HAS_COMPAT_STATUS_OKAY(nordic_nrf_pdm)

#define MIC_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(nordic_nrf_pdm)

#define BLOCK_BYTES     (MIC_BLOCK_SAMPLES * sizeof(int16_t))
#define BLOCK_COUNT     4
#define READ_TIMEOUT_MS 1000

#define FULL_SCALE   32768.0f /* 16-bit PCM */
#define SILENCE_DBFS (-200.0f)

static const struct device *const dmic_dev = DEVICE_DT_GET(MIC_NODE);

K_MEM_SLAB_DEFINE_STATIC(mic_slab, BLOCK_BYTES, BLOCK_COUNT, 4);

void mic_read(struct measurement *m)
{
	m->mic_ok = false;

	if (!device_is_ready(dmic_dev)) {
		printk("[MIC] PDM device not ready\n");
		return;
	}

	struct pcm_stream_cfg stream = {
		.pcm_rate = MIC_SAMPLE_RATE,
		.pcm_width = 16,
		.block_size = BLOCK_BYTES,
		.mem_slab = &mic_slab,
	};
	struct dmic_cfg cfg = {
		.io = {
			/* Clock window covering MSM261DGT006-class mics. */
			.min_pdm_clk_freq = 1000000,
			.max_pdm_clk_freq = 3500000,
			.min_pdm_clk_dc = 40,
			.max_pdm_clk_dc = 60,
		},
		.streams = &stream,
		.channel = {
			.req_num_streams = 1,
			.req_num_chan = 1,
		},
	};

	cfg.channel.req_chan_map_lo = dmic_build_channel_map(0, 0, PDM_CHAN_LEFT);

	if (dmic_configure(dmic_dev, &cfg) != 0) {
		printk("[MIC] dmic_configure failed\n");
		return;
	}
	if (dmic_trigger(dmic_dev, DMIC_TRIGGER_START) != 0) {
		printk("[MIC] start failed\n");
		return;
	}

	float sum = 0.0f, sum_sq = 0.0f;
	int16_t mn = INT16_MAX, mx = INT16_MIN;
	uint32_t count = 0;
	unsigned block = 0;

	while (count < MIC_SAMPLE_FRAMES) {
		void *buf;
		uint32_t size;

		if (dmic_read(dmic_dev, 0, &buf, &size, READ_TIMEOUT_MS) != 0) {
			printk("[MIC] read stopped after %u frames\n",
			       (unsigned)count);
			break;
		}

		if (block++ < MIC_WARMUP_BLOCKS) {
			/* Discard: PDM filter + mic output still settling. */
			k_mem_slab_free(&mic_slab, buf);
			continue;
		}

		const int16_t *pcm = buf;
		size_t frames = size / sizeof(int16_t);

		for (size_t i = 0; i < frames && count < MIC_SAMPLE_FRAMES; i++) {
			int16_t s = pcm[i];
			float f = (float)s;

			sum += f;
			sum_sq += f * f;
			if (s > mx) {
				mx = s;
			}
			if (s < mn) {
				mn = s;
			}
			count++;
		}
		k_mem_slab_free(&mic_slab, buf);
	}

	/* Stop the PDM clock between captures — the microphone consumes power
	 * whenever it is clocked. */
	dmic_trigger(dmic_dev, DMIC_TRIGGER_STOP);

	if (count == 0) {
		printk("[MIC] no PCM samples\n");
		return;
	}

	/* Remove DC offset; AC power = variance. */
	float mean = sum / (float)count;
	float variance = sum_sq / (float)count - mean * mean;

	if (variance < 0.0f) {
		variance = 0.0f;
	}
	float rms_norm = sqrtf(variance) / FULL_SCALE;

	m->mic_rms_dbfs = rms_norm > 0.0f ? 20.0f * log10f(rms_norm)
					  : SILENCE_DBFS;

	float dev_hi = (float)mx - mean;
	float dev_lo = mean - (float)mn;
	float peak_dev = dev_hi > dev_lo ? dev_hi : dev_lo;

	m->mic_peak_dbfs = peak_dev > 0.0f
				   ? 20.0f * log10f(peak_dev / FULL_SCALE)
				   : SILENCE_DBFS;
	m->mic_frames = count;
	m->mic_ok = true;
}

#else /* !ENABLE_MIC or no nordic,nrf-pdm node enabled in the devicetree */

void mic_read(struct measurement *m)
{
	m->mic_ok = false;
#if ENABLE_MIC
	printk("[MIC] no enabled nordic,nrf-pdm devicetree node — mic absent\n");
#endif
}

#endif

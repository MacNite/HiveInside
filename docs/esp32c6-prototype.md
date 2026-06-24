# HiveInside — XIAO ESP32-C6 prototype

A breadboard-friendly bring-up of the HiveInside sensor suite on cheap modules,
**before** the XIAO nRF52840 final board. It captures LIS3DH/LIS2DH12 vibration,
MP34DT01-compatible PDM microphone audio and SHT40 climate readings, then exposes
the processed measurement over a **connectable BLE GATT server**. Firmware:
[`firmware/`](../firmware).

## Parts

| Part | Role | Interface |
|---|---|---|
| [Seeed XIAO ESP32-C6](https://s.click.aliexpress.com/e/_c43qaNVb) | MCU + BLE 5 radio, USB-C, LiPo charger | — |
| LIS3DH breakout (GY-LIS3DH) | 3-axis vibration → swarm prediction | I²C |
| SHT40 | temperature + humidity | I²C |
| MP34DT01 / compatible one-bit PDM MEMS mic | acoustic FFT (colony hum, piping, stress) | PDM CLK + DATA |

## What it returns

Vibration and acoustics are analysed exactly like HiveScale, so the band
definitions match across the ecosystem:

- **Acoustic FFT bands (dBFS)** + broadband RMS/peak: sub-bass 50–150, hum
  150–300, piping 300–550, stress 550–1500, high 1500–3000 Hz. The ESP32-C6
  firmware captures raw PDM through I²S PDM RX, decimates it to PCM in firmware,
  then feeds the existing FFT path.
- **Vibration FFT bands (mg)** + broadband RMS/peak: swarm 8–30, fanning 30–100,
  activity 100–200 Hz. The 8–30 Hz band is the ~20 Hz pre-swarm signal
  (Ramsey et al. 2020) the microphone cannot hear.
- **Temperature / humidity** (SHT40, low precision, no heater).
- **Battery** voltage + rough percentage (optional; see below).

## Wiring (XIAO ESP32-C6 — override in `platformio.ini`)

| Signal | XIAO pad | GPIO | Connects to |
|---|---|---|---|
| I²C SDA | D4 | 22 | SHT40 + LIS3DH SDA |
| I²C SCL | D5 | 23 | SHT40 + LIS3DH SCL |
| PDM CLK | D6 | 16 | MP34DT01 CLK / CK |
| PDM DATA | D8 | 19 | MP34DT01 DOUT / DATA |
| Battery sense | A0 | 0 | external divider midpoint |
| Button | — | 9 | on-board BOOT button (reused) |
| LED | — | 15 | on-board LED (heartbeat) |

> GPIO3 / GPIO14 drive the XIAO's internal RF switch and are **not** on the
> header — don't use them. The default I²C pads (D4/D5) and PDM pads (D6/D8)
> above avoid them. D7/GPIO17 is no longer used by the microphone because PDM has
> no word-select line.

LIS3DH straps: `SDO/SA0 → GND` = I²C address **0x18** (firmware default; set
`-DLIS3DH_ADDR=0x19` for VCC). Leave `CS` high so the breakout stays in I²C
mode. For MP34DT01-style breakouts, connect `VDD` to 3V3, `GND` to GND, `CLK` to
`PIN_PDM_CLK` and `DOUT`/`DATA` to `PIN_PDM_DATA`. If your breakout exposes a
left/right or select pin, strap it for the default left-slot/phase or override
`MIC_PDM_SLOT_MASK` in `platformio.ini`. Add **4.7 kΩ pull-ups** on SDA/SCL to
3V3 if your breakouts lack them.

## PDM microphone capture

The firmware no longer uses the standard-I²S INMP441 path (`BCLK`/`WS`/`SD`). It
uses the ESP32-C6 I²S PDM RX driver (`driver/i2s_pdm.h`) instead:

| Build flag / config | Default | Meaning |
|---|---:|---|
| `PIN_PDM_CLK` | `16` / D6 | PDM clock driven by the ESP32-C6 |
| `PIN_PDM_DATA` | `19` / D8 | one-bit PDM data input |
| `MIC_SAMPLE_RATE` | `16000` | decoded PCM rate used by RMS/FFT |
| `MIC_SAMPLE_FRAMES` | `8000` | broadband RMS/peak capture length |
| `MIC_FFT_SAMPLE_COUNT` | `2048` | FFT block size |
| `MIC_PDM_DECIMATION` | `128` | raw PDM bits per decoded PCM sample |
| `MIC_PDM_RAW_SAMPLE_RATE` | `2048000` | microphone PDM clock with defaults |
| `MIC_PDM_GAIN_Q8` | `256` | post-decimator gain, Q8 (`256` = 1.0×) |
| `MIC_PDM_WARMUP_FRAMES` | `256` | decoded frames discarded after enabling CLK |
| `MIC_PDM_CLK_INVERT` | `0` | invert PDM clock if required by wiring/scope check |
| `MIC_PDM_INVERT` | `0` | invert one-bit PDM polarity before decimation |
| `MIC_PDM_MSB_FIRST` | `1` | raw bit order inside each 16-bit DMA word |
| `MIC_PDM_SLOT_MASK` | `I2S_PDM_SLOT_LEFT` | PDM phase/slot selected by mic strap |

ESP32-C6 I²S can clock and DMA-capture raw PDM, but the C6 target does not expose
a hardware PDM-to-PCM RX converter in the ESP-IDF target matrix. The firmware
therefore converts raw PDM with a small third-order CIC/sinc decimator, removes
DC, computes RMS/peak and then runs the five acoustic FFT bands. With the
defaults, `2.048 MHz / 128 = 16 kHz` decoded PCM.

## Sensor power / precision behaviour

- `accel::begin()` probes the LIS3DH/LIS2DH12 and leaves it in power-down mode.
- `accel::read()` wakes the accelerometer by programming `CTRL_REG1`/`CTRL_REG4`,
  captures the vibration block, computes RMS/peak + bands, then calls
  `accel::sleep()` so `CTRL_REG1` returns to power-down (`ODR = 0`).
- `main.cpp` also calls `accel::sleep()` before deep sleep as a safety net.
- `sht40::begin()` sets `SHT4X_LOW_PRECISION` and `SHT4X_NO_HEATER` to reduce
  conversion time and energy per wake.
- `mic::read()` stops and deletes the PDM RX channel after each capture, removing
  the PDM clock between acoustic measurement windows.

## BLE: connectable GATT server

The device runs as a connectable **GATT server**: standard Battery (0x180F) +
Environmental Sensing (0x181A) services for generic clients, plus a custom
HiveInside service whose JSON characteristic returns the **entire** measurement
(every band). Read once or subscribe to notifications.

```bash
pio run -e c6_gatt -t upload
```

It refreshes the characteristics every `MEASURE_INTERVAL_MS` (default 5 min;
press the BOOT button to force a refresh). Read the `8e8b0002-…` characteristic
for the full JSON.

### Wake synchronisation

When `HIVEINSIDE_SYNC_ENABLED` (default) is set **with** `DEEP_SLEEP_ENABLED`,
HiveScale is the schedule master. The custom service adds a **writable**
characteristic:

| UUID | Properties | Payload |
|---|---|---|
| `8e8b0002-…` | read / notify | full measurement JSON |
| `8e8b0003-…` | write / read | `uint32` LE — seconds to deep-sleep before the next connection |

Each cycle HiveScale connects, reads `8e8b0002-…`, then writes the next sleep
duration to `8e8b0003-…` (computed from its own send interval, so it tracks
interval changes automatically). HiveInside then sleeps that long instead of
`MEASURE_INTERVAL_MS`, waking just before HiveScale's next scan rather than
staying connectable continuously.

Both deep-sleep timers free-run on the internal ~150 kHz RC oscillator, which
drifts with temperature, so the rendezvous has slack: HiveScale subtracts
`HIVEINSIDE_SYNC_LEAD_S` (60 s) so HiveInside wakes *before* the scan, and
HiveInside then stays connectable for `SYNC_LISTEN_MS` (150 s) in case HiveScale
is late. Once the central writes the hint and disconnects, HiveInside sleeps
immediately — so the long window costs extra awake time **only on a missed
cycle**. On a miss with no write, HiveInside reuses the **last cadence** from RTC
memory (not `MEASURE_INTERVAL_MS`) so both devices stay on the same period while
they re-acquire. Received values are clamped to
`[SYNC_MIN_SLEEP_MS, SYNC_MAX_SLEEP_MS]`. Tune the 60 s / 150 s pair from the
`[SYNC] … awake Nms` serial logs once you have field drift numbers.

### Serial output

The XIAO's USB-C is the C6's **native USB Serial/JTAG**, so the build enables
`ARDUINO_USB_CDC_ON_BOOT` (in `platformio.ini`) to route `Serial` there. Open the
monitor at 115200 baud (`pio device monitor`). Without that flag, `Serial.*` goes
to the UART0 pins and nothing appears over USB.

## Battery monitoring

The XIAO ESP32-C6 **charges** a LiPo over USB-C (on-board charger) via its
BAT+/BAT- pads — no external charger needed. But it has **no battery-sense
divider**, so to *read* the cell add an external divider (e.g. 2×220 kΩ) from
BAT+ to **A0** (`PIN_VBAT_ADC`, GPIO0); the firmware reads it with
`analogReadMilliVolts()`. Set `VBAT_DIVIDER` to your ratio (2.0 for equal
resistors). To skip battery telemetry, build with `-DENABLE_BATTERY=0`.

> The final XIAO nRF52840 board reads the cell on-board (no external divider), so
> this divider is a prototype-only workaround.

## Measurement JSON (characteristic `8e8b0002-…`)

The full measurement is a compact JSON string (read or notify), trimmed under the
512-byte ATT limit to carry exactly the fields the HiveScale GATT reader
(`firmware/src/ble_sensor.cpp::gattReadHiveInside`) consumes:

| Field | Units |
|---|---|
| `fw` | firmware version string |
| `packet_id` | uint8, de-duplication |
| `temp_c` | °C |
| `humidity_percent` | % |
| `accel_ok` | bool |
| `accel_rms_mg`, `accel_peak_mg` | mg |
| `accel_band_swarm_mg` (8–30 Hz) | mg |
| `accel_band_fanning_mg` (30–100 Hz) | mg |
| `accel_band_activity_mg` (100–200 Hz) | mg |
| `mic_ok` | bool |
| `mic_rms_dbfs` | dBFS |
| `mic_band_sub_bass_dbfs` (50–150 Hz) | dBFS |
| `mic_band_hum_dbfs` (150–300 Hz) | dBFS |
| `mic_band_piping_dbfs` (300–550 Hz) | dBFS |
| `mic_band_stress_dbfs` (550–1500 Hz) | dBFS |
| `mic_band_high_dbfs` (1500–3000 Hz) | dBFS |
| `battery_percent` | % |

Temperature, humidity and battery are also exposed on the standard
Environmental-Sensing (0x181A) and Battery (0x180F) services for generic clients.
`NaN` fields serialise as `null` ("field absent").

## Status & caveats

🚧 **Prototype, not yet hardware-validated.** The BLE layer (NimBLE 2.x on the
C6), PDM microphone path and XIAO pin map need a bench check. Calibrate
`VBAT_DIVIDER` against your divider, verify the GATT characteristics in nRF
Connect on first flash, and tune the PDM mic noise floor / absolute dBFS scaling
with the actual breakout. If NimBLE fails to init on your core version, update
the `h2zero/NimBLE-Arduino` pin in `platformio.ini`.

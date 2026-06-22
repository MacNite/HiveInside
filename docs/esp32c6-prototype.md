# HiveInside — ESP32-C6 BLE prototype

A quick, breadboard-friendly bring-up of the HiveInside sensor suite on cheap
modules, **before** the nRF52840 production PCB. It ports the proven LIS3DH and
INMP441 FFT code from [HiveScale](https://github.com/MacNite/HiveScale) and
exposes the readings over a **connectable BLE GATT server**.

Project: [`firmware-esp32c6/`](../firmware-esp32c6).

## Parts

| Part | Role | Interface |
|---|---|---|
| ESP32-C6 SuperMini | MCU + BLE 5 radio | — |
| LIS3DH breakout (GY-LIS3DH) | 3-axis vibration → swarm prediction | I²C |
| SHT40 | temperature + humidity | I²C |
| INMP441 | acoustic FFT (MEMS mic) | I²S |

## What it returns

Both vibration and acoustics are analysed exactly like HiveScale, so the band
definitions match across the ecosystem:

- **Acoustic FFT bands (dBFS)** + broadband RMS/peak: sub-bass 50–150, hum
  150–300, piping 300–550, stress 550–1500, high 1500–3000 Hz.
- **Vibration FFT bands (mg)** + broadband RMS/peak: swarm 8–30, fanning
  30–100, activity 100–200 Hz. The 8–30 Hz band is the ~20 Hz pre-swarm
  vibration (Ramsey et al. 2020) that the microphone cannot hear.
- **Temperature / humidity** (SHT40).
- **Battery** voltage + rough percentage (optional; see below).

## BLE: connectable GATT server

The device runs as a connectable **GATT server**: standard Battery (0x180F) +
Environmental Sensing (0x181A) services for generic clients, plus a custom
HiveInside service whose JSON characteristic returns the **entire** measurement
(every band). Read once or subscribe to notifications.

```bash
pio run -e c6_gatt -t upload   # connectable GATT server
```

The device stays advertising as connectable and refreshes the characteristics
every `MEASURE_INTERVAL_MS` (default 5 min — press the BOOT button to force an
immediate refresh). Connect with nRF Connect and read the `8e8b0002-…`
characteristic for the full JSON, or subscribe for notifications.

### Wake synchronisation

When `HIVEINSIDE_SYNC_ENABLED` is set (default) **with** `DEEP_SLEEP_ENABLED`,
HiveScale acts as the schedule master. The custom HiveInside service exposes a
third, **writable** characteristic:

| UUID | Properties | Payload |
|---|---|---|
| `8e8b0002-…` | read / notify | full measurement JSON |
| `8e8b0003-…` | write / read | `uint32` little-endian — seconds to deep-sleep before the next connection |

Each cycle HiveScale connects, reads `8e8b0002-…`, then writes the next sleep
duration to `8e8b0003-…` (computed from its own send interval, so it tracks
remote interval changes automatically). On the following deep sleep HiveInside
honours that value instead of `MEASURE_INTERVAL_MS`, waking just before
HiveScale's next scan rather than staying connectable continuously.

Both deep-sleep timers free-run on the internal ~150 kHz RC oscillator, which
drifts several percent with temperature, so the rendezvous has slack on both
sides. HiveScale subtracts `HIVEINSIDE_SYNC_LEAD_S` (60 s) from the interval so
HiveInside wakes *before* the scan (covers HiveInside waking late), and HiveInside
then stays connectable for `SYNC_LISTEN_MS` (150 s) so it is still up if HiveScale
arrives late (covers HiveInside waking early). HiveScale also anchors its own
sleep to the start of each boot (not the variable end of the upload cycle), so the
scan recurs on a stable cadence. Once the central has written the hint and
disconnected, HiveInside sleeps immediately — the long listen window therefore
costs extra awake time **only on a missed cycle**, not on a healthy one.

If no value is written during a wake (HiveScale missed the connection), HiveInside
falls back to the **last cadence HiveScale gave it** (retained in RTC memory across
deep sleep) rather than `MEASURE_INTERVAL_MS`, so a single miss keeps both devices
on the same period while they re-acquire instead of drifting onto two different
intervals. `MEASURE_INTERVAL_MS` only applies before the very first sync after a
cold boot. Received values are clamped to `[SYNC_MIN_SLEEP_MS, SYNC_MAX_SLEEP_MS]`.

The `[SYNC] … awake Nms` serial line on HiveInside and the `wake-sync written …
at boot+Nms` / `Scanning … at boot+Nms` lines on HiveScale let you measure the
real per-cycle drift in the field and re-tune the 60 s / 150 s pair if needed.

### Serial output

The SuperMini's USB-C is the C6's **native USB Serial/JTAG**, so the build
enables `ARDUINO_USB_CDC_ON_BOOT` (in `platformio.ini`) to route `Serial` there.
Open the monitor at 115200 baud (`pio device monitor` or `pio run -t monitor`).
Without that flag, `Serial.*` goes to the UART0 TX/RX pins instead and nothing
appears over USB.

## Wiring (default pins — override in `platformio.ini`)

| Signal | ESP32-C6 GPIO | Connects to |
|---|---|---|
| I²C SDA | GPIO6 | SHT40 SDA, LIS3DH SDA | 
| I²C SCL | GPIO7 | SHT40 SCL, LIS3DH SCL |
| I²S BCLK (SCK) | GPIO2 | INMP441 SCK |
| I²S WS (LRCL) | GPIO3 | INMP441 WS |
| I²S SD | GPIO4 | INMP441 SD |
| Battery sense | GPIO1 (ADC1) | external divider midpoint |
| Button | GPIO9 | on-board BOOT button (reused) |
| LED | GPIO8 | on-board LED (heartbeat) |

LIS3DH straps: `SDO/SA0 → GND` gives I²C address **0x18** (the firmware default;
set `-DLIS3DH_ADDR=0x19` if you tie it to VCC). Leave `CS` high / unconnected so
the breakout stays in I²C mode. INMP441 `L/R → GND` selects the left slot, which
this mono build reads.

Add **4.7 kΩ pull-ups** on SDA and SCL to 3V3 if your breakouts don't include
them.

## Battery monitoring on the ESP32-C6 SuperMini — short answer

**The BAT+/BAT- pads are a battery *input* only.** On the common SuperMini
clones the cell feeds the 3.3 V LDO (usually through a series Schottky diode);
there is **no charge IC** and **no on-board sense divider**. So:

- **Charging through BAT+/-? No.** The board cannot charge a LiPo — there's no
  charger. Add an external **TP4056** module (or pick a board variant that
  explicitly lists USB charging) if you want in-place charging.
- **Monitoring through BAT+/-? Not directly.** You can't read the cell on an ADC
  without help, and the diode drop makes the rail an unreliable proxy. Add an
  **external resistor divider** (e.g. 2 × 100 kΩ) from BAT+ to an ADC1 GPIO
  (`PIN_VBAT_ADC`, default GPIO1) and the firmware reads it via
  `analogReadMilliVolts()`. Set `VBAT_DIVIDER` to your actual ratio.

If you don't fit a divider, set `-DENABLE_BATTERY=0` and the battery fields are
simply omitted.

> For a proper deployment, the production HiveInside path uses a real fuel gauge
> rather than a divider — this divider is just enough for prototype telemetry.

## Measurement JSON (GATT characteristic `8e8b0002-…`)

The full measurement is published as a compact JSON string on the custom
characteristic (read or notify). It is trimmed to stay under the 512-byte ATT
limit and to carry exactly the fields the HiveScale GATT reader
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
Environmental-Sensing (0x181A) and Battery (0x180F) services for generic
clients. `NaN` fields serialise as `null` ("field absent").

## Status & caveats

🚧 **Prototype, not yet hardware-validated.** The sensor/FFT modules are ported
from the field-tested HiveScale code; the BLE layer (NimBLE 2.x on the C6) and
pin map are new and need a bench check. Calibrate `VBAT_DIVIDER` against your
divider, and verify the GATT characteristics in nRF Connect on first flash. If
NimBLE fails to init on your core version, update the `h2zero/NimBLE-Arduino`
pin in `platformio.ini`.

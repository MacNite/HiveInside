# HiveInside

A stand-alone, battery-powered **in-hive environmental and acoustic sensor** for
beehive monitoring. It broadcasts its readings over **BLE as a beacon** that the
[HiveScale](https://github.com/MacNite/HiveScale) / HiveHub ecosystem picks up
with a passive scan each cycle and bridges to the backend.

- **Primary target:** Seeed **XIAO nRF54LM20A Sense** with an external SHT40, for low power and a
  compact integrated sensor platform — its on-board 6-axis IMU (LSM6DS3TR-C),
  PDM mic (MSM261DGT006) and nPM1300 PMIC fold most of the discrete sensors onto
  the module. The default contributor workflow is PlatformIO with Zephyr in
  [`firmware-nrf54lm20a/`](firmware-nrf54lm20a).
  [[buy]](https://www.seeedstudio.com/Seeed-Studio-XIAO-nRF54LM20A-Sense-p-6840.html)
> Part of the open beehive-monitoring ecosystem alongside **HiveScale** (weight /
> external sensing) and **BeeCounter** (entrance traffic).

---

## Why a separate device?

The frame-mounted accelerometer is, per the literature (Ramsey et al. 2020,
*Sci. Rep.*), the only reliable way to **predict swarming** up to ~30 days ahead —
microphones cannot capture the critical ~20 Hz vibrational signature. HiveInside
puts that accelerometer, plus acoustic FFT bands and climate sensing, **inside**
the hive on a tiny wireless board, avoiding the cabling a wired sensor needs.

---

## Sensors

| Function | Sensor | Interface |
|---|---|---|
| 3-axis vibration (swarm prediction, ~20 Hz) | LSM6DS3TR-C (on-board 6-axis IMU) | I²C |
| Acoustic FFT (piping, hum, stress) | MSM261DGT006 (on-board PDM mic) | PDM |
| Temperature + humidity | SHT40 (external) | I²C |
| Battery | nPM1300 (on-board PMIC) | sensor API |

Vibration and acoustics are analysed into the same FFT bands as HiveScale, so a
value means the same thing across the ecosystem.

---

## Key features

- **BLE beacon transport (nRF54LM20A)** — the full measurement (climate,
  vibration + acoustic FFT bands, battery) broadcasts continuously as a
  29-byte manufacturer-data advertisement that HiveHub decodes with a passive
  scan. No connection, pairing window, or wake-sync schedule needed.
- **Low-power asynchronous transport** — the controller repeats the measurement
  beacon while the application sleeps between sensor cycles.
- **Firmware-over-BLE (OTA)** — a connectable custom GATT service streams release
  bytes directly into the MCUboot secondary slot, with size and CRC verification.
  Measurement delivery remains connectionless on a separate advertising set.
- **PlatformIO with Zephyr** — the primary nRF54LM20A workflow keeps standard
  Zephyr source and configuration files while PlatformIO drives builds and uploads.

---

## Repository layout

```
HiveInside/
├── firmware-nrf54lm20a/  PlatformIO / Zephyr project (XIAO nRF54LM20A Sense)
├── enclosure/            Current 3D-printable enclosure files
├── docs/                 wiring, flashing, OTA, and Home Assistant guidance
└── README.md
```

---

## Quick start (XIAO nRF54LM20A Sense)

Install PlatformIO, then use the nRF54LM20A PlatformIO project:

```bash
cd firmware-nrf54lm20a
pio run
pio run -t upload
pio device monitor
```

PlatformIO builds the checked-in Zephyr application and its standard Zephyr
configuration files. See [`firmware-nrf54lm20a/README.md`](firmware-nrf54lm20a/README.md)
for the advanced nRF Connect SDK / `west` alternative and upload limitations.


---

## Status

The nRF54LM20A firmware reads all four sensors, performs vibration/acoustic
FFTs, broadcasts the HiveHub-compatible measurement beacon, and exposes the
MCUboot-backed Firmware-over-BLE service. See
[`firmware-nrf54lm20a/README.md`](firmware-nrf54lm20a/README.md),
[`docs/ota-over-ble.md`](docs/ota-over-ble.md), and
[`docs/wiring.md`](docs/wiring.md).

## License

MIT — see [`LICENSE`](LICENSE).

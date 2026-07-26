# HiveInside

A stand-alone, battery-powered **in-hive environmental and acoustic sensor** for
beehive monitoring. It broadcasts its readings over **BLE as a beacon** that the
[HiveScale](https://github.com/MacNite/HiveScale) / HiveHub ecosystem picks up
with a passive scan each cycle and bridges to the backend, and it can be updated
in place over BLE (MCUboot OTA) by a HiveHub relay.

- **Target:** Seeed **XIAO nRF54LM20A Sense** with an external SHT40, for low
  power and a compact integrated sensor platform — its on-board 6-axis IMU
  (LSM6DS3TR-C), PDM mic (MSM261DGT006) and nPM1300 PMIC fold most of the
  discrete sensors onto the module. The contributor workflow is PlatformIO with
  Zephyr in [`firmware-nrf54lm20a/`](firmware-nrf54lm20a).
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

| Function | Sensor (XIAO nRF54LM20A Sense) | Interface |
|---|---|---|
| 3-axis vibration (swarm prediction, ~20 Hz) | LSM6DS3TR-C (on-board 6-axis IMU) | I²C/SPI |
| Acoustic FFT (piping, hum, stress) | MSM261DGT006 (on-board PDM mic) | PDM |
| Temperature + humidity | SHT40 (external) | I²C |
| Barometric pressure | LPS22HB (external, optional) | I²C |

Vibration and acoustics are analysed into the same FFT bands as HiveScale, so a
value means the same thing across the ecosystem.

---

## Key features

- **BLE beacon transport (nRF54LM20A)** — the full measurement (climate,
  vibration + acoustic FFT bands, battery) broadcasts continuously as a
  29-byte manufacturer-data advertisement that HiveHub decodes with a passive
  scan. No connection, pairing window, or wake-sync schedule needed.
- **Ultra-low power** — the nRF54 idles with only the ~1 s advertiser
  running (a few µA); no deep-sleep rendezvous machinery or wake-sync schedule
  required.
- **Firmware-over-BLE (OTA)** — a HiveHub relay opens a connectable GATT
  session and streams a new image into MCUboot's inactive slot, which the node
  swaps in on the next boot. The measurement beacon stays connectionless; the
  OTA service runs as a second, connectable advertising set. See
  [`docs/ota-over-ble.md`](docs/ota-over-ble.md).
- **PlatformIO with Zephyr** — the nRF54LM20A workflow keeps standard Zephyr
  source and configuration files while PlatformIO drives builds and uploads.

---

## Repository layout

```
HiveInside/
├── firmware-nrf54lm20a/  PlatformIO / Zephyr project (XIAO nRF54LM20A Sense)
├── enclosure/            Current 3D-printable enclosure files
├── docs/                 wiring, flashing, OTA, Home Assistant
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
for the OTA design, the advanced nRF Connect SDK / `west` + sysbuild alternative
(which also builds the MCUboot bootloader), and upload limitations.

See [`docs/flashing.md`](docs/flashing.md) for flashing details,
[`docs/ota-over-ble.md`](docs/ota-over-ble.md) for the firmware-over-BLE wire
contract, and [`docs/wiring.md`](docs/wiring.md) for the XIAO and SHT40
connection reference.

---

## Status

🚧 **nRF54LM20A firmware bring-up in progress.** The firmware reads all four
sensors (SHT40 climate, LSM6DS3TR-C acceleration, PDM microphone level, nPM1300
battery), performs the vibration/acoustic FFTs, prints the readout, and
broadcasts the HiveHub-compatible BLE measurement beacon. Active scans
additionally receive its XIAO nRF54LM20A board identity and firmware version
without changing the measurement advertisement. Firmware-over-BLE (MCUboot
dual-slot OTA) is in place as a connectable GATT service alongside the
connectionless beacon.
See [`firmware-nrf54lm20a/README.md`](firmware-nrf54lm20a/README.md) for the
readout format and roadmap, and [`docs/wiring.md`](docs/wiring.md) for the
XIAO and SHT40 connection reference.

## License

MIT — see [`LICENSE`](LICENSE).

# HiveInside

A stand-alone, battery-powered **in-hive environmental and acoustic sensor** for
beehive monitoring. It exposes its readings over **BLE as a connectable GATT
server** and pairs with the [HiveScale](https://github.com/MacNite/HiveScale)
ecosystem, where a HiveScale node connects each cycle and bridges the data to the
backend.

- **Primary target:** Seeed **XIAO nRF54LM20A Sense** on a custom carrier PCB, for lowest
  power and a single integrated board — its on-board 6-axis IMU (LSM6DS3TR-C),
  PDM mic (MSM261DGT006) and nPM1300 PMIC fold most of the discrete sensors onto
  the module. The default contributor workflow is PlatformIO with Zephyr in
  [`firmware-nrf54lm20a/`](firmware-nrf54lm20a).
  [[buy]](https://www.seeedstudio.com/Seeed-Studio-XIAO-nRF54LM20A-Sense-p-6840.html)
- **Deprecated prototype:** Seeed **XIAO ESP32-C6** + breakout sensors. Its
  PlatformIO firmware remains in [`firmware-esp32-c6/`](firmware-esp32-c6) for
  historical testing and migration reference; it is not the target for new work.

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

| Function | Prototype (breakout) | Final (XIAO nRF54LM20A Sense) | Interface |
|---|---|---|---|
| 3-axis vibration (swarm prediction, ~20 Hz) | LIS3DH | LSM6DS3TR-C (on-board 6-axis IMU) | I²C/SPI |
| Acoustic FFT (piping, hum, stress) | MP34DT01 | MSM261DGT006 (on-board PDM mic) | PDM |
| Temperature + humidity | SHT40 | SHT40 (external) | I²C |
| Barometric pressure | — | LPS22HB (external, optional) | I²C |

Vibration and acoustics are analysed into the same FFT bands as HiveScale, so a
value means the same thing across the ecosystem.

---

## Key features

- **Connectable GATT server** — provided by the deprecated ESP32-C6 prototype,
  with standard Battery + Environmental-Sensing services plus a custom JSON
  characteristic carrying the full FFT dataset.
- **Ultra-low power** — the target design is intended for deep sleep with
  HiveScale-scheduled wake sync; nRF54LM20A firmware is still in bring-up.
- **Firmware-over-BLE (OTA)** — implemented on the deprecated ESP32-C6 prototype;
  not yet implemented for nRF54LM20A bring-up firmware.
- **Open hardware** — KiCad design + JLCPCB-ready BOM for the final carrier PCB.
- **PlatformIO with Zephyr** — the primary nRF54LM20A workflow keeps standard
  Zephyr source and configuration files while PlatformIO drives builds and uploads.

---

## Repository layout

```
HiveInside/
├── firmware-esp32-c6/    PlatformIO project (XIAO ESP32-C6 / Arduino) — deprecated prototype
│   ├── platformio.ini
│   ├── include/          config + pin map
│   └── src/              main + sensor + BLE modules
├── firmware-nrf54lm20a/  PlatformIO / Zephyr project (XIAO nRF54LM20A Sense) — primary target
├── hardware/             KiCad design + JLCPCB BOM (XIAO nRF54LM20A Sense carrier)
├── docs/                 prototype, wiring, flashing, OTA, Home Assistant
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

### Deprecated ESP32-C6 prototype

The ESP32-C6 project remains buildable for historical testing and migration
reference. It is not the primary firmware path:

```bash
cd firmware-esp32-c6
pio run -e c6_gatt_deprecated -t upload
```

See [`docs/flashing.md`](docs/flashing.md) for flashing details,
[`firmware-nrf54lm20a/README.md`](firmware-nrf54lm20a/README.md) for the primary
target, and [`docs/esp32c6-prototype.md`](docs/esp32c6-prototype.md) for the
deprecated prototype's wiring and measurement JSON.

---

## Status

🚧 **nRF54LM20A bring-up, not yet hardware-validated.** The primary firmware is
currently a Zephyr blinky/console bring-up application; sensor, BLE, MCUboot, and
DFU integration remain to be implemented. The deprecated ESP32-C6 prototype
retains its sensor, GATT, and OTA implementation for reference. Hardware design
for the final board is documented in
[`docs/wiring.md`](docs/wiring.md).

## License

MIT — see [`LICENSE`](LICENSE).

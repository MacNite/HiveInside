# HiveInside

A stand-alone, battery-powered **in-hive environmental and acoustic sensor** for
beehive monitoring. Built around the Nordic **nRF52840** and designed to run for
months on a single coin cell or small LiPo.

HiveInside broadcasts its readings over **BLE using the open
[BTHome v2](https://bthome.io/) protocol**, so it integrates natively with
**Home Assistant** (and any BTHome-aware receiver) with zero custom code — no
pairing, no cloud, no app required. It also pairs cleanly with the
[HiveScale](https://github.com/MacNite/HiveScale) ecosystem, where a HiveScale
node can act as a passive BLE bridge to the backend.

> Part of the open beehive-monitoring ecosystem alongside
> **HiveScale** (weight / external sensing) and **BeeCounter** (entrance traffic).

---

## Why a separate device?

The frame-mounted accelerometer is, per the literature
(Ramsey et al. 2020, *Sci. Rep.*), the only reliable way to **predict swarming**
up to ~30 days ahead — microphones cannot capture the critical ~20 Hz vibrational
signature. HiveInside puts that accelerometer, plus acoustic FFT bands and
climate sensing, **inside** the hive on a tiny wireless board, avoiding the
cabling that a wired in-hive sensor would otherwise require.

---

## Sensors

| Sensor | Measures | Interface |
|---|---|---|
| **LIS2DH12** | 3-axis vibration (swarm prediction, ~20 Hz band) | I²C |
| **ICS-43434** | Acoustic FFT bands (piping, hum, stress) | I²S |
| **SHT40** | Temperature + humidity | I²C |
| **LPS22HB** | Barometric pressure | I²C |

---

## Key features

- **BTHome v2** BLE advertising — native Home Assistant support
- **Ultra-low power** — nRF52840 deep sleep ~3–20 µA; months on a CR2477 / LiPo
- **No pairing required** — passive broadcast; optional encrypted mode via button
- **Open hardware** — KiCad design, JLCPCB-ready BOM
- **PlatformIO + Arduino** — easy to build and flash over USB (Nice!Nano) or SWD

---

## Repository layout

```
HiveInside/
├── firmware/        PlatformIO project (nRF52840 / Arduino)
│   ├── platformio.ini
│   ├── include/     config + pin map
│   └── src/         main + sensor + BLE modules
├── hardware/        KiCad schematic/PCB + JLCPCB BOM
├── docs/            wiring, flashing, Home Assistant integration
└── README.md
```

---

## Quick start (prototype on a Nice!Nano)

1. Install **VSCodium** + the **PlatformIO** extension.
2. Open `firmware/` as a PlatformIO project.
3. Plug in a **Nice!Nano v2** (or clone) via USB-C.
4. Build & upload (`PlatformIO: Upload`). The board mounts as a UF2 drive and
   flashes automatically.
5. Open Home Assistant → it should auto-discover a new **BTHome** device.

See [`docs/flashing.md`](docs/flashing.md) for custom-PCB / SWD flashing and
[`docs/homeassistant.md`](docs/homeassistant.md) for HA setup.

---

## Status

🚧 **Early scaffold.** Firmware modules are stubbed with working structure;
sensor drivers and the FFT pipeline are being ported next. Hardware design is
documented in [`docs/wiring.md`](docs/wiring.md).

## License

MIT — see [`LICENSE`](LICENSE).

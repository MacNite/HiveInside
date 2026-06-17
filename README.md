# As of 12.06.2026 development in this repository is halted in favor of using a [HolyIot 25015](https://de.aliexpress.com/item/1005010474941392.html?spm=a2g0o.order_list.order_list_main.4.6c975c5frpXTaz&gatewayAdapt=glo2deu). This devices currently lacks the sound sensor. A different solution for this is currently beeing developed



# HiveInside

A stand-alone, battery-powered **in-hive environmental and acoustic sensor** for
beehive monitoring. Built around the Nordic **nRF52840** and designed to run for
months on a single coin cell or small LiPo.

HiveInside exposes its readings over **BLE as a connectable GATT server** —
standard Battery and Environmental-Sensing services plus a custom characteristic
that carries the full measurement (every FFT band) as JSON. It pairs cleanly with
the [HiveScale](https://github.com/MacNite/HiveScale) ecosystem, where a HiveScale
node connects over GATT each cycle and bridges the readings to the backend.

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

- **Connectable GATT server** — standard Battery + Environmental-Sensing services plus a custom JSON characteristic with the full FFT dataset
- **Ultra-low power** — nRF52840 deep sleep ~3–20 µA; months on a CR2477 / LiPo
- **HiveScale-driven wake sync** — HiveScale schedules the next wake over GATT, so the radio is on only when it connects
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
5. Connect with a BLE client (e.g. **nRF Connect**) and read the HiveInside
   service, or pair the device with a **HiveScale** node so it bridges the
   readings to the backend.

See [`docs/flashing.md`](docs/flashing.md) for custom-PCB / SWD flashing and
[`docs/homeassistant.md`](docs/homeassistant.md) for Home Assistant / HiveScale
integration.

---

## Status

🚧 **Early scaffold.** Firmware modules are stubbed with working structure;
sensor drivers and the FFT pipeline are being ported next. Hardware design is
documented in [`docs/wiring.md`](docs/wiring.md).

## License

MIT — see [`LICENSE`](LICENSE).

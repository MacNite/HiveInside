# HiveInside

A stand-alone, battery-powered **in-hive environmental, vibration and acoustic sensor** for
beehive monitoring. It exposes its readings over **BLE as a connectable GATT
server** and pairs with the [HiveScale](https://github.com/MacNite/HiveScale)
ecosystem, where a HiveScale node connects each cycle and bridges the data to the
backend.

- **Now (prototype):** Seeed **XIAO ESP32-C6** + breakout sensors — the firmware
  in [`firmware/`](firmware) targets this board.
  [[buy]](https://s.click.aliexpress.com/e/_c43qaNVb)
- **Final:** Seeed **XIAO nRF52840** (or its SMD/castellated module) on a custom
  carrier PCB, for lowest power and a single integrated board.
  [[buy]](https://s.click.aliexpress.com/e/_c2yM9y1r)

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

| Function | Prototype (breakout) | Final (SMD) | Interface |
|---|---|---|---|
| 3-axis vibration (swarm prediction, ~20 Hz) | LIS3DH | LIS2DH12 | I²C |
| Acoustic FFT (piping, hum, stress) | MP34DT01 / compatible PDM MEMS mic | PDM MEMS mic | PDM |
| Temperature + humidity | SHT40 | SHT40 | I²C |
| Barometric pressure | — | LPS22HB | I²C |

Vibration and acoustics are analysed into the same FFT bands as HiveScale, so a
value means the same thing across the ecosystem. On the ESP32-C6 prototype the
microphone path now uses the chip's I²S PDM RX peripheral to capture raw PDM,
then decimates it to PCM in firmware before computing broadband dBFS and the
five acoustic FFT bands.

---

## Key features

- **Connectable GATT server** — standard Battery + Environmental-Sensing services
  plus a custom JSON characteristic carrying the full FFT dataset.
- **Ultra-low power** — deep sleep with HiveScale-scheduled wake sync, so the
  radio is on only when a central connects. Months on a CR2477 / LiPo.
- **Per-capture sensor power trimming** — the LIS3DH is woken only for the
  vibration block and then powered down again; the PDM microphone clock is
  stopped between acoustic captures; SHT40 runs in low-precision/no-heater mode.
- **Firmware-over-BLE (OTA)** — HiveScale relays new images over GATT.
- **Open hardware** — KiCad design + JLCPCB-ready BOM for the final carrier PCB.
- **PlatformIO + Arduino** — flashes over USB-C; no programmer needed on either XIAO.

---

## Repository layout

```
HiveInside/
├── firmware/        PlatformIO project (XIAO ESP32-C6 / Arduino)
│   ├── platformio.ini
│   ├── include/     config + pin map
│   └── src/         main + sensor + BLE modules
├── hardware/        KiCad design + JLCPCB BOM (XIAO nRF52840 carrier)
├── docs/            prototype, wiring, flashing, OTA, Home Assistant
└── README.md
```

---

## Quick start (XIAO ESP32-C6 prototype)

1. Install **VSCodium** + the **PlatformIO** extension.
2. Open [`firmware/`](firmware) as a PlatformIO project.
3. Plug in a **XIAO ESP32-C6** via USB-C.
4. Build & upload: `pio run -e c6_gatt -t upload`.
5. Connect with a BLE client (e.g. **nRF Connect**) to read the HiveInside
   service, or pair with a **HiveScale** node to bridge readings to the backend.

See [`docs/esp32c6-prototype.md`](docs/esp32c6-prototype.md) for wiring + the
measurement JSON, [`docs/flashing.md`](docs/flashing.md) for flashing both
boards, and [`docs/homeassistant.md`](docs/homeassistant.md) for integration.

---

## Status

🚧 **Prototype, not yet hardware-validated.** Sensor/FFT modules are ported from
the field-tested HiveScale code; the BLE layer, XIAO pin map and new PDM
microphone path need a bench check. Hardware design for the final board is
documented in [`docs/wiring.md`](docs/wiring.md).

## License

MIT — see [`LICENSE`](LICENSE).

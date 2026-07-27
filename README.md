# HiveInside

A stand-alone, battery-powered **in-hive environmental and acoustic sensor** for
beehive monitoring. It broadcasts its readings over **BLE as a beacon** that the
[HiveScale](https://github.com/MacNite/HiveScale) / HiveHub ecosystem picks up
with a passive scan each cycle and bridges to the backend.

- **Primary target:** Seeed **XIAO nRF54LM20A Sense** with an external SHT40, for low power and a
  compact integrated sensor platform — its on-board 6-axis IMU (LSM6DS3TR-C),
  PDM mic (MSM261DGT006) and nPM1300 PMIC fold most of the discrete sensors onto
  the module. The firmware is a Zephyr application; builds and flashing use the
  nRF Connect SDK / `west` in [`firmware-nrf54lm20a/`](firmware-nrf54lm20a).
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

| Function | XIAO nRF54LM20A Sense | Interface |
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
  running (a few µA); no deep-sleep rendezvous machinery required.
- **Firmware-over-BLE (OTA)** — MCUboot-based DFU streamed over a BLE GATT
  service from HiveScale, with signed images, CRC verification, and an automatic
  revert if a new image fails to boot. See [`docs/ota-over-ble.md`](docs/ota-over-ble.md).
- **Zephyr firmware built with `west`/sysbuild** — the application boots through
  MCUboot; a `west --sysbuild` build produces the bootable merged image, and
  PlatformIO serves as a compile check (see [`docs/flashing.md`](docs/flashing.md)).

---

## Repository layout

```
HiveInside/
├── firmware-nrf54lm20a/  Zephyr project (XIAO nRF54LM20A Sense) — primary target
├── firmware-esp32-c6/    unmaintained early prototype firmware (kept for reference only)
├── enclosure/            Current 3D-printable enclosure files
├── docs/                 wiring, flashing, OTA, Home Assistant
└── README.md
```

---

## Quick start (XIAO nRF54LM20A Sense)

The firmware boots through MCUboot, so the bootable image (MCUboot + signed app)
is produced by a `west --sysbuild` build and flashed as a single merged hex:

```bash
# In an nRF Connect SDK / Zephyr workspace with the XIAO nRF54LM20A board:
west build --sysbuild -b <board-target> path/to/firmware-nrf54lm20a
west flash            # flashes build/merged.hex over the on-board debugger
pio device monitor    # serial console over the same USB-C cable
```

`pio run` builds the application on its own as a **compile check**; it does not
run sysbuild and cannot produce a bootable image, so `pio run -t upload` is
guarded off. See [`docs/flashing.md`](docs/flashing.md) for the full build/flash
workflow — including the **nRF Connect for VS Code** GUI flow and common
troubleshooting — and [`firmware-nrf54lm20a/README.md`](firmware-nrf54lm20a/README.md)
for the readout format and layout.

---

## Status

🚧 **nRF54LM20A firmware bring-up in progress.** The firmware reads all four
sensors (SHT40 climate, LSM6DS3TR-C acceleration, PDM microphone level, nPM1300
battery), performs the vibration/acoustic FFTs, prints the readout, and
broadcasts the current HiveHub-compatible BLE measurement beacon. Active scans
additionally receive its XIAO nRF54LM20A board identity and firmware version
without changing the measurement advertisement. Firmware-over-BLE (OTA) is
implemented via MCUboot with a BLE GATT transport.
See [`firmware-nrf54lm20a/README.md`](firmware-nrf54lm20a/README.md) for the
readout format and roadmap, and [`docs/wiring.md`](docs/wiring.md) for the
XIAO and SHT40 connection reference.

## License

MIT — see [`LICENSE`](LICENSE).

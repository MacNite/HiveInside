# HiveInside

A stand-alone, battery-powered **in-hive environmental and acoustic sensor** for
beehive monitoring. It broadcasts its readings over **BLE as a beacon** that the
[HiveScale](https://github.com/MacNite/HiveScale) / HiveHub ecosystem picks up
with a passive scan each cycle and bridges to the backend.

- **Target:** Seeed **XIAO nRF54LM20A Sense** with an external SHT40, for low power and a
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

| Function | Part (XIAO nRF54LM20A Sense) | Interface |
|---|---|---|
| 3-axis vibration (swarm prediction, ~20 Hz) | LSM6DS3TR-C (on-board 6-axis IMU) | I²C |
| Acoustic FFT (piping, hum, stress) | MSM261DGT006 (on-board PDM mic) | PDM |
| Temperature + humidity | SHT40 (external) | I²C |
| Battery voltage + charge | nPM1300 PMIC fuel gauge (on-board) | — |

The firmware also auto-detects an external LIS3DH/LIS2DH12 at `0x18`/`0x19`, so a
breakout-based bench setup runs the same pipeline as the on-board IMU.

Vibration and acoustics are analysed into the same FFT bands as HiveScale, so a
value means the same thing across the ecosystem.

---

## Key features

- **BLE beacon transport** — the full measurement (climate, vibration +
  acoustic FFT bands, battery) broadcasts continuously as a 29-byte
  manufacturer-data advertisement that HiveHub decodes with a passive scan.
  Reading the data needs no connection, pairing window, or wake-sync schedule.
- **Ultra-low power** — the nRF54 idles with only the ~1 s advertiser
  running (a few µA); no deep-sleep rendezvous machinery required. The sensor
  rail (IMU + microphone) is switched off between the five-minute measurements.
- **Firmware-over-BLE (OTA)** — implemented: MCUboot dual-slot with a small
  GATT service that streams the signed image into the secondary slot, verifies
  size + CRC-32, and test-swaps with automatic rollback. The device therefore
  advertises *connectable* (`ADV_IND`); the measurement payload itself is still
  read passively from the advertisement.
  See [`docs/ota-over-ble.md`](docs/ota-over-ble.md).
- **PlatformIO with Zephyr** — the contributor workflow keeps standard Zephyr
  source and configuration files while PlatformIO drives the compile check.
  Bootable images come from a `west --sysbuild` build.

---

## Repository layout

```
HiveInside/
├── firmware-nrf54lm20a/  PlatformIO / Zephyr project (XIAO nRF54LM20A Sense)
│   ├── platformio.ini    compile-check env
│   ├── prj.conf          Zephyr config + app.overlay (board DT tweaks)
│   ├── sysbuild/         MCUboot child-image config for `west --sysbuild`
│   └── src/              main + sensor + FFT + beacon + OTA modules
├── enclosure/            Current 3D-printable enclosure files
├── docs/                 wiring, flashing, OTA, Home Assistant
└── README.md
```

---

## Quick start (XIAO nRF54LM20A Sense)

Install PlatformIO, then compile-check the application and open the console:

```bash
cd firmware-nrf54lm20a
pio run              # compile check only — does NOT produce a bootable image
pio device monitor   # serial console at 115200 over the on-board debugger
```

Because the firmware boots through MCUboot, a bootable device needs the
bootloader **and** the signed application, which only a `west --sysbuild` build
produces:

```bash
west build --sysbuild -b xiao_nrf54lm20a/nrf54lm20a/cpuapp path/to/firmware-nrf54lm20a
west flash
```

> ⚠️ `pio run -t upload` is deliberately blocked — it would flash an
> application-only image with nothing at `0x0` and brick the boot chain.

See [`docs/flashing.md`](docs/flashing.md) for flashing details and
troubleshooting, and [`firmware-nrf54lm20a/README.md`](firmware-nrf54lm20a/README.md)
for the readout format, BLE frame layout, and roadmap.

---

## Status

🚧 **Bring-up in progress; not yet hardware-validated in the field.** The
firmware reads all four sensors (SHT40 climate, LSM6DS3TR-C acceleration, PDM
microphone level, nPM1300 battery), performs the vibration/acoustic FFTs, prints
the readout, and broadcasts the HiveHub-compatible BLE measurement beacon.
Active scans additionally receive its XIAO nRF54LM20A board identity and
firmware version without changing the measurement advertisement.
Firmware-over-BLE is implemented on top of MCUboot.
See [`firmware-nrf54lm20a/README.md`](firmware-nrf54lm20a/README.md) for the
readout format and roadmap, and [`docs/wiring.md`](docs/wiring.md) for the
XIAO and SHT40 connection reference.

## License

MIT — see [`LICENSE`](LICENSE).

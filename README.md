# HiveInside

A stand-alone, battery-powered **in-hive environmental and acoustic sensor** for
beehive monitoring. It broadcasts its readings over **BLE as a beacon** that the
[HiveScale](https://github.com/MacNite/HiveScale) / HiveHub ecosystem picks up
with a passive scan each cycle and bridges to the backend.

- **Target:** Seeed **XIAO nRF54LM20A Sense** with an external SHT40, for low power and a
  compact integrated sensor platform — its on-board 6-axis IMU (LSM6DS3TR-C),
  PDM mic (MSM261DGT006) and nPM1300 PMIC fold most of the discrete sensors onto
  the module. The firmware is a Zephyr application in
  [`firmware-nrf54lm20a/`](firmware-nrf54lm20a), built with west and sysbuild.
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
- **Zephyr with sysbuild** — one toolchain for everything. `west --sysbuild`
  builds MCUboot and the signed application together, which is what makes a
  bootable image and an OTA payload.

---

## Repository layout

```
HiveInside/
├── firmware-nrf54lm20a/  Zephyr application (XIAO nRF54LM20A Sense)
│   ├── prj.conf          Zephyr config + app.overlay (board DT tweaks)
│   ├── low-power.conf    deployment profile, layered on top (docs/low-power.md)
│   ├── sysbuild/         MCUboot child-image config for `west --sysbuild`
│   └── src/              main + sensor + FFT + beacon + OTA modules
├── enclosure/            Current 3D-printable enclosure files
├── docs/                 wiring, building, flashing, OTA, Home Assistant
└── README.md
```

---

## Quick start (XIAO nRF54LM20A Sense)

From an nRF Connect SDK / Zephyr workspace that has the XIAO nRF54LM20A board
definition available (see [`docs/flashing.md`](docs/flashing.md)):

```bash
west build --sysbuild -b xiao_nrf54lm20a/nrf54lm20a/cpuapp -d debug path/to/firmware-nrf54lm20a
west flash -d debug
picocom -b 115200 /dev/ttyACM0   # serial console over the on-board debugger
```

`--sysbuild` is not optional: the firmware boots through MCUboot, so a bootable
device needs the bootloader **and** the signed application, and only a sysbuild
build produces both (plus the OTA payload). A plain `west build` yields an
application-only image with nothing at `0x0`, which does not boot.

See [`docs/vscode-build.md`](docs/vscode-build.md) to build it from VS Code or
VSCodium, [`docs/flashing.md`](docs/flashing.md) for flashing details and
troubleshooting, and [`firmware-nrf54lm20a/README.md`](firmware-nrf54lm20a/README.md)
for the readout format, BLE frame layout, and roadmap.
For battery deployment, see the audited [`low-power build profile`](docs/low-power.md),
including measurement guidance and upstream references.

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

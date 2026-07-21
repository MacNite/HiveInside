# HiveInside — XIAO nRF54LM20A Sense firmware

This is the primary HiveInside firmware target: a Zephyr application for the
Seeed XIAO nRF54LM20A Sense. It is the full port of the (deprecated) ESP32-C6
prototype's sensor suite:

- **SHT40** temperature + humidity (external, XIAO I²C header)
- **LSM6DS3TR-C** on-board IMU → vibration RMS/peak + swarm/fanning/activity
  FFT bands (a prototype-style external LIS3DH on the header is auto-detected
  too)
- **MSM261DGT006** on-board PDM microphone → acoustic RMS/peak + five FFT
  bands (sub-bass/hum/piping/stress/high)
- **nPM1300** PMIC → battery voltage + percentage

Every ~5 minutes (`MEASURE_INTERVAL_MS`) the device runs one capture cycle and
re-encodes the result; between cycles it idles with only the BLE advertiser
running.

## Data transport: BLE beacon

Unlike the ESP32-C6 prototype (a connectable GATT server that HiveHub had to
connect to every cycle), this firmware **broadcasts** the whole measurement as
a 26-byte manufacturer-specific advertisement — the frame documented in
`src/beacon.c` and decoded by HiveHub's `parseHiveInside()`
(HiveHub `firmware/src/ble_sensor.cpp`). HiveHub ingests it with its existing
passive scan; no connection, pairing window, or wake-sync schedule is needed,
and any number of HiveHubs can hear the same node.

Advertising is still **connectable**: HiveHub makes a one-off cached GATT
connection per session to read the version characteristic
(`{"fw":…,"board":"nrf54lm20a"}`) for board-matched OTA selection. The
device's address is the nRF factory static address (privacy off), so
MAC-based pairing in the HiveHub portal keeps working across reboots.

### Advertising rate (and why 1–1.2 s)

`BEACON_ADV_INT_MIN/MAX` default to 1.0–1.2 s. HiveHub wakes and scans for
`HOLYIOT_BLE_SCAN_SECONDS` (6 s) once per upload cycle, so what matters is
that several advertising events land inside any 6 s window:

| Interval | Events per 6 s scan | Avg. radio current (order) | Verdict |
|---|---|---|---|
| 100–200 ms | 30–60 | tens of µA | Only useful for app-discovery UX; ~10× the power for no extra data |
| **1.0–1.2 s** | **~5** | **a few µA** | Default: reliable capture, negligible battery cost |
| ≥ 2 s | 2–3 | ~2 µA | Starts missing whole scan windows on collisions; saves almost nothing |

The sensor capture (~3 s of IMU + mic sampling per cycle), not the radio,
dominates the energy budget, so slowing the beacon below ~1 s buys nothing
measurable. On the 500 mAh cell the wiring guide specifies, advertising at
1 s costs on the order of 1–2 % of capacity per year.

## GATT service (version + OTA placeholder)

`src/gatt_hive.c` serves the ecosystem's custom service
`8e8b0001-7a1c-4b9e-9a2f-1d6e0b9c1a01`:

| Characteristic | UUID | Status |
|---|---|---|
| Version/board JSON | `8e8b0002-…` | implemented (read) |
| OTA control | `8e8b0010-…` | **placeholder — rejects BEGIN** |
| OTA data | `8e8b0011-…` | **placeholder — rejects writes** |
| OTA status | `8e8b0013-…` | implemented (reports `OTA_ERR_BEGIN` after a BEGIN attempt) |

MCUboot and Zephyr DFU are **not** integrated yet, so firmware-over-BLE does
not work on this target. The placeholder keeps the final UUIDs and framing
(see `docs/ota-over-ble.md`) and refuses `BEGIN` at the ATT level so a
HiveHub relay fails fast — before downloading and streaming an image — with a
clear log on both sides. Implementing real OTA later means adding MCUboot to
the build and replacing the three handler bodies with slot writes + CRC
verify; the wire contract is already in place.

## Hardware expectations

The firmware binds peripherals by devicetree *compatible*, not board node
labels, and every sensor degrades to "absent" (a cleared bit in the beacon
flags byte) instead of failing the build or boot:

- **I²C**: all enabled `nordic,nrf-twim` controllers are probed — SHT40 at
  0x44, IMU at 0x6A/0x6B (LSM6 family) or 0x18/0x19 (LIS3DH). On the
  upstream board definition that means `i2c22` (the XIAO header) and `i2c30`
  (internal, where the on-board LSM6DS3TR-C answers at 0x6a).
- **Microphone**: any enabled `nordic,nrf-pdm` node. The board definition
  ships `pdm20` disabled, so `app.overlay` enables it.
- **Battery**: the `nordic,npm1300-charger` sensor node (the board wires the
  PMIC to a bit-banged `gpio-i2c` bus; the driver handles it).
- **Sensor rail**: the Sense's IMU and microphone are powered from nPM1300
  **LDO1**, which the stock board definition pins to 1.8 V — too low for
  either part. `app.overlay` re-ranges it to 3.3 V and `src/power.c`
  enforces/enables it at boot.
- **LED/button**: `led0` / `sw0` aliases, both optional. A short button press
  triggers an immediate measurement.

`app.overlay` and `zephyr/app.overlay` are kept identical (PlatformIO's
Seeed builder reads the `zephyr/` copy, `west` the root one); edit both
together.

## PlatformIO with Zephyr (default)

The default contributor workflow is PlatformIO with Zephyr:

```bash
cd firmware-nrf54lm20a
pio run
pio run -t upload
pio device monitor
```

`platformio.ini` selects the `seeed-xiao-nrf54lm20a` board, the **Zephyr**
framework, and the GCC Arm Embedded toolchain version used by Seeed's nRF54LM20A
Zephyr examples. The Seeed PlatformIO platform is pinned to commit
`9ba53b691fb007d9c1b8fd37600cc71d6702125a` so builds do not silently follow
upstream `main`.

PlatformIO builds the normal Zephyr application files that remain checked into
this directory: `src/`, root `CMakeLists.txt`, `prj.conf` and `app.overlay`,
plus the `zephyr/` copies used by Seeed's PlatformIO builder. PlatformIO does
**not** use the Arduino framework for this board.

### Uploading

The checked-in `platformio.ini` does not override the board upload setting, so
`pio run -t upload` uses the pinned Seeed board definition's `cmsis-dap` default.
That definition also lists pyOCD, probe-rs, and J-Link as optional protocols.
They are not the default contributor workflow and require a verified compatible
probe and board revision. UF2 is not configured by the checked-in board
definition.

### Updating the pinned Seeed platform

Update this dependency only intentionally: choose a candidate commit from
`Seeed-Studio/platform-seeedboards`, replace the full SHA in `platformio.ini`,
then run a clean build to validate the board and framework integration:

```bash
cd firmware-nrf54lm20a
pio run -t clean
pio run
```

Confirm that PlatformIO resolved the platform checkout to that exact SHA (for
example, with `git -C ~/.platformio/platforms/SeeedStudio rev-parse HEAD`) before
committing the updated SHA and this documentation. Review any Zephyr framework
or GCC Arm Embedded toolchain changes introduced by the candidate commit; do
not use an automatic dependency updater for this platform.

## Advanced: Zephyr / `west` alternative

The application source and root Zephyr files remain suitable for an existing
Zephyr or nRF Connect SDK workspace (Zephyr ≥ 3.7 for the advertising API
used in `beacon.c`). This is an advanced alternative, not the beginner
default:

```bash
west build -b xiao_nrf54lm20a/nrf54lm20a/cpuapp firmware-nrf54lm20a
```

For this route, install or expose the Seeed XIAO nRF54LM20A board definitions in
that workspace as required by the SDK version in use. Its flash procedure and
available debug probes are determined by that workspace and board revision.

## Bring-up checklist

On first flash, watch the 115200-baud console for:

1. `[PWR] nPM1300 LDO1 at 3.3V` — sensor rail up (absent on non-Sense DTs).
2. `[SHT40] present on …` / `[ACCEL] LSM6-class IMU at 0x6A on …` — probe
   results with the bus each part answered on.
3. `[MIC] rms=…` — first acoustic capture (or the overlay hint if the PDM
   node is disabled).
4. `[BLE] beacon advertising as <MAC> every 1000-1200 ms` — note the MAC for
   pairing in the HiveHub portal (HiveHub also auto-discovers the frame and
   labels it "HiveInside").

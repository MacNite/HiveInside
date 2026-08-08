# HiveInside — nRF54LM20A firmware

Firmware for the **Seeed XIAO nRF54LM20A Sense**, built with **Zephyr** —
`west --sysbuild`, from the command line or the nRF Connect VS Code extension.

The firmware reads every sensor, prints the readings to the USB serial console,
runs the **same vibration and acoustic FFT band analysis as HiveScale/HiveHub**,
and broadcasts each reduced result as a BLE beacon. The 29-byte
manufacturer-data frame keeps its 26-byte version-1 prefix directly compatible
with the current HiveHub passive scanner; reading the measurement needs no BLE
connection and no wake synchronisation.

## What it reads

Every `MEASURE_INTERVAL_MS` (default 5 min) the firmware reads all four sensors
and prints a block to the console:

| Group | Sensor | Source |
|---|---|---|
| Climate | SHT40 (external, XIAO I²C header, `0x44`) | raw I²C, low-precision measure |
| Vibration | LSM6DS3TR-C on-board IMU (I²C `0x6A`) | raw I²C, ~1024 samples @ ~400 Hz → mean X/Y/Z + FFT bands (mg) |
| Sound | MSM261DGT006 on-board PDM mic | Zephyr `dmic` API, RMS/peak + FFT bands (dBFS) |
| Battery | nPM1300 PMIC fuel gauge | Zephyr sensor API (`SENSOR_CHAN_GAUGE_VOLTAGE`) |

The accelerometer probe auto-detects the chip by `WHO_AM_I`, so an external
LIS3DH/LIS2DH12 breakout (`0x18`/`0x19`) also works for bench comparisons.

The FFT bands are the ecosystem-shared bands:

- **Vibration (mg, gravity removed):** swarm `8–30 Hz`, fanning `30–100 Hz`,
  activity `100–200 Hz`.
- **Acoustic (dBFS):** sub-bass `50–150 Hz`, hum `150–300 Hz`, piping
  `300–550 Hz`, stress `550–1500 Hz`, high `1500–3000 Hz`.

Each group prints `n/a` when its sensor is missing or the read failed that
cycle, so a partial board still gives a useful readout.

## BLE data transfer

The node sends one **legacy connectable undirected (`ADV_IND`)** advertisement
containing a 29-byte manufacturer-data value. The measurement itself is read
passively out of that advertisement — the connectable PDU type exists only so a
relay can open the firmware-over-BLE service (see
[`../docs/ota-over-ble.md`](../docs/ota-over-ble.md)); no GATT characteristic
carries sensor data.

Format version 2 retains every version-1 field at the same offset and appends
acceleration peak and microphone peak, so existing HiveHubs continue to decode
the core data while an updated decoder can expose both peaks. A sensor failure
clears that group's validity flag, preventing the server from interpreting
zero-filled bytes as a real measurement.

All multi-byte fields are little-endian. Offsets count from the start of the
manufacturer-specific data, i.e. including the two company-ID bytes:

| Offset | Field | Encoding | Valid when |
|---:|---|---|---|
| `0..1` | Company ID | `0x02E5` | always |
| `2` | Magic | `0x48` (`H`) | always |
| `3` | Format version | `2` | always |
| `4` | Validity flags | bit 0 climate, 1 accel, 2 mic, 3 battery | always |
| `5..6` | Temperature | `int16`, 0.1 °C/LSB | climate flag |
| `7..8` | Humidity | `uint16`, 0.1 %RH/LSB | climate flag |
| `9..10` | Battery voltage | `uint16`, 1 mV/LSB | battery flag |
| `11` | Battery charge | `uint8`, % (0–100) | battery flag |
| `12..13` | Acceleration RMS | `uint16`, 0.1 mg/LSB | accel flag |
| `14..15` | Vibration band — swarm (8–30 Hz) | `uint16`, 0.1 mg/LSB | accel flag |
| `16..17` | Vibration band — fanning (30–100 Hz) | `uint16`, 0.1 mg/LSB | accel flag |
| `18..19` | Vibration band — activity (100–200 Hz) | `uint16`, 0.1 mg/LSB | accel flag |
| `20` | Microphone RMS | `int8`, 1 dBFS/LSB | mic flag |
| `21` | Acoustic band — sub-bass (50–150 Hz) | `int8`, 1 dBFS/LSB | mic flag |
| `22` | Acoustic band — hum (150–300 Hz) | `int8`, 1 dBFS/LSB | mic flag |
| `23` | Acoustic band — piping (300–550 Hz) | `int8`, 1 dBFS/LSB | mic flag |
| `24` | Acoustic band — stress (550–1500 Hz) | `int8`, 1 dBFS/LSB | mic flag |
| `25` | Acoustic band — high (1500–3000 Hz) | `int8`, 1 dBFS/LSB | mic flag |
| `26..27` | Acceleration peak *(v2)* | `uint16`, 0.1 mg/LSB | accel flag |
| `28` | Microphone peak *(v2)* | `int8`, 1 dBFS/LSB | mic flag |

HiveHub decoders that only understand the 26-byte version-1 prefix safely
ignore the two trailing v2 fields. They continue to ingest every existing field,
but must add the offsets above before the two new peaks appear in backend data.

The primary advertisement is exactly full: 29 data bytes plus the AD length and
type byte fill the 31-byte legacy limit. The optional **BLE Flags** AD element is
therefore omitted, which is what makes the three peak-extension bytes fit without
extended advertising. One consequence is worth knowing: without a Flags element
the device is formally *non-discoverable*, so scanners that filter on the
general/limited discoverable bits may not list it even though it is connectable.
HiveHub's passive scan and a direct connect by address are unaffected.

The human-readable device name `HiveInside` rides in the scan response instead.
This preserves the complete manufacturer payload for HiveHub's passive scanner
while letting an active setup scan display a friendly name. The source also
declares the manufacturer name as `HiveInside`; on the BLE wire,
manufacturer-specific data contains only the numeric company ID (`0x02E5`), as
required by the BLE AD format.

Active scans also receive a compact manufacturer-data identity record in the
scan response. It does not alter the full 29-byte measurement advertisement or
the USB console path.

| Offset | Field | Value |
|---:|---|---|
| `0..1` | Company ID | `0x02E5`, little-endian |
| `2` | Record magic | `0x49` (`I`) |
| `3` | Record version | `1` |
| `4` | Board ID | `2` (Seeed XIAO nRF54LM20A) |
| `5..7` | Firmware version | major, minor, patch bytes |

The Bluetooth controller repeats the latest measurement every second. HiveHub
therefore receives it during its existing shared passive scan and forwards it
with the next server upload. Pair the node by its stable identity address as
**HiveInside (nRF54LM20A) — beacon**.

### Watchdog

A sealed node has no console and nobody watching it, so a hang inside a sensor
driver — a stuck I²C transfer, a `dmic_read()` that never returns — would leave
the firmware alive enough to keep advertising the last good measurement
indefinitely. From HiveHub that looks like a hive that simply stopped changing.

`src/watchdog.c` arms `wdt31` with a 60 s timeout and `main.c` feeds it, both at
the top of each cycle and every 20 s through the long idle between cycles, so
the timeout is sized against one stuck driver call rather than against
`MEASURE_INTERVAL_MS`. The OTA polling loop feeds it too: a firmware upload
legitimately takes minutes and must not be cut short. Adjust with
`HIVE_WDT_TIMEOUT_MS` / `HIVE_WDT_FEED_INTERVAL_MS` in `hive_config.h`.

The watchdog is recovery, not reporting. It resets a wedged node; it cannot
tell you that it happened. A node whose readings jump back to boot defaults is
the symptom to look for.

### Radio sleep and battery life

For the full code/hardware audit, production build profile, measurement method,
and upstream references, see [`../docs/low-power.md`](../docs/low-power.md).

- The firmware switches nPM1300 LDO1 off after each acquisition and restores it
  shortly before the next one. This removes the on-board IMU and microphone's
  standby load for nearly all of the five-minute measurement interval. The PMIC
  and external SHT40 remain available because they use other supplies.
  With the default settings the rail is needed for roughly 3.2 s per 300 s
  cycle (about 1.1% duty cycle). Using typical component standby figures as a
  planning estimate, switching it off saves approximately **10–20 uA average**
  at the battery, or **0.24–0.48 mAh/day** (**88–175 mAh/year**). This is not a
  whole-device battery-life prediction: BLE, the SoC, PMIC, SHT40, battery
  self-discharge, temperature, and regulator losses remain. Confirm the result
  on production hardware with a power analyzer, because board leakage and
  component spread can be comparable to these small standby currents.
- Keep advertising continuous. The nRF controller autonomously sleeps between
  the three short advertising-channel transmissions, and the application CPU
  remains asleep. Stopping BLE between measurements would save a little radio
  energy but makes a short, unsynchronised HiveHub scan likely to miss the node.
- The default one-second interval is the reliability/power compromise expected
  by HiveHub. `BLE_ADV_INTERVAL_MS` can be overridden at build time; increase it
  only after making sure the HiveHub scan window is several times longer than
  the chosen interval. Avoid intervals longer than the scan window.
- Sensor acquisition—especially the multi-second accelerometer capture and PDM
  microphone—is expected to dominate energy use, not beacon advertising. The
  five-minute field default produces two fresh samples per ten-minute reporting
  period. Set it to ten minutes for longer battery life if one fresh sample per
  reporting period is sufficient. Scan-window overlap is not a reason to sample
  more often: the last valid result continues advertising while the CPU sleeps.
- Do not use system-off/deep sleep for the normal cycle: it stops the BLE
  controller and defeats asynchronous passive scanning. Zephyr `k_msleep()`
  already lets the kernel and radio enter their supported idle states. Reserve
  system-off for storage/shipping mode, where discoverability is intentionally
  disabled.

Example output:

```
[HiveInside] nrf54lm20a fw 0.4.4 | USB + HiveHub BLE beacon
[PWR] nPM1300 LDO1 at 3.3V (IMU + mic rail)
[BLE] ready; name=HiveInside manufacturer=HiveInside id=0x02e5 interval=1000 ms
[SHT40] present on i2c@...
[ACCEL] LSM6DS3TR-C at 0x6A on i2c@...
---- HiveInside readout ----
  climate : 24.31 C   47.8 %RH
  accel   : x=-3.2 y=1.8 z=1004.6 mg  |a|=1004.6 mg
  accel AC: rms=2.4 peak=9.1 mg  (1024@416Hz)
  vib FFT : swarm=0.42 fan=0.18 act=0.09 mg
  sound   : rms=-61.4 dBFS  peak=-42.1 dBFS  (8000 frames)
  ac FFT  : sub=-58.2 hum=-49.7 pipe=-63.1 stress=-71.4 hi=-88.0 dBFS
  battery : 4.011 V  ~78%
----------------------------
[BLE] measurement advertised (flags=0x0f)
```

After each successful measurement advertisement update, the built-in blue LED
lights for `MEASUREMENT_LED_BLINK_MS` (default 100 ms). It does not blink for the
controller's automatic one-second repeats, only when fresh sensor data is
published.

## Build, flash, monitor

This firmware boots **through MCUboot** so it can accept firmware-over-BLE
updates (see [`../docs/ota-over-ble.md`](../docs/ota-over-ble.md)). A bootable
image is therefore MCUboot **plus** a signed application in slot 0, produced as
a single merged hex by a `west --sysbuild` build. A build without `--sysbuild`
covers the **application alone**, so it is a compile check and never a bootable
image.

```bash
# One-time: a west workspace at the Zephyr revision pinned in ../west.yml.
west init -m https://github.com/MacNite/HiveInside hiveinside-workspace
cd hiveinside-workspace && west update

# Build + flash the real, bootable OTA image (MCUboot + signed app):
west build --sysbuild -b <board-target> -d debug hiveinside/firmware-nrf54lm20a
west flash -d debug   # programs MCUboot + the signed app over the on-board debugger
                      # (run it against the TOP-LEVEL sysbuild build directory)

picocom -b 115200 /dev/ttyACM0   # serial console (see below)
```

Building from VS Code / VSCodium with the nRF Connect extension — the `debug`
configuration and the `lowpower` one for the deployment image — is documented in
[`../docs/vscode-build.md`](../docs/vscode-build.md).

Each `west` build also writes a version-stamped copy of the signed application
next to `zephyr.signed.bin`, so a release artifact still identifies itself once
it is detached from its build directory:

```
<build>/firmware-nrf54lm20a/zephyr/hiveinside-nrf54lm20a-v0.4.4-bringup.signed.bin
```

The version comes from `HIVEINSIDE_FW_VERSION_MAJOR`/`_MINOR`/`_PATCH` in
`src/hive_config.h` — the same numbers the node advertises; the
`-bringup` / `-lowpower` suffix is derived from `CONFIG_SERIAL` in the image
actually built, so a deployment image built with the
[`low-power`](../docs/low-power.md) profile can never be mistaken for a
console-enabled one when picking an OTA payload. Upload that file rather than
`zephyr.signed.bin` — see [`../docs/ota-over-ble.md`](../docs/ota-over-ble.md).

> ⚠️ **Never flash a build made without `--sysbuild`.** With MCUboot enabled the
> application links at the slot-0 offset, so flashing it alone leaves nothing at
> `0x0`: the CPU faults before `main()` runs and the device goes silent (no
> serial, no BLE). Build with `--sysbuild` and flash the merged image with
> `west flash`.

The XIAO nRF54LM20A Sense has an **on-board SAMD11 CMSIS-DAP debugger** (VID:PID
`0x2886:0x0068`) on the USB-C connector, so no external probe is needed — both
`west flash` and the serial console ride the same USB-C cable through it.

### Serial console over the same USB cable

The SAMD11 also exposes a **USB CDC ACM port** bridged to the SoC's `uart20`, so
`printk()` output appears on the host over the same USB-C cable used for
flashing. It runs at **115200 8N1** and enumerates on Linux as `/dev/ttyACM0`
and on macOS as `/dev/cu.usbmodem*` (run `ls /dev/cu.usbmodem*` to get the exact
name — there is no `/dev/ttyACM0` on macOS).

```bash
picocom -b 115200 /dev/ttyACM0            # Linux (or: screen /dev/ttyACM0 115200)
picocom -b 115200 /dev/cu.usbmodemXXXX    # macOS
```

The boot banner prints once at reset; press **RST** with the monitor connected
to see it and the first readout. The console is a plain polled UART, so the
firmware runs whether or not a terminal is attached.

An unconnected sensor never silences the console — the banner comes before any
sensor access and a missing SHT40 just prints `climate : n/a`. If there is no
output *and* no BLE advertising, the application is not running at all; see
"Device is silent after flashing" in [`../docs/flashing.md`](../docs/flashing.md).

### Microphone bring-up

The on-board microphone needs all three pieces of the Sense-board setup: P1.12
must enable the sensor power gate, nPM1300 LDO1 must supply 3.3 V, and `pwm20`
must be disabled because it shares a peripheral instance with `pdm20`. The
overlay configures the latter two, while `power_init()` explicitly enables the
power gate and LDO1 before the first capture. Microphone errors include the
Zephyr return code on the serial console, which makes a wiring/driver failure
distinguishable from silence.

See [`../docs/flashing.md`](../docs/flashing.md) for the external-probe
alternative and the `west` workflow, and [`../docs/wiring.md`](../docs/wiring.md)
for the SHT40 and battery connections.

## Layout

```
firmware-nrf54lm20a/
├── sysbuild.conf         enable MCUboot for the west --sysbuild build
├── sysbuild/mcuboot.conf MCUboot child-image config (small, console off, board
│                         power-management drivers off — see the file's comments)
├── CMakeLists.txt        application entry point (source list)
├── prj.conf              Zephyr config   ┐ both picked up automatically by
├── app.overlay           board DT tweaks ┘ Zephyr; no build field points at them
├── ncs_fixups.overlay    DT fixups applied by CMakeLists.txt
├── low-power.conf        opt-in deployment profile ─┐ see docs/low-power.md
├── low-power.overlay     opt-in deployment DT tweaks┘
├── cmake/
│   └── stamp_ota_payload.cmake  version-stamped copy of zephyr.signed.bin
└── src/
    ├── main.c            sensor loop + console print + beacon publish
    ├── beacon.[ch]       HiveHub-compatible manufacturer-data advertising
    ├── ota.[ch]          firmware-over-BLE GATT service → MCUboot slot 1
    ├── hive_config.h     addresses, timing, bands, per-sensor settings
    ├── measurement.h     one sensor snapshot, shared by every module
    ├── hive_i2c.[ch]     enumerate every enabled I²C bus for probing
    ├── fft.[ch]          dependency-free radix-2 FFT + band reduction
    ├── power.[ch]        nPM1300 LDO1 → 3.3 V sensor rail
    ├── watchdog.[ch]     wdt31 hardware watchdog, fed from the main loop
    ├── sht40.[ch]        SHT40 climate
    ├── accel.[ch]        LSM6DS3TR-C / LIS3DH vibration + FFT bands
    ├── mic.[ch]          PDM microphone level + FFT bands
    └── battery.[ch]      nPM1300 fuel gauge
```

> **One copy of every config file.** `prj.conf` and `app.overlay` used to be
> duplicated under `zephyr/` because PlatformIO's Seeed builder needed its own
> application root. PlatformIO is gone, and so are the copies — Zephyr picks up
> the files above directly.

## Roadmap

1. **Sensor readout over USB** — done.
2. **Vibration + acoustic FFT band analysis** (the bands shared with
   HiveScale/HiveHub) — done, printed to the console alongside the raw readings.
3. **BLE measurement beacon** (the 29-byte manufacturer-data advertisement
   HiveHub ingests) — done.
4. **BLE board/firmware identity** (compact scan-response record) — done.
5. **Firmware-over-BLE (MCUboot/DFU)** — implemented (GATT OTA service +
   streaming into slot 1 + MCUboot test-swap). Requires a `west --sysbuild`
   build/flash of the merged image; an application-only build cannot boot.

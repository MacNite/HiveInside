# HiveInside — nRF54LM20A firmware

Firmware for the **Seeed XIAO nRF54LM20A Sense**, built with **PlatformIO +
Zephyr**.

The firmware reads every sensor, prints the readings to the USB serial console,
runs the ecosystem's shared **vibration and acoustic FFT band analysis**, and
broadcasts each reduced result as a BLE beacon. The 29-byte
manufacturer-data frame keeps its 26-byte version-1 prefix directly compatible
with the current HiveHub passive scanner; no BLE connection or wake
synchronisation is needed.

## What it reads

Every `MEASURE_INTERVAL_MS` (default 5 min) the firmware reads all four sensors
and prints a block to the console:

| Group | Sensor | Source |
|---|---|---|
| Climate | SHT40 (external, XIAO I²C header, `0x44`) | raw I²C, low-precision measure |
| Vibration | LSM6DS3TR-C on-board IMU (I²C `0x6A`) | raw I²C, ~1024 samples @ ~400 Hz → mean X/Y/Z + FFT bands (mg) |
| Sound | MSM261DGT006 on-board PDM mic | Zephyr `dmic` API, RMS/peak + FFT bands (dBFS) |
| Battery | nPM1300 PMIC fuel gauge | Zephyr sensor API (`SENSOR_CHAN_GAUGE_VOLTAGE`) |

The accelerometer probe auto-detects the chip by `WHO_AM_I`, so a prototype-style
external LIS3DH/LIS2DH12 (`0x18`/`0x19`) also works for bench comparisons.

The FFT bands are the ecosystem-shared bands:

- **Vibration (mg, gravity removed):** swarm `8–30 Hz`, fanning `30–100 Hz`,
  activity `100–200 Hz`.
- **Acoustic (dBFS):** sub-bass `50–150 Hz`, hum `150–300 Hz`, piping
  `300–550 Hz`, stress `550–1500 Hz`, high `1500–3000 Hz`.

Each group prints `n/a` when its sensor is missing or the read failed that
cycle, so a partial board still gives a useful readout.

## BLE data transfer

The node sends one non-connectable legacy advertisement containing a 29-byte
manufacturer-data value. Format version 2 retains every version-1 field at the
same offset and appends acceleration peak and microphone peak, so existing
HiveHubs continue to decode the core data while an updated decoder can expose
both peaks. The frame starts with company ID `0x02E5`, magic `H`, and the format
version, followed by climate, battery, vibration FFT and acoustic FFT values.
A sensor failure
clears that group's validity flag, preventing the server from interpreting
zero-filled bytes as a real measurement.

The version-2 extension is little-endian and uses the existing group validity
bits:

| Offset | Field | Encoding | Valid when |
|---:|---|---|---|
| `26..27` | acceleration peak | `uint16`, 0.1 mg/LSB | accel flag (bit 1) |
| `28` | microphone peak | `int8`, 1 dBFS/LSB | mic flag (bit 2) |

HiveHub decoders that only understand the 26-byte version-1 prefix safely
ignore these trailing bytes. They continue to ingest every existing field, but
must add the offsets above before the two new peaks appear in backend data.

The primary advertisement is exactly full. BLE Flags are optional for this
non-connectable LE-only broadcaster and are omitted so the three peak-extension
bytes fit without requiring extended advertising. The human-readable device name
`HiveInside` is provided in its scan response. This preserves the complete
manufacturer payload for HiveHub's passive scanner while letting an active
setup scan display a friendly name. The source also declares the manufacturer
name as `HiveInside`; on the BLE wire, manufacturer-specific data contains only
the numeric company ID (`0x02E5`), as required by the BLE AD format.

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

### Firmware-over-BLE

A second, connectable legacy advertising set exposes the custom OTA service
without changing the non-connectable measurement beacon or its scan response.
The control, data, and status characteristics follow
[`docs/ota-over-ble.md`](../docs/ota-over-ble.md). DATA writes stream through
Zephyr's `flash_img` API into MCUboot slot 1; END requests an upgrade only after
the exact byte count and IEEE CRC-32 match. Sensor acquisition pauses while a
transfer is active, and the node reboots 1.5 seconds after publishing DONE.

The board's 2 MiB RRAM layout provides a 64 KiB boot partition and two 449 KiB
application slots (with corresponding secure/non-secure partitions). Sysbuild
enables MCUboot. `HIVEINSIDE_OTA_ENABLED` defaults to `1` and can be overridden
to `0` to compile out the service.

### Radio sleep and battery life

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
[HiveInside] nrf54lm20a fw 0.5.0 | sensor readout over USB
[PWR] nPM1300 LDO1 at 3.3V (IMU + mic rail)
[SHT40] present on i2c@...
[ACCEL] LSM6-class IMU at 0x6A on i2c@...
---- HiveInside readout ----
  climate : 24.31 C   47.8 %RH
  accel   : x=-3.2 y=1.8 z=1004.6 mg  |a|=1004.6 mg
  accel AC: rms=2.4 peak=9.1 mg  (1024@416Hz)
  vib FFT : swarm=0.42 fan=0.18 act=0.09 mg
  sound   : rms=-61.4 dBFS  peak=-42.1 dBFS  (8000 frames)
  ac FFT  : sub=-58.2 hum=-49.7 pipe=-63.1 stress=-71.4 hi=-88.0 dBFS
  battery : 4.011 V  ~78%
----------------------------
```

After each successful measurement advertisement update, the built-in blue LED
lights for `MEASUREMENT_LED_BLINK_MS` (default 100 ms). It does not blink for the
controller's automatic one-second repeats, only when fresh sensor data is
published.

## Build, flash, monitor

```bash
cd firmware-nrf54lm20a
pio run
pio run -t upload
pio device monitor
```

The XIAO nRF54LM20A Sense has an **on-board SAMD11 CMSIS-DAP debugger** (VID:PID
`0x2886:0x0068`) on the USB-C connector, so no external probe is needed —
`pio run -t upload` flashes over SWD through it.

### Serial console over the same USB cable

The SAMD11 also exposes a **USB CDC ACM port** bridged to the SoC's `uart20`, so
`printk()` output appears on the host over the same USB-C cable used for
flashing. It runs at **115200 8N1** and enumerates on Linux as `/dev/ttyACM0`.

```bash
pio device monitor -p /dev/ttyACM0 -b 115200
```

The boot banner prints once at reset; press **RST** with the monitor connected
to see it and the first readout. The console is a plain polled UART, so the
firmware runs whether or not a terminal is attached.

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
├── platformio.ini        PlatformIO env (Seeed platform, Zephyr, cmsis-dap upload)
├── CMakeLists.txt        west entry point (source list)
├── prj.conf              Zephyr config  ─┐ root copies serve west; the zephyr/
├── app.overlay           board DT tweaks ─┘ copies serve the PlatformIO builder
├── zephyr/               PlatformIO Zephyr application root
│   ├── CMakeLists.txt    (points back at ../src)
│   ├── prj.conf          identical copy of the root prj.conf
│   └── app.overlay       identical copy of the root app.overlay
└── src/
    ├── main.c            sensor loop + console print + beacon publish
    ├── beacon.[ch]       HiveHub-compatible manufacturer-data advertising
    ├── gatt_hive.[ch]    MCUboot-backed Firmware-over-BLE GATT service
    ├── hive_config.h     addresses, timing, bands, per-sensor settings
    ├── measurement.h     one sensor snapshot, shared by every module
    ├── hive_i2c.[ch]     enumerate every enabled I²C bus for probing
    ├── fft.[ch]          dependency-free radix-2 FFT + band reduction
    ├── power.[ch]        nPM1300 LDO1 → 3.3 V sensor rail
    ├── sht40.[ch]        SHT40 climate
    ├── accel.[ch]        LSM6DS3TR-C / LIS3DH vibration + FFT bands
    ├── mic.[ch]          PDM microphone level + FFT bands
    └── battery.[ch]      nPM1300 fuel gauge
```

> **Config sync:** the root `prj.conf`/`app.overlay` (for `west`) and the
> `zephyr/` copies (for the PlatformIO builder) describe the same application
> and **must stay byte-identical** — CI's `config-sync` job enforces it. Edit
> both together.

## Roadmap

1. **Sensor readout over USB** — done.
2. **Vibration + acoustic FFT band analysis** — done, printed to the console
   alongside the raw readings.
3. **BLE measurement beacon** (the 29-byte manufacturer-data advertisement
   HiveHub ingests) — done.
4. **BLE board/firmware identity** (compact scan-response record) — done.
5. **Firmware-over-BLE (MCUboot/DFU)** — done.

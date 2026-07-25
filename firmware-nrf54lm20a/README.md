# HiveInside — nRF54LM20A firmware

Firmware for the **Seeed XIAO nRF54LM20A Sense**, built with **PlatformIO +
Zephyr**.

Fresh rewrite that reads every sensor, prints the readings to the USB serial
console, and runs the **same vibration and acoustic FFT band analysis as the
ESP32-C6 prototype** (identical bands and units, so a value means the same thing
across the ecosystem). Each cycle the reading is also broadcast as a **26-byte
BLE manufacturer-data beacon** that HiveHub ingests with a passive scan and
bridges to the backend — no connection, no pairing window, no wake-sync.

## What it reads

Every `MEASURE_INTERVAL_MS` (default 5 s) the firmware reads all four sensors
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
cycle, so a partial board still gives a useful readout. The same per-group
"present" flags travel in the beacon, so HiveHub reports an absent sensor as
missing rather than as a bogus `0`.

## BLE data transfer to HiveHub

Every cycle the reading is encoded into a **26-byte manufacturer-data
advertisement** and published with `bt_le_adv_update_data()`. HiveHub's passive
scan (`ENABLE_BLE_SCAN`, on by default) matches the beacon by MAC and decodes it
in `firmware/src/ble_sensor.cpp::parseHiveInside()` — **no HiveHub change is
needed**; an existing HiveHub already understands this exact frame (company id
`0x02E5` + magic `'H'`). The layout (temperature, humidity, battery, vibration
RMS + swarm/fanning/activity bands, and the five acoustic dBFS bands) lives in
[`src/beacon.c`](src/beacon.c) and must stay in step with HiveHub's decoder.

**Pairing:** the firmware prints its BLE address at boot —

```
[BLE] beacon address F2:AB:… (random) — pair this MAC in HiveHub
```

Add that MAC as the hive's in-hive sensor in the HiveHub provisioning portal.
The address is the SoC's stable static-random identity, so it survives reboots.

### Radio sleep / power

The advertiser is **non-connectable and runs continuously** at
`BEACON_ADV_INTERVAL` (default ~1 s). This is deliberate: a beacon has no
back-channel to learn *when* HiveHub's short (~6 s) passive scan lands, so it has
to be on air at all times to be caught reliably. That costs almost nothing —
each advertising event is well under 1 ms of radio time, so at a 1 s interval the
radio is active **< 0.1 % of the time** (a few µA average); between events the
controller re-emits the last frame on its own while the CPU is asleep. (The old
ESP32-C6 prototype instead used a connectable GATT link with a HiveHub-driven
*wake-sync*; a pure beacon needs none of that machinery.)

Because the radio is essentially free, the real battery lever is
**`MEASURE_INTERVAL_MS`** — the sensor + FFT cycle (accel capture at ~400 Hz,
PDM microphone capture, two FFTs) is what draws real current. The 5 s default is
a bench value so a monitor shows fresh numbers quickly; for a deployed node raise
it to minutes (a hive's climate and vibration change slowly) and match it to how
often HiveHub uploads — there is no benefit to measuring faster than HiveHub
scans. The advertiser keeps broadcasting the last computed frame in between, so
every HiveHub scan still finds a valid reading. Nudging `BEACON_ADV_INTERVAL`
toward its ~2 s ceiling shaves a little more radio power if the battery is tight;
keep a margin below HiveHub's scan window so a scan never falls entirely between
two advertising events.

> **Board / firmware discovery:** because the node is non-connectable, HiveHub
> cannot read the optional board/version characteristic and will show the board
> as unknown. That only gates board-matched OTA selection, which the nRF54 node
> does not implement yet, so it is harmless today. When firmware-over-BLE lands
> (roadmap step 4) a minimal *connectable* GATT version service can be added
> alongside the beacon.

Example output:

```
[HiveInside] nrf54lm20a fw 0.2.0 | sensor readout over USB
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
    ├── main.c            readout loop + console print
    ├── hive_config.h     addresses, timing, bands, per-sensor settings
    ├── measurement.h     one sensor snapshot, shared by every module
    ├── hive_i2c.[ch]     enumerate every enabled I²C bus for probing
    ├── fft.[ch]          dependency-free radix-2 FFT + band reduction
    ├── power.[ch]        nPM1300 LDO1 → 3.3 V sensor rail
    ├── sht40.[ch]        SHT40 climate
    ├── accel.[ch]        LSM6DS3TR-C / LIS3DH vibration + FFT bands
    ├── mic.[ch]          PDM microphone level + FFT bands
    ├── battery.[ch]      nPM1300 fuel gauge
    └── beacon.[ch]       26-byte BLE measurement beacon (HiveHub ingest)
```

> **Config sync:** the root `prj.conf`/`app.overlay` (for `west`) and the
> `zephyr/` copies (for the PlatformIO builder) describe the same application
> and **must stay byte-identical** — CI's `config-sync` job enforces it. Edit
> both together.

## Roadmap

1. **Sensor readout over USB** — done.
2. **Vibration + acoustic FFT band analysis** (same bands as the ESP32-C6
   prototype) — done, printed to the console alongside the raw readings.
3. **BLE measurement beacon** (the 26-byte manufacturer-data advertisement
   HiveHub ingests) — done ([`src/beacon.c`](src/beacon.c)).
4. Firmware-over-BLE (MCUboot/DFU), and with it a minimal connectable GATT
   version service for HiveHub board/firmware discovery.

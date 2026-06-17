# HiveInside — ESP32-C6 BLE prototype

A quick, breadboard-friendly bring-up of the HiveInside sensor suite on cheap
modules, **before** the nRF52840 production PCB. It ports the proven LIS3DH and
INMP441 FFT code from [HiveScale](https://github.com/MacNite/HiveScale) and adds
a single **firmware switch between BLE advertising and a GATT server**.

Project: [`firmware-esp32c6/`](../firmware-esp32c6).

## Parts

| Part | Role | Interface |
|---|---|---|
| ESP32-C6 SuperMini | MCU + BLE 5 radio | — |
| LIS3DH breakout (GY-LIS3DH) | 3-axis vibration → swarm prediction | I²C |
| SHT40 | temperature + humidity | I²C |
| INMP441 | acoustic FFT (MEMS mic) | I²S |

## What it returns

Both vibration and acoustics are analysed exactly like HiveScale, so the band
definitions match across the ecosystem:

- **Acoustic FFT bands (dBFS)** + broadband RMS/peak: sub-bass 50–150, hum
  150–300, piping 300–550, stress 550–1500, high 1500–3000 Hz.
- **Vibration FFT bands (mg)** + broadband RMS/peak: swarm 8–30, fanning
  30–100, activity 100–200 Hz. The 8–30 Hz band is the ~20 Hz pre-swarm
  vibration (Ramsey et al. 2020) that the microphone cannot hear.
- **Temperature / humidity** (SHT40).
- **Battery** voltage + rough percentage (optional; see below).

## BLE mode switch (the "firmware variable")

Set by `BLE_MODE` in [`include/config.h`](../firmware-esp32c6/include/config.h),
overridden per PlatformIO environment:

| Env | `BLE_MODE` | Behaviour |
|---|---|---|
| `c6_advertising` (default) | `BLE_MODE_ADVERTISING` | Connectionless **BTHome v2** broadcast — temp/humidity/battery show up natively in **Home Assistant**. A compact manufacturer-data blob in the **scan response** also carries the full vibration + acoustic summary (all bands, RMS, peak). Radio on only `ADV_BURST_MS` per cycle → lowest power. |
| `c6_gatt` | `BLE_MODE_GATT` | Connectable **GATT server**: standard Battery (0x180F) + Environmental Sensing (0x181A) services for generic clients, plus a custom HiveInside service whose JSON characteristic returns the **entire** measurement (every band). Read once or subscribe to notifications. |

```bash
pio run -e c6_advertising -t upload   # BTHome broadcast
pio run -e c6_gatt        -t upload   # connectable GATT server
```

In advertising mode the device advertises **continuously** (refreshing the
payload every `MEASURE_INTERVAL_MS`, default 5 min — press the BOOT button to
force an immediate refresh). Scan with the **BTHome** integration in Home
Assistant or with nRF Connect. In GATT mode, connect with nRF Connect and read
the `8e8b0002-…` characteristic for the full JSON.

### Wake synchronisation (GATT mode)

When `HIVEINSIDE_SYNC_ENABLED` is set (default) and the device runs in
`BLE_MODE_GATT` **with** `DEEP_SLEEP_ENABLED`, HiveScale acts as the schedule
master. The custom HiveInside service exposes a third, **writable**
characteristic:

| UUID | Properties | Payload |
|---|---|---|
| `8e8b0002-…` | read / notify | full measurement JSON |
| `8e8b0003-…` | write / read | `uint32` little-endian — seconds to deep-sleep before the next connection |

Each cycle HiveScale connects, reads `8e8b0002-…`, then writes the next sleep
duration to `8e8b0003-…` (computed from its own send interval, so it tracks
remote interval changes automatically). On the following deep sleep HiveInside
honours that value instead of `MEASURE_INTERVAL_MS`, waking just before
HiveScale's next scan rather than advertising continuously.

Because the deep-sleep timer drifts (±5–10% on the internal RC oscillator) and
HiveScale's hint subtracts a small lead, HiveInside stays connectable for
`SYNC_LISTEN_MS` (45 s) after each wake to guarantee the scan/connect overlaps;
once the central has written the hint and disconnected, it sleeps immediately.
If no value is written during a wake (HiveScale missed the connection), it falls
back to `MEASURE_INTERVAL_MS`, so a missed cycle never leaves it asleep forever.
Received values are clamped to `[SYNC_MIN_SLEEP_MS, SYNC_MAX_SLEEP_MS]`.

### Serial output

The SuperMini's USB-C is the C6's **native USB Serial/JTAG**, so the build
enables `ARDUINO_USB_CDC_ON_BOOT` (in `platformio.ini`) to route `Serial` there.
Open the monitor at 115200 baud (`pio device monitor` or `pio run -t monitor`).
Without that flag, `Serial.*` goes to the UART0 TX/RX pins instead and nothing
appears over USB.

## Wiring (default pins — override in `platformio.ini`)

| Signal | ESP32-C6 GPIO | Connects to |
|---|---|---|
| I²C SDA | GPIO6 | SHT40 SDA, LIS3DH SDA | 
| I²C SCL | GPIO7 | SHT40 SCL, LIS3DH SCL |
| I²S BCLK (SCK) | GPIO2 | INMP441 SCK |
| I²S WS (LRCL) | GPIO3 | INMP441 WS |
| I²S SD | GPIO4 | INMP441 SD |
| Battery sense | GPIO1 (ADC1) | external divider midpoint |
| Button | GPIO9 | on-board BOOT button (reused) |
| LED | GPIO8 | on-board LED (heartbeat) |

LIS3DH straps: `SDO/SA0 → GND` gives I²C address **0x18** (the firmware default;
set `-DLIS3DH_ADDR=0x19` if you tie it to VCC). Leave `CS` high / unconnected so
the breakout stays in I²C mode. INMP441 `L/R → GND` selects the left slot, which
this mono build reads.

Add **4.7 kΩ pull-ups** on SDA and SCL to 3V3 if your breakouts don't include
them.

## Battery monitoring on the ESP32-C6 SuperMini — short answer

**The BAT+/BAT- pads are a battery *input* only.** On the common SuperMini
clones the cell feeds the 3.3 V LDO (usually through a series Schottky diode);
there is **no charge IC** and **no on-board sense divider**. So:

- **Charging through BAT+/-? No.** The board cannot charge a LiPo — there's no
  charger. Add an external **TP4056** module (or pick a board variant that
  explicitly lists USB charging) if you want in-place charging.
- **Monitoring through BAT+/-? Not directly.** You can't read the cell on an ADC
  without help, and the diode drop makes the rail an unreliable proxy. Add an
  **external resistor divider** (e.g. 2 × 100 kΩ) from BAT+ to an ADC1 GPIO
  (`PIN_VBAT_ADC`, default GPIO1) and the firmware reads it via
  `analogReadMilliVolts()`. Set `VBAT_DIVIDER` to your actual ratio.

If you don't fit a divider, set `-DENABLE_BATTERY=0` and the battery fields are
simply omitted.

> For a proper deployment, the production HiveInside path uses a real fuel gauge
> rather than a divider — this divider is just enough for prototype telemetry.

## Manufacturer-data blob layout (advertising mode scan response)

Little-endian, company id `0xFFFF`, so a HiveScale-side parser (mirroring
`firmware/src/ble_sensor.cpp`) can decode it:

| Offset | Type | Field | Scale |
|---:|---|---|---|
| 0 | u16 | company id (0xFFFF) | — |
| 2 | u8 | blob version (0x01) | — |
| 3 | u8 | flags: bit0 sht, bit1 accel, bit2 mic, bit3 batt | — |
| 4 | i16 | temperature °C | ×100 |
| 6 | u16 | humidity % | ×100 |
| 8 | u16 | battery mV | ×1 |
| 10 | u8 | battery % | ×1 |
| 11 | u16 | accel RMS mg | ×10 |
| 13 | u16 | accel peak mg | ×10 |
| 15 | u16 | accel band swarm mg | ×10 |
| 17 | u16 | accel band fanning mg | ×10 |
| 19 | u16 | accel band activity mg | ×10 |
| 21 | i8 | mic RMS dBFS | ×1 |
| 22 | i8 | mic peak dBFS | ×1 |
| 23 | i8 | mic sub-bass dBFS | ×1 |
| 24 | i8 | mic hum dBFS | ×1 |
| 25 | i8 | mic piping dBFS | ×1 |
| 26 | i8 | mic stress dBFS | ×1 |
| 27 | i8 | mic high dBFS | ×1 |

## Status & caveats

🚧 **Prototype, not yet hardware-validated.** The sensor/FFT modules are ported
from the field-tested HiveScale code; the BLE layer (NimBLE 2.x on the C6) and
pin map are new and need a bench check. Calibrate `VBAT_DIVIDER` against your
divider, and verify the BTHome packet in Home Assistant / nRF Connect on first
flash. If NimBLE fails to init on your core version, update the
`h2zero/NimBLE-Arduino` pin in `platformio.ini`.

# Home Assistant integration

HiveInside is a connectable **BLE GATT server**, not a BTHome broadcaster, so it
is **not** auto-discovered by Home Assistant's BTHome integration. There are two
supported ways to get its data into Home Assistant.

## Option 1 (recommended): via HiveScale

In a normal deployment a **HiveScale** node connects to HiveInside over GATT each
cycle, reads the measurement characteristic and forwards everything to the
HiveScale backend. Surface the data in Home Assistant from there (e.g. the
HiveScale API / MQTT) rather than talking to the sensor over BLE directly. This
is the path the wake-sync feature is built around, and it keeps the sensor's
radio duty cycle low.

## Option 2: direct GATT via an ESPHome BLE client

If you want Home Assistant to read the sensor directly, put an **ESP32 running
ESPHome** within radio range and use a `ble_client` to subscribe to the
HiveInside characteristics:

| Service | Characteristic | Data |
|---|---|---|
| Battery `0x180F` | `0x2A19` | battery % |
| Environmental Sensing `0x181A` | `0x2A6E` | temperature (0.01 °C) |
| Environmental Sensing `0x181A` | `0x2A6F` | humidity (0.01 %) |
| HiveInside `8e8b0001-…` | `8e8b0002-…` | full measurement JSON |

Temperature, humidity and battery use standard SIG characteristics, so an
ESPHome `ble_client_sensor` (or any generic GATT client) can read them directly.
The full FFT dataset lives in the JSON `8e8b0002-…` characteristic and needs a
small template / lambda to parse.

Note that an ESPHome BLE client holds a connection, which conflicts with
HiveScale's per-cycle connect + wake-sync — pick **one** central per device.

## Range & placement

BLE from inside a wooden hive attenuates somewhat. Keep the central (HiveScale
node or ESPHome proxy) within ~10–15 m, or co-locate it on the same stand.

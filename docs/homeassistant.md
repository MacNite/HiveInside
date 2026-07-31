# Home Assistant integration

HiveInside broadcasts its measurement as a **BLE manufacturer-data
advertisement** in a HiveHub-specific format — it is not a BTHome broadcaster
and it exposes **no measurement GATT characteristic**, so nothing about it is
auto-discovered by Home Assistant. The only connectable GATT service on the
device is the firmware-over-BLE service (see [`ota-over-ble.md`](ota-over-ble.md)),
which carries no sensor data.

There are two supported ways to get its data into Home Assistant.

## Option 1 (recommended): via HiveScale / HiveHub

In a normal deployment a **HiveScale / HiveHub** node picks the beacon up in its
shared passive scan and forwards the measurement to the backend with its next
upload. Surface the data in Home Assistant from there (e.g. the HiveScale API /
MQTT) rather than talking to the sensor over BLE directly.

Because the transport is a passive broadcast, this costs the sensor nothing
extra and any number of receivers can listen at once.

## Option 2: decode the advertisement locally

Put a BLE listener within radio range — an **ESP32 running ESPHome** with
`esp32_ble_tracker`, or Home Assistant's own Bluetooth integration via a
[Bluetooth proxy](https://esphome.io/projects/?type=bluetooth) — and parse the
manufacturer data yourself.

Match on company ID `0x02E5` and check that the payload starts with the magic
byte `0x48` (`'H'`). The full 29-byte frame layout, its validity flags, and the
per-field scaling are documented in
[`../firmware-nrf54lm20a/README.md`](../firmware-nrf54lm20a/README.md#ble-data-transfer).

A minimal ESPHome sketch of the approach:

```yaml
sensor:
  - platform: template
    name: "Hive temperature"
    id: hive_temp
    unit_of_measurement: "°C"
    accuracy_decimals: 1

esp32_ble_tracker:
  on_ble_manufacturer_data_advertise:
    - mac_address: XX:XX:XX:XX:XX:XX      # the node's identity address
      manufacturer_id: 02E5
      then:
        - lambda: |-
            // x = manufacturer payload after the company ID:
            //   x[0]='H', x[1]=version, x[2]=validity flags,
            //   x[3..4]=temp int16 LE in 0.1 C  (valid when flags & 0x01)
            if (x.size() < 5 || x[0] != 0x48) return;
            if (!(x[2] & 0x01)) return;
            int16_t t = (int16_t)(x[3] | (x[4] << 8));
            id(hive_temp).publish_state(t / 10.0f);
```

Extend the same pattern for humidity, battery, and the vibration/acoustic FFT
bands using the offset table in the firmware README. Note that the offsets in
that table are counted from the start of the manufacturer-specific data
**including** the two company-ID bytes, while ESPHome's `x` vector starts after
them — hence the two-byte shift above.

Unlike the connection-based approach this replaces, a passive listener does not
conflict with HiveScale: several receivers can decode the same advertisement.

## Range & placement

BLE from inside a wooden hive attenuates somewhat. Keep the receiver (HiveScale
node or Bluetooth proxy) within ~10–15 m, or co-locate it on the same stand.

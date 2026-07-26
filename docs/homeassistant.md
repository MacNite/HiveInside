# Home Assistant integration

HiveInside broadcasts its measurement as a **BLE manufacturer-data beacon** (not
BTHome), so it is **not** auto-discovered by Home Assistant's BTHome
integration. There are two supported ways to get its data into Home Assistant.

## Option 1 (recommended): via HiveScale

In a normal deployment a **HiveScale** / HiveHub node picks up HiveInside's
advertisement with its shared passive scan each cycle and forwards everything to
the HiveScale backend. Surface the data in Home Assistant from there (e.g. the
HiveScale API / MQTT) rather than listening to the sensor over BLE directly. This
keeps the sensor's radio duty cycle low and needs no extra hardware near the
hive.

## Option 2: direct via an ESPHome BLE listener

If you want Home Assistant to read the sensor directly, put an **ESP32 running
ESPHome** within radio range and parse the advertisement with
`esp32_ble_tracker`. HiveInside is a passive beacon — no connection or pairing is
needed; the ESP32 just needs to hear the manufacturer-data packet.

The 29-byte manufacturer-data frame starts with company ID `0x02E5`
(little-endian), magic `H` (`0x48`), and a format version, followed by climate,
battery, vibration-FFT and acoustic-FFT fields. Its exact byte layout — and the
version-2 trailing peak bytes — is documented in
[`../firmware-nrf54lm20a/README.md`](../firmware-nrf54lm20a/README.md); mirror
that layout in an ESPHome `lambda` on the manufacturer data to publish the
fields you care about (temperature, humidity, battery, and the FFT bands).

Because it is a broadcast, several listeners can decode the same packet at once —
unlike a GATT connection there is no single-central restriction.

## Range & placement

BLE from inside a wooden hive attenuates somewhat. Keep the central (HiveScale
node or ESPHome listener) within ~10–15 m, or co-locate it on the same stand.

# Home Assistant integration

HiveInside broadcasts **BTHome v2** advertisements, which Home Assistant
discovers automatically — no custom integration, no cloud, no app.

## Requirements

- Home Assistant with the **BTHome** integration (built in).
- A Bluetooth adapter on the HA host **or** an **ESP32 Bluetooth Proxy**
  (ESPHome) within radio range of the hive. The HiveScale node can also serve
  as a passive BLE bridge.

## Pairing (none, by default)

Because BTHome uses connectionless advertising, there is **no pairing step** for
the default (unencrypted) mode:

1. Power up the HiveInside device.
2. Within a couple of minutes HA shows a **"BTHome sensor discovered"**
   notification.
3. Click **Configure** → the device and its sensors (temperature, humidity,
   pressure, battery, acceleration) are added.

Press the on-board button once to force an immediate advertisement if you want
the device to appear faster ("identify").

## Optional: encrypted advertisements

BTHome supports AES-encrypted payloads. When enabled (future firmware option),
HA asks for a **bind key** during configuration. The on-board button is used to
enter pairing/key-handover mode so the key can be provisioned cleanly. This is
optional — unencrypted mode is fine for most hobby deployments.

## Acoustic bands

Climate, pressure, battery and accel map to standard BTHome object IDs and
appear automatically. The five FFT acoustic bands are application-specific; they
are exposed as additional values and may need a small template or a BTHome
custom-object mapping to surface as named sensors. See firmware
`src/ble_bthome.cpp` for the payload layout.

## Range & placement

BLE from inside a wooden hive attenuates somewhat. Keep the HA Bluetooth adapter
/ ESP32 proxy within ~10–15 m, or co-locate a HiveScale node (acting as bridge)
on the same stand.

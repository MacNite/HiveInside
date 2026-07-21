# Flashing HiveInside

## Primary target: XIAO nRF54LM20A Sense

The default contributor workflow for the final target is **PlatformIO with
Zephyr**. The project keeps ordinary Zephyr application source and configuration
files; PlatformIO is the build, upload, and monitor interface.

```bash
cd firmware-nrf54lm20a
pio run
pio run -t upload
pio device monitor
```

`pio run -t upload` uses the `cmsis-dap` upload protocol selected by this
project's pinned Seeed PlatformIO board definition. The checked-in project does
not override that default or configure UF2. Follow the PlatformIO upload output
for the resolved platform revision and connected debug hardware.

The current nRF54LM20A firmware is **bring-up firmware** (LED and console), not
a complete application. It has no MCUboot/DFU integration, so BLE OTA is not yet
available on this target. See [`firmware-nrf54lm20a/README.md`](../firmware-nrf54lm20a/README.md)
for the PlatformIO details and the advanced `west` alternative.

### Advanced alternative: nRF Connect SDK / `west`

Contributors already using a compatible nRF Connect SDK or Zephyr workspace may
build the same application with `west`. This is an advanced alternative rather
than the beginner default; its board definitions and flashing setup depend on
the SDK workspace in use. Do not substitute it for the PlatformIO instructions
above unless you have set up that workspace. The board definition lists pyOCD,
probe-rs, and J-Link as optional supported protocols; they are not the default
workflow and require a verified compatible probe and board revision.

## Deprecated prototype: XIAO ESP32-C6

The ESP32-C6 PlatformIO project is retained for historical testing and migration
reference. It remains buildable and retains its OTA implementation, but is not
the primary firmware path:

```bash
cd firmware-esp32-c6
pio run -e c6_gatt_deprecated -t upload
```

The compatibility environment `c6_gatt` remains available for existing commands
and CI. The C6 uses its native USB upload flow. If the port is not found, hold
**BOOT**, tap **RESET**, then release **BOOT** to enter download mode and retry.

See [`ota-over-ble.md`](ota-over-ble.md) for the ESP32-C6 prototype OTA protocol.

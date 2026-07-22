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

`pio run -t upload` uploads over SWD using a **CMSIS-DAP debug probe**. The
nRF54LM20A XIAO has no on-board debugger, so an external probe wired to the
board's SWD pads is required (UF2 is not configured by the board definition).

This project sets `upload_protocol = pyocd` in `platformio.ini` rather than the
board's `cmsis-dap`/OpenOCD default. The Seeed PlatformIO builder invokes
OpenOCD with a fixed Seeed CMSIS-DAP VID:PID filter (`0x2886:0x0068`), which
rejects generic third-party probes with `unable to find a matching CMSIS-DAP
device`. pyOCD auto-detects any connected CMSIS-DAP probe and applies no such
filter. If pyOCD reports a missing nRF54LM20A target pack, fall back to
`upload_protocol = probe-rs`.

### Using a XIAO RP2040 as the CMSIS-DAP probe

A spare XIAO RP2040 works as the SWD probe:

1. Flash it with Raspberry Pi **Debugprobe** firmware: double-tap reset to mount
   the `RPI-RP2` drive, then drag on `debugprobe_on_pico.uf2` (from the
   `raspberrypi/debugprobe` releases). It re-enumerates as a CMSIS-DAP probe
   (`0x2E8A:0x000C`).
2. Wire probe → target (both are 3.3 V, no level shifting):

   | Probe (XIAO RP2040) | Signal | Target (XIAO nRF54LM20A) |
   | ------------------- | ------ | ------------------------ |
   | GP2 (pad D8)        | SWCLK  | SWCLK / SWDCLK           |
   | GP3 (pad D10)       | SWDIO  | SWDIO                    |
   | GND                 | GND    | GND                      |

   Power the target from its own USB (or the probe's 3V3 — not both).
3. `pio run -t upload`. pyOCD finds the probe automatically.

On Linux, add a udev rule so the probe is accessible without `sudo`
(`SUBSYSTEM=="usb", ATTRS{idVendor}=="2e8a", MODE="0666"`), then reload rules
and replug.

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

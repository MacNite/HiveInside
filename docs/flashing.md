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

The XIAO nRF54LM20A Sense has an **on-board SAMD11 CMSIS-DAP debugger** (Seeed
USB VID:PID `0x2886:0x0068`) connected to the SoC's SWD lines and brought out on
the USB-C connector. **No external probe is required** — plug the board into USB
and `pio run -t upload` flashes over SWD through the on-board debugger.

This project sets `upload_protocol = cmsis-dap` in `platformio.ini`, which uses
the board definition's OpenOCD path. That path filters for the on-board
debugger's fixed VID:PID (`0x2886:0x0068`), so it binds to this board's debugger
and ignores unrelated CMSIS-DAP dongles.

Do **not** switch this target back to `pyocd`: on the current silicon pyOCD
aborts during APPROTECT recovery with
`Memory transfer fault @ 0x00ffc31c-0x00ffc31f`. `cmsis-dap` (OpenOCD) is the
supported upload path; the board definition also lists `probe-rs` and J-Link for
contributors who deliberately attach an external probe.

### Serial console over the same USB cable

The on-board SAMD11 also exposes a **USB CDC ACM serial port** and bridges it to
the SoC's `uart20`, so application `printk()` and Zephyr logs appear on the host
over the same USB-C cable used for flashing — no second adapter needed. It runs
at **115200 8N1** and enumerates on Linux as `/dev/ttyACM0` (the index can
differ when other ACM devices are attached).

```bash
# Confirm which ACM node is the Seeed on-board debugger (VID 2886)
udevadm info -q property -n /dev/ttyACM0 | grep -E 'ID_VENDOR_ID|ID_MODEL|ID_SERIAL'
# ID_VENDOR_ID=2886  → Seeed XIAO nRF54LM20A on-board debugger

pio device monitor -p /dev/ttyACM0 -b 115200
```

The startup banner
(`[HiveInside] nrf54lm20a fw <version> | BLE beacon transport`) prints **once at
boot**, so if the monitor is opened afterwards, press **RST** with it connected
to see the banner and the first measurement cycle. The console is a plain polled
UART, so the firmware never blocks on a missing terminal — it boots and keeps
advertising over BLE regardless. (The nRF54's native `usbhs` is not wired to the
port — the SAMD11 owns it — so the console must ride the debugger's UART bridge
rather than a native USB-CDC device on the nRF54.)

### Optional: using a XIAO RP2040 as an external CMSIS-DAP probe

The on-board debugger is sufficient for normal use; this is only for boards
whose debugger is unavailable, or for bench setups that prefer an external
probe. A spare XIAO RP2040 works as the SWD probe:

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
3. Select an upload protocol that targets an external probe, since the default
   `cmsis-dap`/OpenOCD path is filtered to the on-board debugger's VID:PID and
   will not bind to the RP2040 (`0x2E8A:0x000C`). Use `upload_protocol = probe-rs`
   (with the nRF54LM20A target pack installed) and then `pio run -t upload`.

On Linux, add a udev rule so the probe is accessible without `sudo`
(`SUBSYSTEM=="usb", ATTRS{idVendor}=="2e8a", MODE="0666"`), then reload rules
and replug.

The nRF54LM20A firmware is the full sensor-beacon application (SHT40, IMU,
microphone, nPM1300 battery, and the BLE beacon transport). It has no
MCUboot/DFU integration yet, so BLE OTA is not available on this target. See
[`firmware-nrf54lm20a/README.md`](../firmware-nrf54lm20a/README.md) for the
PlatformIO details, the serial-console setup, and the advanced `west`
alternative.

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

# Flashing HiveInside

## Primary target: XIAO nRF54LM20A Sense

This firmware boots **through MCUboot** so it can accept firmware-over-BLE
updates (see [`ota-over-ble.md`](ota-over-ble.md)). A bootable image is therefore
MCUboot **plus** a signed application in slot 0. That combined, signed image is
produced only by a **`west --sysbuild`** build as a single `merged.hex`, and
that is what gets flashed:

```bash
# From an nRF Connect SDK / Zephyr workspace that has the XIAO nRF54LM20A
# board definition available (it ships with the Seeed PlatformIO platform
# under zephyr/boards/; add it out-of-tree to your west workspace).
west build --sysbuild -b <board-target> path/to/firmware-nrf54lm20a
west flash            # flashes build/merged.hex over the on-board debugger
pio device monitor    # serial console over the same cable (see below)
```

Use a pristine build when changing the board, partition layout, bootloader, or
signing configuration (`west build --pristine --sysbuild ...`). Before treating
the result as a release, inspect the generated partition report and retain both
artifacts for their distinct purposes: `merged.hex` is the SWD factory/recovery
image, while the application's `zephyr.signed.bin` is the BLE OTA payload. Never
send `merged.hex` or the unsigned `zephyr.bin` through the OTA characteristic.
See the production-key, release-test, and recovery checklist in
[`ota-over-ble.md`](ota-over-ble.md#production-release-and-recovery-checklist).

The board target is the west name of the Seeed board definition with its
nRF54L core qualifier (for example `xiao_nrf54lm20a/nrf54lm20a/cpuapp`); use the
exact name the board's `board.yml` declares.

The XIAO nRF54LM20A Sense has an **on-board SAMD11 CMSIS-DAP debugger** (Seeed
USB VID:PID `0x2886:0x0068`) connected to the SoC's SWD lines and brought out on
the USB-C connector. **No external probe is required** — plug the board into USB
and `west flash` programs the merged image over SWD through the on-board
debugger.

### PlatformIO is a compile check, not a flashing path

PlatformIO's Zephyr builder produces a **single application image** — it does
not run sysbuild, so it never builds MCUboot, signs the app, or emits a merged
hex (`sysbuild.conf` / `sysbuild/mcuboot.conf` are ignored by `pio run`). Use it
to check that the application compiles:

```bash
cd firmware-nrf54lm20a
pio run
```

> ⚠️ **Do not `pio run -t upload`.** With MCUboot enabled, PlatformIO links the
> application at the slot-0 offset (behind an MCUboot header) and flashes it with
> **nothing at `0x0`**, so the CPU faults before `main()` runs and the device
> goes completely silent — no serial output, no BLE. This is the classic "builds
> fine but never boots" symptom. The `upload` target is guarded in
> `platformio.ini` and refuses to run; flash the merged hex with `west flash`.

The board definition's default runner uses OpenOCD's CMSIS-DAP path, filtered to
the on-board debugger's fixed VID:PID (`0x2886:0x0068`), so it binds to this
board's debugger and ignores unrelated CMSIS-DAP dongles. `west flash` uses that
same on-board debugger.

Do **not** use `pyocd` on the current silicon: it aborts during APPROTECT
recovery with `Memory transfer fault @ 0x00ffc31c-0x00ffc31f`. CMSIS-DAP
(OpenOCD) is the supported path; the board definition also lists `probe-rs` and
J-Link for contributors who deliberately attach an external probe.

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
(`[HiveInside] nrf54lm20a fw <version> | sensor readout over USB`) prints **once
at boot**, followed by a readout block every few seconds, so if the monitor is
opened afterwards, press **RST** with it connected to see the banner and the
first readout. The console is a plain polled UART, so the firmware never blocks
on a missing terminal — it boots and keeps sampling regardless. (The nRF54's
native `usbhs` is not wired to the port — the SAMD11 owns it — so the console
must ride the debugger's UART bridge rather than a native USB-CDC device on the
nRF54.)

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
3. Flash the merged image through the external probe with a matching `west`
   runner (for example `west flash --runner probe-rs`, with the nRF54LM20A
   target pack installed). The on-board debugger's OpenOCD path is filtered to
   its fixed VID:PID and will not bind to the RP2040 (`0x2E8A:0x000C`).

On Linux, add a udev rule so the probe is accessible without `sudo`
(`SUBSYSTEM=="usb", ATTRS{idVendor}=="2e8a", MODE="0666"`), then reload rules
and replug.

The nRF54LM20A firmware reads all four sensors (SHT40, IMU, microphone, nPM1300
battery), prints the readout to this serial console, runs the vibration and
acoustic FFT band analysis, broadcasts the HiveHub measurement beacon, and
accepts firmware-over-BLE updates through MCUboot. See
[`firmware-nrf54lm20a/README.md`](../firmware-nrf54lm20a/README.md) for the
readout format, PlatformIO details, and the serial-console setup, and
[`ota-over-ble.md`](ota-over-ble.md) for the OTA protocol.

### The `west --sysbuild` build is the flashing path

Because the firmware boots through MCUboot, the only bootable artifact is the
merged MCUboot-plus-signed-app image, and only `west --sysbuild` produces it —
see the top of this document. Build it in an nRF Connect SDK / Zephyr workspace
that has the XIAO nRF54LM20A board definition available (it ships with the Seeed
PlatformIO platform under `zephyr/boards/`; add it out-of-tree to your west
workspace). `west flash` programs `build/merged.hex` over the on-board CMSIS-DAP
debugger; with an external probe (below) select the matching runner, e.g.
`west flash --runner jlink`. The board definition also lists pyOCD, probe-rs,
and J-Link as optional protocols that require a verified compatible probe and
board revision.

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

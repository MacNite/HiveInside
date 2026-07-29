# Flashing HiveInside

## Primary target: XIAO nRF54LM20A Sense

This firmware boots **through MCUboot** so it can accept firmware-over-BLE
updates (see [`ota-over-ble.md`](ota-over-ble.md)). A bootable image is therefore
MCUboot **plus** a signed application in slot 0. Only a **`west --sysbuild`**
build produces both, and both must reach the chip:

```bash
# From an nRF Connect SDK / Zephyr workspace that has the XIAO nRF54LM20A
# board definition available (it ships with the Seeed PlatformIO platform
# under zephyr/boards/; add it out-of-tree to your west workspace).
west build --sysbuild -b <board-target> path/to/firmware-nrf54lm20a
west flash            # programs every sysbuild image over the on-board debugger
pio device monitor    # serial console over the same cable (see below)
```

`west flash` run against the **top-level** sysbuild build directory reads
`build/domains.yaml` and programs every image in flash order — MCUboot at `0x0`
and the *signed* application at slot 0 (`zephyr.signed.hex`; Zephyr's
`cmake/mcuboot.cmake` repoints the runner at it). Flashing a single image, or
flashing `build/firmware-nrf54lm20a/zephyr/zephyr.hex` by hand, leaves the
device unbootable — see "Device is silent after flashing" below.

> **`merged.hex` is an nRF Connect SDK artifact, not an upstream Zephyr one.**
> NCS's sysbuild concatenates the images into `build/merged.hex`; a plain
> `zephyrproject` workspace does **not** create that file, and it is not needed —
> `west flash` handles the multi-image case on its own. Where this document and
> [`ota-over-ble.md`](ota-over-ble.md) refer to `merged.hex` as the SWD
> factory/recovery image, that applies to NCS builds; on upstream Zephyr the
> equivalent is "everything `west flash` programs".

Use a pristine build when changing the board, partition layout, bootloader, or
signing configuration (`west build --pristine --sysbuild ...`). Before treating
the result as a release, inspect the generated partition report and retain the
artifacts for their distinct purposes: the full-flash image is for SWD
factory/recovery, while the application's `zephyr.signed.bin` is the BLE OTA
payload. Never send a full-flash image or the unsigned `zephyr.bin` through the
OTA characteristic.
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

On **macOS** the same port appears as `/dev/cu.usbmodem*` — there is no
`/dev/ttyACM0`, so a copied-and-pasted `-p /dev/ttyACM0` fails or silently binds
nothing. List the ports first and pick the Seeed one; note that the board
presents both a CDC ACM data port and the CMSIS-DAP interface, so more than one
entry can appear:

```bash
pio device list                     # or: ls /dev/cu.usbmodem*
pio device monitor -p /dev/cu.usbmodemXXXX -b 115200
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

Because the firmware boots through MCUboot, a bootable device needs both the
bootloader and the signed application, and only `west --sysbuild` produces them
— see the top of this document. Build it in an nRF Connect SDK / Zephyr
workspace that has the XIAO nRF54LM20A board definition available (it ships with
the Seeed PlatformIO platform under `zephyr/boards/`; add it out-of-tree to your
west workspace). `west flash` programs every sysbuild image over the on-board
CMSIS-DAP debugger; with an external probe (below) select the matching runner, e.g.
`west flash --runner jlink`. The board definition also lists pyOCD, probe-rs,
and J-Link as optional protocols that require a verified compatible probe and
board revision.

> ⚠️ **Not** the "nRF Connect SDK **Bare Metal**" option (`nrf-bm`, board
> targets prefixed `bm_`). That is a separate, RTOS-free SDK line built on the
> SoftDevice and raw nrfx drivers — it has no Zephyr kernel, no Zephyr device
> model or devicetree-driven peripherals, and no Zephyr Bluetooth host. This
> firmware is a Zephyr application (`zephyr/kernel.h`, `zephyr/bluetooth/*`,
> `zephyr/drivers/{regulator,i2c,sensor,gpio}.h`, `zephyr/audio/dmic.h`,
> `zephyr/dfu/mcuboot.h`, plus the board overlays), so it cannot build there at
> any version. Use upstream Zephyr or the RTOS-based nRF Connect SDK. Note also
> that `bm_nrf54lm20dk` is Nordic's nRF54LM20 **DK**, a different board from the
> Seeed XIAO nRF54LM20A Sense this firmware targets.

#### The board target is missing from an nRF Connect SDK workspace

`xiao_nrf54lm20a` ships in **upstream Zephyr**. The nRF Connect SDK uses its own
Zephyr fork (`nrfconnect/sdk-zephyr`), which does **not** carry it — not on
`main`, `v3.2-branch` or `v3.1-branch` — so it never appears in the VS Code
extension's board-target dropdown, however new the SDK is. Only Nordic's own
`nrf54lm20dk` is there. Nothing is broken; the definition is simply absent.

Two ways forward:

* **Build against upstream Zephyr** (a plain `zephyrproject` west workspace).
  This is the path the rest of this document describes and needs no extra setup.
* **Add the board out-of-tree to the NCS workspace.** Copy the whole
  `boards/seeed/xiao_nrf54lm20a/` directory out of upstream Zephyr into a board
  root of your own, keeping the `boards/<vendor>/<board>/` structure:

  ```
  my-boards/
  └── boards/seeed/xiao_nrf54lm20a/     ← the upstream directory, unmodified
  ```

  Then point the build at the directory that *contains* `boards/` — not at the
  board directory itself:

  ```bash
  west build --sysbuild -b xiao_nrf54lm20a/nrf54lm20a/cpuapp \
    path/to/firmware-nrf54lm20a -- -DBOARD_ROOT=/abs/path/to/my-boards
  ```

  In the VS Code extension the same value goes in the nRF Connect **board roots**
  setting, after which the target shows up in the dropdown. Use an absolute path:
  sysbuild resolves relative `*_ROOT` variables against the application
  directory, which is rarely what you mean.

  The copy is self-contained: `seeed_xiao_connector.dtsi` lives inside the board
  directory, and the SoC-level includes it pulls in
  (`nordic/nrf54lm20a_cpuapp.dtsi`, `vendor/nordic/nrf54lm20_a_b_cpuapp_partition.dtsi`)
  are both present in `sdk-zephyr`. Expect to re-sync the copy whenever upstream
  changes the board — this repo's `ncs_fixups.overlay` already exists because the
  two trees do not always agree.

#### If the MCUboot image fails to link

A `--sysbuild` build that dies while linking `mcuboot/zephyr/zephyr_pre0.elf`
with `undefined reference to z_impl_k_mutex_lock` / `k_work_submit` /
`z_impl_k_usleep` (usually preceded by the Kconfig warning `I2C ... was assigned
the value 'n' but got the value 'y'`) is the board's power-management drivers
leaking into the bootloader: MCUboot is built single-threaded on Nordic SoCs,
but the XIAO board tree enables `CONFIG_REGULATOR` plus the `power_en` regulator
and the nPM1300 on a bit-banged I²C bus for *every* sysbuild image, and those
drivers need the kernel mutex/work-queue APIs. `sysbuild/mcuboot.conf` turns
`CONFIG_REGULATOR` and `CONFIG_MFD` off for the bootloader image to prevent
this; if you see the error, check that fragment is being picked up (it must sit
next to the application, i.e. `firmware-nrf54lm20a/sysbuild/mcuboot.conf`) and
rebuild with `--pristine`. The failure is not host- or toolchain-specific, and
neither a plain `west build` nor `pio run` reproduces it — they never build
MCUboot.

#### Device is silent after flashing (no console, no BLE)

The firmware prints its banner as the very first statement of `main()`, before
touching any sensor, and starts advertising a few lines later. Missing sensors
never cause silence — an unconnected SHT40 just prints `climate : n/a`. So *no
console output **and** no advertising* means the application is not running at
all, and the fault is in the boot chain rather than in the application. Work
through it in this order:

1. **Is the console itself proven?** Check the port name (macOS uses
   `/dev/cu.usbmodem*`, not `/dev/ttyACM0`) and press **RST** with the monitor
   already attached — the banner prints once at reset and is easy to miss.
   Silence on BLE too makes a pure console problem unlikely, but this is the
   cheapest check.
2. **Did both images reach the chip?** Run `west flash` against the top-level
   sysbuild build directory, not a sub-image directory, and read its output: it
   should program two images. Re-flash explicitly with
   `west flash --domain mcuboot` and `west flash --domain firmware-nrf54lm20a`
   if in doubt. An application alone at slot 0 with nothing at `0x0` faults
   before `main()` and is completely silent — this is the same brick the
   `pio run -t upload` warning above describes.
3. **Make MCUboot talk.** `sysbuild/mcuboot.conf` disables the bootloader's
   console for size, which also means a bootloader that refuses to start slot 0
   fails *silently*. Temporarily comment out the `CONFIG_SERIAL=n` /
   `CONFIG_CONSOLE=n` / `CONFIG_UART_CONSOLE=n` lines there, add
   `CONFIG_MCUBOOT_LOG_LEVEL_INF=y`, rebuild with `--pristine` and re-flash.
   MCUboot then reports on `uart20` whether it found and validated a slot-0
   image. A signature or image-magic complaint points at the signing key; no
   MCUboot output at all points back at step 2.
4. **Check the signing key matched.** The application must be signed with the
   key the bootloader was built with. Zephyr does *not* fail the build when the
   application has no signing key — `cmake/mcuboot.cmake` only warns that the
   result "will not be bootable by MCUboot unless it is signed manually", and
   `west flash` then programs an unsigned image that MCUboot silently rejects.
   Confirm `build/firmware-nrf54lm20a/zephyr/zephyr.signed.hex` exists and that
   the configure log names the same key file for both images. See the next
   section for how to tell the two apart.

#### `E: Image in the primary slot is not valid!`

With the bootloader console enabled (step 3 above) a rejected application looks
like this:

```
I: Starting bootloader
I: Primary image: magic=unset, swap_type=0x1, copy_done=0x3, image_ok=0x3
I: Boot source: none
E: Image in the primary slot is not valid!
E: Unable to find bootable image
```

`magic=unset` and `image_ok=0x3` are **not** the problem — those describe the
image *trailer*, which an SWD-flashed image legitimately does not have, and
MCUboot boots such an image happily. The failure is `boot_validate_slot()`
rejecting slot 0, which has exactly two causes: the header/signature does not
verify against the key built into the bootloader, or the bytes on the chip are
not the bytes `imgtool` produced. Two commands separate them:

```bash
# (a) Is the image itself validly signed with the bootloader's key?
#     Default sysbuild config uses MCUboot's RSA-2048 development key.
cd <west-topdir>
python bootloader/mcuboot/scripts/imgtool.py verify \
  -k bootloader/mcuboot/root-rsa-2048.pem \
  <build>/firmware-nrf54lm20a/zephyr/zephyr.signed.bin

# (b) Do the bytes on the chip match the hex? (openocd runner only)
west flash --domain firmware-nrf54lm20a --verify
```

If (a) fails, the build is at fault — compare the two images' Kconfig, which is
where a signing-type or key mismatch shows up:

```bash
grep -E 'MCUBOOT_SIGNATURE_KEY_FILE|MCUBOOT_GENERATE_UNSIGNED_IMAGE|ROM_START_OFFSET|MCUBOOT_BOOTLOADER_MODE' \
  <build>/firmware-nrf54lm20a/zephyr/.config
grep -E 'BOOT_SIGNATURE_TYPE|BOOT_SIGNATURE_KEY_FILE|BOOT_VALIDATE_SLOT0|BOOT_SWAP|SINGLE_APPLICATION_SLOT' \
  <build>/mcuboot/zephyr/.config
```

The bootloader and the application must agree on the signature type and the key,
and on the MCUboot mode. Sysbuild derives both sides from the same
`SB_CONFIG_BOOT_SIGNATURE_TYPE_*` / `SB_CONFIG_BOOT_SIGNATURE_KEY_FILE` (default:
RSA with `$(ZEPHYR_MCUBOOT_MODULE_DIR)/root-rsa-2048.pem`), so a mismatch means
something overrode one side — most often a `CONFIG_BOOT_*` line added to
`sysbuild/mcuboot.conf`, which changes only the bootloader and leaves the
application signed the old way. Set signing options in `sysbuild.conf` as
`SB_CONFIG_*` instead, so both images move together.

If (a) passes but (b) fails, the image is good and the programming step is at
fault: re-flash with `west flash --domain firmware-nrf54lm20a --erase`, and if
that does not help try a different runner (`--runner jlink` with an external
probe).

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

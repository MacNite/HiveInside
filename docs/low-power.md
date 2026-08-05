# Reducing idle power on the XIAO nRF54LM20A Sense

This is a code-and-hardware audit for HiveInside's normal mode: continuous BLE
advertising with a sensor acquisition every five minutes. It deliberately
separates **idle current** from average current. A power analyzer is required
to validate the final number; component typical values are not a substitute
for a measurement of the assembled board.

## What the firmware already gets right

* `k_msleep()` blocks the application instead of polling. Zephyr can idle the
  CPU while the Bluetooth controller schedules advertising events. The idle
  wait is split into watchdog-sized slices, which costs a few wake-ups per
  interval against roughly 300 advertising events in the same window.
* `CONFIG_TICKLESS_KERNEL=y` is pinned in `prj.conf`. It is the Zephyr default,
  but it is the single assumption the whole idle story rests on: without it the
  CPU would wake on every system tick for five minutes at a time.
* The nRF54 main regulator is configured for DC/DC operation by Seeed's board
  definition (`&vregmain { regulator-initial-mode = <NRF5X_REG_MODE_DCDC>; }`
  in `nrf54lm20a_cpuapp_common.dtsi`), so the application does not have to.
* PDM is stopped after capture, and the accelerometer is put in power-down.
* More importantly, nPM1300 LDO1 (the IMU/microphone rail) is disabled after
  both captures and enabled only immediately before the next acquisition.
* The SHT40 uses its lowest-power, lowest-precision single-shot command rather
  than periodic measurement mode.
* BLE data is updated once per measurement. The controller repeats it without
  a one-second application wake-up.

## Use the deployment profile

The default build intentionally retains `uart20`, floating-point `printk()`,
and the blue measurement LED for bring-up. For a battery deployment, build the
additional profile:

```bash
west build --sysbuild -b xiao_nrf54lm20a/nrf54lm20a/cpuapp \
  path/to/firmware-nrf54lm20a -- \
  -DEXTRA_CONF_FILE=low-power.conf \
  -DEXTRA_DTC_OVERLAY_FILE=low-power.overlay
```

The profile disables the diagnostic UART, its console/`printk()` support, and
the measurement LED. BLE measurement advertisements and BLE OTA are unchanged.
The UART is the most important idle-only firmware difference: leaving a serial
peripheral and its pins active solely for an unattended console is needless.

Use the normal build for troubleshooting. The deployment profile has no serial
output at all, so a wedged driver, a failed LDO1 enable, or a fatal error is
invisible on the wire — the beacon keeps repeating the last good measurement.
The watchdog (`src/watchdog.c`, armed in both builds) is what turns that from
permanent into a reset, but it is recovery, not reporting: a node that resets
repeatedly looks the same from HiveHub as one that is healthy. Treat readings
that stop changing, or that jump back to boot defaults, as a site visit.

Both file names above are relative and are resolved for you: Zephyr looks up
`EXTRA_CONF_FILE` entries under the application config directory, and runs the
devicetree preprocessor with the application source directory as its working
directory. No absolute paths are needed.

The root `CMakeLists.txt` sets `EXTRA_DTC_OVERLAY_FILE` to the NCS devicetree
fix-up, and that does **not** hide the overlay you pass on the command line.
Zephyr reads the variable with
`zephyr_get(EXTRA_DTC_OVERLAY_FILE SYSBUILD LOCAL MERGE REVERSE)`, which merges
the sysbuild, CMake-cache, environment, and local-variable scopes rather than
taking only the highest-precedence one. Both overlays end up in the DTS list,
fix-up first.

> **Status: CI does not cover this profile.** CI compiles only the default
> configuration, through PlatformIO against `firmware-nrf54lm20a/zephyr/`.
> Build the profile yourself after changing anything it touches, and before
> flashing a sealed node.

## In VS Code / VSCodium (nRF Connect extension)

Keep the bring-up image and the deployment image as two build configurations,
so both `merged.hex` files exist side by side and you can flash either one.

In **APPLICATIONS** → the application → **Add build configuration**:

* **Board target** — the same one the existing configuration uses.
* **Build directory name** — change it from `build` to e.g. `build_lowpower`.
  This is what separates the two; reusing `build` collides with the bring-up
  configuration.
* **Use sysbuild** — required. Without it there is no bootable image; see
  [`flashing.md`](flashing.md).
* **Extra Kconfig fragments** — `low-power.conf`
* **Extra Devicetree overlays** — `low-power.overlay`

Leave the two **Base** fields and **Snippets** empty. Nothing needs to go in
**Extra CMake arguments**; the two fields above already map to
`EXTRA_CONF_FILE` and `EXTRA_DTC_OVERLAY_FILE`. (Passing them by hand as
`-DEXTRA_CONF_FILE=low-power.conf -DEXTRA_DTC_OVERLAY_FILE=low-power.overlay`
works identically, if you prefer.)

> ⚠️ **Do not use "Base configuration files (Kconfig fragments)".** Despite the
> parenthetical, that field is `CONF_FILE`: it *replaces* `prj.conf` rather than
> adding to it. Pointing it at `low-power.conf` produces a build with no
> Bluetooth, no MCUboot image manager and no sensor stack, which fails while
> compiling `ota.c`:
>
> ```
> error: 'CONFIG_IMG_BLOCK_BUF_SIZE' undeclared here (not in a function)
> ```
>
> The give-aways are `-DCONF_FILE="low-power.conf"` (no `EXTRA_`) in the west
> command line the build prints, and a Kconfig section that merges
> `low-power.conf` without ever merging `prj.conf`. The field you want is
> **Extra Kconfig fragments**, one row below it. The same distinction applies to
> **Base Devicetree overlays** versus **Extra Devicetree overlays**: the base
> field replaces `app.overlay`, which would drop the PMIC pin fix, the PDM
> microphone and the watchdog.

Check three things in the build log before trusting the result:

1. The west command line contains `-DEXTRA_CONF_FILE` and
   `-DEXTRA_DTC_OVERLAY_FILE`, and no bare `-DCONF_FILE`.
2. The application image merges **both** configurations, in this order:
   `Merged configuration '.../prj.conf'` then
   `Merged configuration '.../low-power.conf'`.
3. All three overlays are listed: `app.overlay`, `ncs_fixups.overlay`, and
   `low-power.overlay`.

Then compare the reported `FLASH` size against the bring-up build. The
deployment image must come out visibly smaller — the UART driver, console,
`printk()` and the cbprintf floating-point formatter are all gone. An identical
size means the fragment was not applied.

The extension adds `-DCONFIG_DEBUG_THREAD_INFO=y` of its own accord for
debugger thread awareness. It is harmless here and is not part of the profile.

Both configurations write their signed image to the same filename,
`zephyr.signed.bin`, so each build also drops a stamped copy beside it —
`hiveinside-nrf54lm20a-v<version>-lowpower.signed.bin` here versus
`…-bringup.signed.bin` for the console build. Use those when picking an OTA
payload; see [`ota-over-ble.md`](ota-over-ble.md).

## Tune the two real duty cycles

1. **Advertising interval.** The default is 1 s. Increasing
   `BLE_ADV_INTERVAL_MS` reduces radio events, but the Hub scan window must be
   several times longer than the interval. Measure packet reception before
   deploying a larger value. A missed beacon that causes extra scans or site
   visits is not a power saving.
2. **Sensor interval.** Vibration capture lasts about 2.5 s and microphone
   capture about 0.7 s, dwarfing idle leakage during those seconds. Increase
   `MEASURE_INTERVAL_MS` to ten minutes if one new sample per ten-minute report
   is acceptable. Do not shorten acquisition merely to align with a scan: the
   latest result remains in the controller.

Both are compile-time overrides, for example
`-DEXTRA_CFLAGS=-DMEASURE_INTERVAL_MS=600000U` (or the equivalent build-system
flag). Change one variable at a time and compare charge per complete cycle, not
only the lowest current displayed between events.

## Device power management is deliberately left off

`CONFIG_PM_DEVICE=y` looks like an obvious addition to a low-power profile, but
on its own it does nothing here. Neither `prj.conf` nor the Seeed board
defconfig enables `CONFIG_PM` (system power management), and the profile does
not enable `CONFIG_PM_DEVICE_RUNTIME`. Without one of those, no code path ever
runs a device's suspend action: `CONFIG_PM_DEVICE` only compiles in the
machinery. The result is extra flash, a per-device PM state, and altered driver
init paths — several Nordic drivers behave differently under device PM — in
exchange for no measured saving.

If device PM is worth pursuing, do it as its own change: enable
`CONFIG_PM_DEVICE_RUNTIME=y` (or `CONFIG_PM=y`) as well, re-verify that I²C,
PDM, and the nPM1300 rail still come up, and compare integrated charge against
the profile without it. The peripherals that stay powered between cycles here
are few, so measure before assuming the saving exists.

## Do not use system-off in normal beacon mode

Seeed's low-power example suspends the console and calls `sys_poweroff()`.
That is appropriate for shipping/storage mode, but system-off resets the CPU
on wake and stops continuous BLE advertising. It therefore breaks HiveHub's
unsynchronised passive scan. A future shipping mode can use a button or timed
wake source, but it must be a separate user-selected mode.

Likewise, do not periodically stop and restart BLE without a synchronised Hub.
The existing controller-managed interval lets the CPU sleep while retaining
reliable asynchronous discovery.

## Hardware measurement checklist

* Measure from the battery input with USB disconnected. The on-board debugger,
  USB bridge, and charger can dominate a measurement made through USB.
* Remove the power analyzer's debugger connection after flashing; an attached
  SWD probe can prevent the lowest hardware state or back-power I/O.
* Capture at least one whole five-minute cycle. Record idle baseline,
  advertising spikes, sensor-rail enable/capture, and total integrated charge.
* Verify LDO1 is actually off between cycles. If it is not, check the nPM1300
  I2C pin override and the firmware's `[PWR]` errors with a diagnostic build.
* Check for external SHT40 breakout pull-ups or regulator LEDs. A convenient
  breakout can consume much more than the sensor itself; use a bare low-leakage
  board or switch its supply in the final hardware.
* Compare normal and deployment images on the same board, battery voltage,
  temperature, advertising interval, and analyzer range.

## Source basis and limits

The recommendations were checked against the complete firmware tree and these
upstream resources (accessed 2026-08-03):

* [Zephyr system power management](https://docs.zephyrproject.org/latest/services/pm/system.html)
  explains idle/system states and residency decisions.
* [Zephyr device power management](https://docs.zephyrproject.org/latest/services/pm/device.html)
  covers device suspend and runtime PM.
* [Zephyr system-off sample](https://docs.zephyrproject.org/latest/samples/boards/nordic/system_off/README.html)
  documents wake/reset behavior for Nordic targets.
* [Seeed's Zephyr low-power example](https://github.com/Seeed-Studio/platform-seeedboards/tree/main/examples/zephyr-lowpower)
  explicitly suspends the console before system-off.
* [Seeed's XIAO nRF54LM20A board definition](https://github.com/Seeed-Studio/platform-seeedboards/tree/main/zephyr/boards/arm/xiao_nrf54lm20a)
  is the authority for DC/DC mode, UART sleep pinctrl, PMIC, and sensor rails.
* [Nordic nRF54L Series power optimization guide](https://docs.nordicsemi.com/bundle/nwp_045/page/WP/nwp_045/intro.html)
  gives the SoC-level measurement and optimization background.

Upstream repositories and documentation can change. The links justify the
mechanisms, not a promised board current. This project has not yet been
hardware-validated, so no new microamp or battery-life claim is made here.

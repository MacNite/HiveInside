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
* More importantly, nPM1300 LDO1 (the IMU/microphone rail) **and** the upstream
  `power_en` gate (P1.12) are disabled after both captures and enabled only
  immediately before the next acquisition. LDO1 alone is not enough: the fixed
  regulator in front of it feeds the whole Sense sensor island, so leaving it on
  keeps a powered standby path alive for the ~99 % of each cycle that measures
  nothing.
* `power_init()` releases the reference Zephyr's regulator core takes at its own
  init before taking one of its own. This is what makes the two bullets above
  true rather than merely intended. `regulator_disable()` reaches the hardware
  only when the reference count hits zero, and the core starts at one whenever
  it finds a rail already on — `regulator-boot-on` does that for `power_en` and
  for LDO1, and the nPM1300 does it after any CPU reset taken while the rail was
  up, because the PMIC is a separate chip that a reset does not touch. Without
  the release, the count would sit at two, every `power_sensor_rail_disable()`
  would decrement it to one, and the rail would stay powered for the life of the
  node while the firmware reported success.
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
the measurement LED, and removes the `regulator-boot-on` defaults from
`power_en` and LDO1 so the sensor island is not held powered from reset until
the first cycle switches it. BLE measurement advertisements and BLE OTA are
unchanged. The UART is the most important idle-only firmware difference:
leaving a serial peripheral and its pins active solely for an unattended
console is needless.

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

Keep the bring-up image and the deployment image as two build configurations so
both exist side by side and you can flash either one. The complete setup —
every field, the verification checklist, and the two ways this form invites you
to build a quietly broken image — is in
[`vscode-build.md`](vscode-build.md). In short, the deployment configuration
differs from the bring-up one in exactly three fields:

| Field | Value |
|---|---|
| Build directory name | `build_lowpower` |
| Extra Kconfig fragments | `low-power.conf` |
| Extra Devicetree overlays | `low-power.overlay` |

Both **Base** fields stay empty. They are `CONF_FILE` and `DTC_OVERLAY_FILE`,
which *replace* `prj.conf` and `app.overlay` rather than adding to them —
putting the profile there costs you Bluetooth and the MCUboot image manager, or
silently the nPM1300 pin fix, the PDM microphone and the watchdog.

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

## What the sensor-rail gating is worth

An order-of-magnitude estimate, so the measurement below has something to
falsify. It is arithmetic over datasheet-class figures, not a claim about this
board — see the limits at the end of this document.

The cycle is 300 s. Acquisition holds the rail up for roughly 3.3 s (about
2.5 s of vibration capture, 0.7 s of microphone capture, plus the 20 ms settle),
so the rail is **off for about 98.9 % of the time**. Everything below is
therefore very close to a straight saving on the average current.

What is downstream of the two rails while they are on but idle:

| Item | Typical standby | Note |
|---|---|---|
| LSM6DS3TR-C, power-down | ~3 µA | `accel.c` already powers it down after capture |
| MSM261DGT006 PDM mic, clock stopped | ~1–20 µA | family datasheets vary; the widest source of error here |
| nPM1300 LDO1 enabled, unloaded | a few µA | quiescent of the regulator itself |
| Sensor-island path behind `power_en` | board-specific | pull-ups and the gate's own quiescent |

So gating both rails is worth **single-digit to low-tens of µA of idle
current** — call it 5–25 µA, with the microphone's standby figure dominating
the uncertainty. Against a 300 s cycle that is 5–25 µA of average current,
or roughly 0.12–0.6 mAh per day.

Two things put that in proportion:

* **It is not the largest item in the budget.** The 3.3 s acquisition burst
  draws milliamps: at an assumed 3 mA it contributes about 33 µA of *average*
  current on its own, and continuous 1 s advertising is of the same order.
  Rail gating improves the idle floor; it does not change the shape of the
  budget. `MEASURE_INTERVAL_MS` and `BLE_ADV_INTERVAL_MS` still are the two
  knobs that matter most, which is why they have their own section above.
* **Most of it was already meant to be there.** The rail gating was written
  before this; what was missing was the reference-count release, without which
  `regulator_disable()` never reached the hardware. The honest way to read the
  numbers above is not "a new saving" but "the saving the previous change was
  already claiming, now actually taken".

The two `/delete-property/ regulator-boot-on` lines in the overlay are worth
close to nothing on their own — they remove the rails from a few hundred
milliseconds of boot, and spare `power_init()` one disable/re-enable. They are
in the profile because a deployment image should not assert a rail it manages
itself, not because they move the average.

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

## The external SPI flash is left unprobed, which is not the same as off

The board's 8 MB PY25Q64 is unused: the measurement beacon never touches it and
MCUboot's slots are in on-chip RRAM. Its devicetree node ships
`status = "disabled"` in the board tree, so nothing probes it and no overlay
line is needed — adding `&py25q64 { status = "disabled"; }` to the deployment
profile changes nothing.

That is worth stating explicitly because Seeed's low-power guide does the
*opposite*, enabling the node. The reason is that an unprobed SPI NOR sits in
its power-on standby state, which is typically tens of µA, whereas the part's
deep-power-down state is around 1 µA — and reaching DPD needs a driver to issue
the command. The node already carries `has-dpd`, `t-enter-dpd` and
`t-exit-dpd`, and Zephyr's `spi_nor` driver has `CONFIG_SPI_NOR_IDLE_IN_DPD`
for exactly this, so the change is small.

It is not made here because it is not free: enabling the node builds the SPI
driver and `spi00`, and the SPI pin states while idle then matter. On the
figures in the section above it is potentially the same order as the whole
sensor-rail saving, so it is worth doing — as its own change, with a
before/after measurement on the assembled board rather than on trust.

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
* Verify LDO1 **and** P1.12 (`power_en`) are actually off between cycles, and
  that they are still off after a watchdog reset — that is the case the
  reference-count release in `power_init()` exists for, and the one a short
  bench run will not show you. If a rail stays up, check the nPM1300 I2C pin
  override and the firmware's `[PWR]` errors with a diagnostic build.
* Check for external SHT40 breakout pull-ups or regulator LEDs. A convenient
  breakout can consume much more than the sensor itself; use a bare low-leakage
  board or switch its supply in the final hardware.
* Compare normal and deployment images on the same board, battery voltage,
  temperature, advertising interval, and analyzer range.

## Source basis and limits

The recommendations were checked against the complete firmware tree and these
upstream resources (accessed 2026-08-05):

* [Zephyr system power management](https://docs.zephyrproject.org/latest/services/pm/system.html)
  explains idle/system states and residency decisions.
* [Zephyr device power management](https://docs.zephyrproject.org/latest/services/pm/device.html)
  covers device suspend and runtime PM.
* [Zephyr system-off sample](https://docs.zephyrproject.org/latest/samples/boards/nordic/system_off/README.html)
  documents wake/reset behavior for Nordic targets.
* [Seeed's XIAO nRF54LM20A low-power guide](https://wiki.seeedstudio.com/xiao_nrf54lm20a_with_low_power/)
  names the external flash, the PMIC LED engine, `power_en` and LDO1 as the
  board's low-power-relevant devicetree nodes. Check its overlay snippets
  against the board tree you actually build with before copying them: the
  `pmic_leds` label it uses does not exist in either tree this firmware builds
  against, and it enables the flash node rather than disabling it.
* [Zephyr's `regulator_common.c`](https://github.com/zephyrproject-rtos/zephyr/blob/main/drivers/regulator/regulator_common.c)
  is the authority for the reference-count behaviour `power_init()` works
  around — `regulator_common_init()` counts a rail it finds enabled, and
  `regulator_disable()` acts only at a count of zero.
* [Seeed's Zephyr low-power example](https://github.com/Seeed-Studio/platform-seeedboards/tree/main/examples/zephyr-lowpower)
  explicitly suspends the console before system-off.
* [Seeed's XIAO nRF54LM20A board definition](https://github.com/Seeed-Studio/platform-seeedboards/tree/main/zephyr/boards/arm/xiao_nrf54lm20a)
  is the authority for DC/DC mode, UART sleep pinctrl, PMIC, and sensor rails.
* [Nordic nRF54L Series power optimization guide](https://docs.nordicsemi.com/bundle/nwp_045/page/WP/nwp_045/intro.html)
  gives the SoC-level measurement and optimization background.

Upstream repositories and documentation can change. The links justify the
mechanisms, not a promised board current. This project has not yet been
hardware-validated, so no new microamp or battery-life claim is made here.

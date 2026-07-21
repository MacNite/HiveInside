# HiveInside — XIAO nRF54LM20A Sense firmware

This is the primary HiveInside firmware target: a Zephyr application for the
Seeed XIAO nRF54LM20A Sense. It currently preserves the initial bring-up
behavior: the board `led0` Devicetree alias blinks every 500 ms and a startup
message is printed at 115200 baud. It is not yet the complete sensor or BLE
application.

## PlatformIO with Zephyr (default)

The default contributor workflow is PlatformIO with Zephyr:

```bash
cd firmware-nrf54lm20a
pio run
pio run -t upload
pio device monitor
```

`platformio.ini` selects the `seeed-xiao-nrf54lm20a` board, the **Zephyr**
framework, and the GCC Arm Embedded toolchain version used by Seeed's nRF54LM20A
Zephyr examples. The Seeed PlatformIO platform is pinned to commit
`9ba53b691fb007d9c1b8fd37600cc71d6702125a` so builds do not silently follow
upstream `main`.

PlatformIO builds the normal Zephyr application files that remain checked into
this directory: `src/main.c`, root `CMakeLists.txt` and `prj.conf`, plus the
`zephyr/` metadata used by Seeed's PlatformIO builder. PlatformIO does **not**
use the Arduino framework for this board.

### Uploading

The checked-in `platformio.ini` does not override the board upload setting, so
`pio run -t upload` uses the pinned Seeed board definition's `cmsis-dap` default.
That definition also lists pyOCD, probe-rs, and J-Link as optional protocols.
They are not the default contributor workflow and require a verified compatible
probe and board revision. UF2 is not configured by the checked-in board
definition.

No hardware upload has been validated by this repository's bring-up build.
Follow the PlatformIO upload output for the resolved platform revision and
connected debug hardware. If a board revision or a local board definition changes
the available protocol, verify that route against the exact revision before using
it.

### Current scope and OTA status

This is bring-up firmware only. MCUboot and Zephyr DFU have not been integrated,
so firmware-over-BLE OTA does **not** work on the nRF54LM20A target yet. The
ESP32-C6 prototype's separate Arduino OTA implementation is retained only for
historical testing and migration reference; it is not shared with this project.

### Updating the pinned Seeed platform

Update this dependency only intentionally: choose a candidate commit from
`Seeed-Studio/platform-seeedboards`, replace the full SHA in `platformio.ini`,
then run a clean build to validate the board and framework integration:

```bash
cd firmware-nrf54lm20a
pio run -t clean
pio run
```

Confirm that PlatformIO resolved the platform checkout to that exact SHA (for
example, with `git -C ~/.platformio/platforms/SeeedStudio rev-parse HEAD`) before
committing the updated SHA and this documentation. Review any Zephyr framework
or GCC Arm Embedded toolchain changes introduced by the candidate commit; do
not use an automatic dependency updater for this platform.

## Advanced: Zephyr / `west` alternative

The application source and root Zephyr files remain suitable for an existing
Zephyr or nRF Connect SDK workspace. This is an advanced alternative, not the
beginner default:

```bash
west build -b xiao_nrf54lm20a/nrf54lm20a/cpuapp firmware-nrf54lm20a
```

For this route, install or expose the Seeed XIAO nRF54LM20A board definitions in
that workspace as required by the SDK version in use. Its flash procedure and
available debug probes are determined by that workspace and board revision.

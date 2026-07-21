# HiveInside — XIAO nRF54LM20A firmware

This is a conventional Zephyr application for the Seeed XIAO nRF54LM20A. It
currently preserves the bring-up behavior: the board `led0` Devicetree alias
blinks every 500 ms and a startup message is printed at 115200 baud.

## PlatformIO (default)

The default build, upload, and monitor interface is PlatformIO with Seeed's
Zephyr platform:

```bash
cd firmware-nrf54lm20a
pio run
pio run -t upload
pio device monitor
```

`platformio.ini` selects `seeed-xiao-nrf54lm20a`, Zephyr, and the GCC Arm
Embedded toolchain version used by Seeed's current nRF54LM20A Zephyr examples.
The Seeed PlatformIO platform is pinned to commit
`9ba53b691fb007d9c1b8fd37600cc71d6702125a` so builds do not silently follow
upstream `main`. Test candidate revisions with the clean PlatformIO build below
before intentionally updating this pin.
The Seeed board definition configures CMSIS-DAP as the default upload protocol;
connect a compatible SWD probe before uploading. If no probe is connected,
`pio run -t upload` will not complete.

Build output is written under
`.pio/build/seeed-xiao-nrf54lm20a/`, including `zephyr/zephyr.elf` and the
corresponding Zephyr image formats produced by the selected platform version.

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

## Zephyr / west alternative

The application source and root Zephyr files are retained, so an existing
Zephyr or nRF Connect SDK workspace can still build it:

```bash
west build -b xiao_nrf54lm20a/nrf54lm20a/cpuapp firmware-nrf54lm20a
```

For this route, install or expose the Seeed XIAO nRF54LM20A board definitions
to that Zephyr workspace as required by the SDK version in use. PlatformIO
uses the `zephyr/` directory for its application metadata because that is the
layout expected by Seeed's Zephyr builder; both entry points compile the same
`src/main.c` and use the same minimal GPIO configuration.

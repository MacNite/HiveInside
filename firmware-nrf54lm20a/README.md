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
The Seeed board definition configures CMSIS-DAP as the default upload protocol;
connect a compatible SWD probe before uploading. If no probe is connected,
`pio run -t upload` will not complete.

Build output is written under
`.pio/build/seeed-xiao-nrf54lm20a/`, including `zephyr/zephyr.elf` and the
corresponding Zephyr image formats produced by the selected platform version.

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

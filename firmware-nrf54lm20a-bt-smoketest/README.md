# nRF54LM20A Bluetooth smoke test

This minimal PlatformIO/Zephyr application separates basic board bring-up from
the production HiveInside sensor and beacon firmware. It deliberately keeps
the production firmware's PlatformIO platform, board, Bluetooth host/controller
selection, console, and upload protocol unchanged.

It does only three things:

1. prints a boot message;
2. blinks `led0` once (when the board exposes that alias);
3. calls `bt_enable(NULL)` and prints `PASS` if Bluetooth initialisation
   completes.

This is **not a blink-only test**. A blink-only image cannot exercise the
Bluetooth `net_buf` pool that faults in the production image. The test removes
PMIC, I2C, microphone, IMU, FFT, GATT, advertising, and application beacon
code while retaining the exact Bluetooth initialisation path.

## Build, upload, and monitor

Open this directory in VSCodium, then use its integrated terminal:

```bash
pio run -t clean
pio run
pio run -t upload
pio device monitor
```

Expected success output:

```text
[BT-SMOKE] boot: blink then bt_enable()
[BT-SMOKE] calling bt_enable()
[BT-SMOKE] PASS: Bluetooth enabled
```

If the device faults after `calling bt_enable()`, this reproduces the failure
without production application code. That strongly implicates the shared
PlatformIO/Zephyr framework, board definition, generated devicetree, or
Bluetooth configuration rather than HiveInside's sensor/beacon code.

## Interpretation

| Result | Meaning |
| --- | --- |
| LED blinks and `PASS` prints | Basic board and Bluetooth initialisation work; compare the production-only configuration and source next. |
| LED blinks, then an MPU fault occurs after `calling bt_enable()` | The minimal image reproduces the Bluetooth startup defect; sensor/PMIC/beacon code is excluded. |
| No boot message or no blink | The issue is below Bluetooth and this test is not yet informative. |

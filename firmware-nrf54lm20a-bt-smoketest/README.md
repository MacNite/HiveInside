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

## Current test: production Bluetooth buffers, FPU, and stack

Test 1 passed on the target, so this smoke test now includes **Test 2** of the
isolation plan. It retains the production Bluetooth connection, L2CAP MTU, and
ACL buffer-size settings:

```ini
CONFIG_BT_MAX_CONN=1
CONFIG_BT_L2CAP_TX_MTU=247
CONFIG_BT_BUF_ACL_RX_SIZE=251
CONFIG_BT_BUF_ACL_TX_SIZE=251
```

It also enables the production FPU, floating-point `cbprintf` support, and
8 KiB main stack:

```ini
CONFIG_FPU=y
CONFIG_CBPRINTF_FP_SUPPORT=y
CONFIG_MAIN_STACK_SIZE=8192
```

No PMIC, I2C, audio, sensor, GATT, advertising, or beacon configuration has
been added. Therefore, an MPU fault at `bt_enable()` now points to the Test 2
runtime/RAM-layout configuration (or the framework path it exercises), rather
than those excluded subsystems.

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
| LED blinks and `PASS` prints | The production ACL/L2CAP buffer sizes, FPU support, float formatting, and 8 KiB main stack work; continue with the next configuration batch. |
| LED blinks, then an MPU fault occurs after `calling bt_enable()` | The minimal image reproduces the Bluetooth startup defect; sensor/PMIC/beacon code is excluded. |
| No boot message or no blink | The issue is below Bluetooth and this test is not yet informative. |

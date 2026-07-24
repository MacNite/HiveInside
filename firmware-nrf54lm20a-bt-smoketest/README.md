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
PMIC, I2C, IMU, microphone capture, FFT, GATT, advertising, and application
beacon code while retaining the exact Bluetooth initialisation path.

## Current test: PDM microphone controller initialisation

Tests 1 and 2 passed on the target, so this smoke test now includes **Test 3**
of the isolation plan. It retains the production Bluetooth connection, L2CAP
MTU, ACL buffer-size, FPU, float-formatting, and main-stack settings:

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

Test 3 adds the production PDM microphone driver and enables the on-board
`pdm20` controller:

```ini
CONFIG_AUDIO=y
CONFIG_AUDIO_DMIC=y
```

The smoke test does not capture audio. Enabling the controller is sufficient
to exercise its devicetree and early driver initialisation before `main()`
calls `bt_enable()`. PMIC/LDO1 configuration, I2C, sensors, GATT,
advertising, and beacon code remain excluded. Therefore, an MPU fault now
points to the Test 3 PDM configuration or its interaction with the existing
runtime layout, rather than those excluded subsystems.

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
| LED blinks and `PASS` prints | The Test 1/2 runtime settings and PDM controller initialisation work; continue with the PMIC/sensor configuration batch. |
| LED blinks, then an MPU fault occurs after `calling bt_enable()` | The minimal image reproduces the Bluetooth startup defect; sensor/PMIC/beacon code is excluded. |
| No boot message or no blink | The issue is below Bluetooth and this test is not yet informative. |

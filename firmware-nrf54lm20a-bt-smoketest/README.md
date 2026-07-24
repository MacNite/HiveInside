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
the production application-level I2C, IMU, microphone-capture, FFT, GATT,
advertising, and beacon code while retaining the exact Bluetooth
initialisation path.

## Current test: nPM1300 LDO1 and battery gauge

Tests 1, 2, and 3 passed on the target, so this smoke test now includes
**Test 5** of the isolation plan. Test 4 also passed, so this test retains the
production Bluetooth connection, L2CAP MTU, ACL buffer-size, FPU,
float-formatting, main-stack, PDM, and PMIC/I2C/sensor framework settings:

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
calls `bt_enable()`.

Test 4 adds the production I2C, sensor, regulator, and MFD Kconfig support:

```ini
CONFIG_I2C=y
CONFIG_SENSOR=y
CONFIG_REGULATOR=y
CONFIG_MFD=y
CONFIG_LSM6DSL=n
```

This allows the board's existing PMIC and sensor-related devicetree drivers to
initialise, while preserving the production choice not to start Zephyr's
LSM6DSL driver.

Test 5 adds the production LDO1 voltage and boot configuration, then prints
live nPM1300 diagnostic results before calling `bt_enable()`:

- whether the LDO1 regulator device is ready;
- the result of setting LDO1 to 3.3 V and enabling it; and
- the nPM1300 charger fuel-gauge voltage, when its sensor device is ready.

Application I2C transactions, IMU reads, microphone capture, GATT,
advertising, and beacon code remain excluded. Therefore, an MPU fault after
these messages points to the nPM1300 Test 5 path or its interaction with the
existing runtime layout, rather than those excluded subsystems.

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
[BT-SMOKE] testing nPM1300
[PMIC] LDO1 set 3.3V: 0
[PMIC] LDO1 enabled: yes
[PMIC] battery gauge: 3.900000 V
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
| LDO1 and battery-gauge messages print, then `PASS` prints | The Test 1–5 runtime, PDM, PMIC framework, LDO1 configuration, and battery gauge work; proceed by adding production application modules one at a time. |
| The PMIC messages show `not ready` or a negative error | The nPM1300/board devicetree or PMIC-driver path is the next issue to investigate; record the exact message before changing Bluetooth settings. |
| An MPU fault occurs before or after the PMIC messages | The Test 5 nPM1300 configuration/operation is the first newly introduced trigger. |
| No boot message or no blink | The issue is below Bluetooth and this test is not yet informative. |

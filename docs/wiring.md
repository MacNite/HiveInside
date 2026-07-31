# HiveInside wiring reference

HiveInside uses a **Seeed XIAO nRF54LM20A Sense**, an external **SHT40**, and a
500 mAh LiPo battery. The XIAO Sense integrates the IMU, PDM microphone, power
management, charging, and USB-C; no external accelerometer or microphone is
needed for the primary target.

## SHT40

Connect the SHT40 to the XIAO's I²C bus:

| SHT40 pin | XIAO connection | Notes |
|---|---|---|
| VIN / VDD | 3V3 | Use the sensor's 3.3 V supply. |
| GND | GND | Common ground. |
| SDA | D4 (SDA) | I²C data. |
| SCL | D5 (SCL) | I²C clock. |

The SHT40's fixed I²C address is `0x44`. Use the current Seeed pinout and the
board devicetree when selecting any additional XIAO pins.

## Battery

Connect the 500 mAh single-cell LiPo to the XIAO battery connector, observing
its polarity. The XIAO's on-board nPM1300 PMIC manages charging over USB-C and
fuel-gauge functions.

## Optional: external accelerometer for bench work

The firmware identifies the accelerometer by `WHO_AM_I` on any enabled I²C bus,
so an external LIS3DH/LIS2DH12 breakout also works for bench comparisons.
Strap `SDO/SA0 → GND` for address `0x18` (or `→ VCC` for `0x19`) and wire SDA/SCL
to the same D4/D5 pads as the SHT40. It is not needed on the Sense module, whose
on-board LSM6DS3TR-C sits on the internal `i2c30` bus.

## Enclosure

The current 3D-printable enclosure release is kept in
[`enclosure/`](../enclosure/). It is sized for the XIAO nRF54LM20A Sense, SHT40,
and 500 mAh LiPo.

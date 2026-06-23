# HiveInside wiring reference

Two targets share the same logical wiring: the **XIAO ESP32-C6** prototype and
the **XIAO nRF52840** final board (the module reflow-mounted on a carrier PCB).
Only the pad/pin numbers differ.

- Prototype pin map: [`esp32c6-prototype.md`](esp32c6-prototype.md) and
  `firmware/include/config.h`.
- Final board ([buy](https://s.click.aliexpress.com/e/_c2yM9y1r)): this document.

---

## Logical signal map

| Signal | Goes to | Notes |
|---|---|---|
| I²C SDA | SHT40, accelerometer, LPS22HB | 4.7 kΩ pull-up to 3.3 V |
| I²C SCL | SHT40, accelerometer, LPS22HB | 4.7 kΩ pull-up to 3.3 V |
| PDM CLK | mic CLK | 100 Ω series guard; MCU → mic |
| PDM DATA | mic DATA | 100 Ω series + 100 kΩ pull-down; mic → MCU |
| mic SEL (L/R) | GND | selects the LEFT slot |
| MIC_EN | P-MOSFET gate | LOW = mic powered; 10 kΩ gate pull-up |
| BUTTON | pushbutton to GND | internal pull-up, active low |
| VBAT sense | battery voltage | prototype: external divider → A0; final: on-board |

## I²C addresses (no conflicts)

| Device | Address | Strap |
|---|---|---|
| accelerometer (LIS3DH proto / LIS2DH12 final) | 0x18 | SA0 → GND, CS → VDD (I²C mode) |
| SHT40 | 0x44 | fixed |
| LPS22HB (final only) | 0x5C | SA0 → GND, CS → VDD (I²C mode) |

---

## Final board — XIAO nRF52840 pads

| Signal | XIAO pad | nRF52840 pin |
|---|---|---|
| I²C SDA | D4 | P0.04 |
| I²C SCL | D5 | P0.05 |
| PDM CLK | D6 | P1.11 |
| PDM DATA | D7 | P1.12 |
| mic SEL (L/R) → GND | — | LEFT slot |
| MIC_EN | D9 | P1.14 |
| BUTTON | D3 | P0.29 |
| VBAT sense | on-board | P0.31 (AIN7), enabled by P0.14 |

> The XIAO nRF52840 has a **dedicated hardware PDM peripheral** (it was designed
> for PDM mics), so the final board needs only the two CLK/DATA lines — one fewer
> signal than the old I²S mic, leaving D8/P1.13 free. The nRF52840 also reads its
> own cell voltage on-board (P0.31, gated by P0.14), so the carrier needs no
> battery divider. Flashing is over USB-C (UF2 bootloader) — no SWD header to
> populate.

---

## Power & decoupling

The XIAO module integrates the nRF52840's DC/DC converter, 3.3 V regulator,
32 kHz crystal, USB and LiPo charger, so the carrier only powers the sensors:

- 100 µF bulk electrolytic on the 3.3 V rail to absorb BLE TX bursts.
- 100 nF per IC at its VDD pin; plus 10 µF at the accelerometer and 4.7 µF at
  LPS22HB.

(On the prototype the XIAO ESP32-C6 likewise provides USB, charging and the
regulator; you just add the sensor breakouts.)

## Microphone power gating

```
3.3V ──Source─┤ P-MOSFET ├─Drain── mic VDD
                  Gate
                   │
   10kΩ ──── 3.3V  │  (default off)
                   │
              MIC_EN GPIO  (LOW = on)
```

Firmware sequence: set the PDM CLK/DATA pins to INPUT, then drive the gate HIGH to
power down — prevents back-feeding the mic through its ESD diodes. The series
100 Ω resistors on CLK/DATA are the second line of defence.

---

## Battery

| Option | Capacity | Notes |
|---|---|---|
| CR2477 | ~1000 mAh | recommended coin cell (handles BLE bursts better than CR2032) |
| LiPo 150–300 mAh | varies | charges on-board via the XIAO's USB-C |
| 2×AAA | ~1000 mAh | user-replaceable standard cells |

Both XIAO boards charge a LiPo over USB-C. CR2032 works for short-lived
prototypes but its 0.2 mA continuous-drain rating makes BLE/mic-capture peaks
hard on it — prefer CR2477 or LiPo for deployment.

> Set the XIAO nRF52840 charge current with P0.13 (HIGH = 50 mA, LOW = 100 mA) to
> match your cell's recommended charge rate.

---

## Mechanical

- LPS22HB has a pressure port hole — keep copper/soldermask clear over it and add
  a small vent hole in the PCB beneath it.
- SHT40 needs a PTFE/ePTFE membrane or protective window: the hive interior has
  propolis, condensation and chemical cleaning by bees.
- Keep copper clear around the XIAO's on-module antenna; no traces underneath it.

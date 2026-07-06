# HiveInside wiring reference

Two targets share the same logical wiring: the **XIAO ESP32-C6** prototype and
the **XIAO nRF54LM20A Sense** final board (the module reflow-mounted on a carrier
PCB). The big difference on the final board is that the **IMU and PDM mic are
on-board** — only the I²C climate sensors, the button and the battery are
external.

- Prototype pin map: [`esp32c6-prototype.md`](esp32c6-prototype.md) and
  `firmware/include/config.h`.
- Final board ([buy](https://www.seeedstudio.com/Seeed-Studio-XIAO-nRF54LM20A-Sense-p-6840.html),
  or the [XIAO nRF54L15 Sense](https://www.seeedstudio.com/XIAO-nRF54L15-Sense-p-6494.html)):
  this document.

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
| accelerometer — prototype LIS3DH (final: on-board LSM6DS3TR-C, wired by the board) | 0x18 (LIS3DH) | SA0 → GND, CS → VDD (I²C mode) |
| SHT40 (external, both boards) | 0x44 | fixed |
| LPS22HB (external, final only, optional) | 0x5C | SA0 → GND, CS → VDD (I²C mode) |

---

## Final board — XIAO nRF54LM20A Sense

On the nRF54LM20A Sense the **6-axis IMU (LSM6DS3TR-C) and PDM mic
(MSM261DGT006) are on the module**, wired to the SoC by Seeed's board files — so
the carrier does *not* route a mic or accelerometer. Firmware reaches them
through the board's devicetree nodes, not through carrier traces. The carrier
only has to break out the external I²C climate sensors and the button:

| Signal | XIAO pad | Goes to |
|---|---|---|
| I²C SDA | D4 | SHT40 (+ optional LPS22HB) |
| I²C SCL | D5 | SHT40 (+ optional LPS22HB) |
| BUTTON | a free D-pin | pushbutton to GND, internal pull-up |
| VBAT / charge | on-board | handled by the on-board **nPM1300** PMIC |

> **Confirm the exact XIAO pad → SoC pin mapping against the Seeed board
> devicetree** (`xiao_nrf54lm20a`) before layout — the nRF54L pin naming differs
> from the old nRF52840 `P0.xx`/`P1.xx` map, and the Sense board reserves several
> pads for its on-board IMU/mic. The nRF54LM20A does have a **hardware PDM
> peripheral** (PDM→PCM in hardware, so no software decimator is needed), but on
> this board the mic is already routed on-module. Flashing is over USB-C via the
> board's on-board **CMSIS-DAP** debugger (`west flash`); some board revisions
> also expose a UF2 drag-and-drop path — check the Seeed wiki.

---

## Power & decoupling

The XIAO nRF54LM20A Sense integrates the SoC's DC/DC converter, 3.3 V regulator,
32 kHz crystal, USB-C and the **nPM1300** PMIC (LiPo charging + fuel gauge), so
the carrier only powers the external sensors:

- 100 µF bulk electrolytic on the 3.3 V rail to absorb BLE TX bursts.
- 100 nF per external IC at its VDD pin; plus 4.7 µF at the LPS22HB. (The on-board
  IMU and mic are decoupled on the Sense module.)

(On the prototype the XIAO ESP32-C6 likewise provides USB, charging and the
regulator; you just add the sensor breakouts.)

## Microphone

The MSM261DGT006 PDM mic is **on the XIAO nRF54LM20A Sense module**, powered and
routed by the board — there is no external mic, series guard or power-gating
MOSFET to place on the carrier. Firmware saves power by only clocking the PDM
peripheral for the short capture window each cycle (see
[`firmware-nrf54lm20a/`](../firmware-nrf54lm20a)); it does not switch a discrete
mic supply.

> On the ESP32-C6 prototype, where the mic is an external MP34DT01 breakout, a
> P-MOSFET on `MIC_EN` gates its supply and 100 Ω series resistors guard CLK/DATA
> — see [`esp32c6-prototype.md`](esp32c6-prototype.md).

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

> On the XIAO nRF54LM20A Sense the **nPM1300 PMIC** handles LiPo charging and
> gives a fuel gauge (battery voltage / current), so the carrier needs no battery
> divider and the charge current is configured through the PMIC (over I²C / its
> Zephyr driver) rather than a fixed strap pin. Confirm the exposed charge-config
> net against the Seeed schematic.

---

## Mechanical

- LPS22HB has a pressure port hole — keep copper/soldermask clear over it and add
  a small vent hole in the PCB beneath it.
- SHT40 needs a PTFE/ePTFE membrane or protective window: the hive interior has
  propolis, condensation and chemical cleaning by bees.
- Keep copper clear around the XIAO's on-module antenna; no traces underneath it.

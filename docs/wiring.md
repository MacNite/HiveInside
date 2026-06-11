# HiveInside wiring reference

Two targets share the same logical wiring: the **Nice!Nano v2** prototype and
the **Ebyte E73-2G4M08S1C** production PCB. Logical signals are identical; only
the GPIO/pad numbers differ (see `firmware/include/config.h`).

---

## Logical signal map

| Signal | Goes to | Notes |
|---|---|---|
| I²C SDA | SHT40, LIS2DH12, LPS22HB | 4.7 kΩ pull-up to 3.3 V |
| I²C SCL | SHT40, LIS2DH12, LPS22HB | 4.7 kΩ pull-up to 3.3 V |
| I²S SCK | ICS-43434 SCK | 100 Ω series (phantom-power guard) |
| I²S WS | ICS-43434 WS | 100 Ω series |
| I²S SD | ICS-43434 SD | 100 Ω series + 100 kΩ pull-down |
| MIC_EN | P-MOSFET gate | LOW = mic powered; 10 kΩ gate pull-up |
| BUTTON | Pushbutton to GND | internal pull-up, active low |
| VBAT_ADC | Battery sense | via divider (prototype: on-board) |

## I²C addresses (no conflicts)

| Device | Address | Strap |
|---|---|---|
| LIS2DH12 | 0x18 | SA0 → GND, CS → VDD (I²C mode) |
| SHT40 | 0x44 | fixed |
| LPS22HB | 0x5C | SA0 → GND, CS → VDD (I²C mode) |

---

## Production PCB — Ebyte E73 pad numbers

From the official Ebyte E73-2G4M08S1C pinout:

| Signal | Module pad | nRF52840 pin |
|---|---:|---|
| I²C SDA | 12 | P0.26 |
| I²C SCL | 14 | P0.06 |
| I²S SCK | 35 | P0.24 |
| I²S WS | 34 | P0.22 |
| I²S SD | 30 | P0.17 |
| MIC_EN | 33 | P0.13 |
| BUTTON | 7 | P0.02 |
| VBAT ADC | 18 | P0.04 / AIN2 |
| SWDIO | 37 | SWDIO |
| SWDCLK | 39 | SWDCLK |
| VDD | 19 | VDD |
| VDDH | 23 | VDDH (battery) |
| DCCH | 25 | DC/DC cap (4.7 µF) |
| GND | 5, 21, 24 | GND |

> Avoid P0.00/P0.01 (32 kHz crystal), P0.09/P0.10 (NFC) and P0.18 (RESET) for
> general I/O.

---

## Power & decoupling

- No external regulator — battery (2.0–3.0 V) feeds VDD and VDDH directly; the
  nRF52840 runs its internal DC/DC.
- 100 µF bulk electrolytic on the battery rail to absorb BLE TX bursts.
- 100 nF per IC at its VDD pin; plus 10 µF at LIS2DH12 and 4.7 µF at LPS22HB.
- 4.7 µF on the nRF52840 DCCH (DC/DC) pin.

## Microphone power gating

```
3.3V ──Source─┤ P-MOSFET ├─Drain── ICS-43434 VDD
                  Gate
                   │
   10kΩ ──── 3.3V  │  (default off)
                   │
              MIC_EN GPIO  (LOW = on)
```

Firmware sequence: set I²S pins to INPUT, then drive gate HIGH to power down —
prevents back-feeding the mic through its ESD diodes. Series 100 Ω resistors on
SCK/WS/SD are the second line of defence.

---

## Battery

| Option | Capacity | Notes |
|---|---|---|
| CR2477 | ~1000 mAh | recommended coin cell (handles BLE bursts better than CR2032) |
| LiPo 150–300 mAh | varies | needs charge circuit; Nice!Nano has one on board |
| 2×AAA | ~1000 mAh | user-replaceable standard cells |

CR2032 works for short-lived prototypes but its 0.2 mA continuous-drain rating
makes BLE/I²S peaks hard on it — prefer CR2477 or LiPo for deployment.

---

## Mechanical

- LPS22HB has a pressure port hole — keep copper/soldermask clear over it and
  add a small vent hole in the PCB beneath it.
- SHT40 needs a PTFE/ePTFE membrane or protective window: the hive interior has
  propolis, condensation and chemical cleaning by bees.
- Keep copper clear around the E73 ceramic antenna; no traces under the antenna.

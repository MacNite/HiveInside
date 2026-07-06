# Hardware (KiCad)

This directory will hold the KiCad schematic and PCB layout for the HiveInside
**final** board: a carrier PCB with the Seeed
[XIAO nRF54LM20A Sense](https://www.seeedstudio.com/Seeed-Studio-XIAO-nRF54LM20A-Sense-p-6840.html)
module reflow-mounted (or the
[XIAO nRF54L15 Sense](https://www.seeedstudio.com/XIAO-nRF54L15-Sense-p-6494.html)
— pin-compatible for this design), plus the external I²C climate sensors.

The bill of materials with JLCPCB part numbers is in [`../bom.csv`](../bom.csv);
full wiring and layout notes are in [`../../docs/wiring.md`](../../docs/wiring.md).

Using the XIAO **Sense** module means the board inherits not just the module's
DC/DC, regulator, USB-C and **nPM1300** LiPo charger / fuel gauge, but also its
**on-board 6-axis IMU (LSM6DS3TR-C) and PDM mic (MSM261DGT006)** — so the carrier
drops the discrete accelerometer, mic and mic-gating parts entirely. Flashing is
over USB-C through the module's on-board CMSIS-DAP debugger, so there is no SWD
header to populate.

## To do

- [ ] Schematic capture (XIAO nRF54LM20A Sense footprint, external SHT40 + optional
      LPS22HB, button; IMU + mic are on-module)
- [ ] Confirm XIAO pad → nRF54 pin mapping against the Seeed board devicetree
- [ ] PCB layout (~25×20 mm, antenna keep-out, LPS22HB vent hole, PTFE window
      for SHT40)
- [ ] JLCPCB fabrication + PCBA export (Gerbers, BOM, CPL)

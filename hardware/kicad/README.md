# Hardware (KiCad)

This directory will hold the KiCad schematic and PCB layout for the HiveInside
**final** board: a carrier PCB with the Seeed
[XIAO nRF52840](https://s.click.aliexpress.com/e/_c2yM9y1r) module (the
SMD/castellated counterpart) reflow-mounted, plus the SMD sensors.

The bill of materials with JLCPCB part numbers is in [`../bom.csv`](../bom.csv);
full wiring and layout notes are in [`../../docs/wiring.md`](../../docs/wiring.md).

Using the XIAO module instead of a bare nRF52840 means the board inherits the
module's DC/DC, regulator, USB, LiPo charger and UF2 bootloader — so there is no
SWD header to populate and flashing is drag-and-drop over USB-C.

## To do

- [ ] Schematic capture (XIAO nRF52840 footprint, sensors, mic power gating, button)
- [ ] PCB layout (~25×20 mm, antenna keep-out, LPS22HB vent hole, PTFE window
      for SHT40)
- [ ] JLCPCB fabrication + PCBA export (Gerbers, BOM, CPL)

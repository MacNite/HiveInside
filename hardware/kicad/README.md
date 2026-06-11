# Hardware (KiCad)

This directory will hold the KiCad schematic and PCB layout for the HiveInside
production board (Ebyte E73-2G4M08S1C based).

The bill of materials with JLCPCB part numbers is in
[`../bom.csv`](../bom.csv); the full wiring and layout notes are in
[`../../docs/wiring.md`](../../docs/wiring.md).

## To do

- [ ] Schematic capture (E73 module, sensors, power gating, SWD header, button)
- [ ] PCB layout (~30×25 mm, antenna keep-out, LPS22HB vent hole, PTFE window
      for SHT40)
- [ ] JLCPCB fabrication + PCBA export (Gerbers, BOM, CPL)

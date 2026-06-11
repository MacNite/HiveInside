# Flashing HiveInside

## Prototype: Nice!Nano v2 (no programmer needed)

The Nice!Nano ships with a UF2 bootloader, so you flash over USB:

1. Open `firmware/` in **VSCodium** with the **PlatformIO** extension.
2. Connect the board via USB-C.
3. Run **PlatformIO: Upload** (env `nice_nano`), or on the CLI:
   ```bash
   pio run -e nice_nano -t upload
   ```
4. If upload can't find the port, double-tap **RESET** to force the bootloader;
   the board mounts as a USB drive and PlatformIO copies the `.uf2` across.

## Production: bare Ebyte E73 custom PCB

A bare nRF52840 has no bootloader, so the **first** flash needs an SWD
programmer. Recommended: **nRF52840-DK** (~$25, has an on-board J-Link that can
program external targets) or a J-Link.

Wire the DK's debug-out header to the PCB's 4-pin SWD header:

| Programmer | PCB SWD header |
|---|---|
| VTG / VDD | VDD (3.0 V) |
| SWDIO | SWDIO (pad 37) |
| SWDCLK | SWDCLK (pad 39) |
| GND | GND |

Then:
```bash
pio run -e e73_custom -t upload
```

### Optional: install a UF2 bootloader once

After the first SWD flash you can program the Adafruit nRF52 UF2 bootloader, and
from then on update over USB like the Nice!Nano — provided the board exposes a
USB port (VBUS/D+/D− on the E73).

## Note: this is a Nordic chip, not an ESP32

Unlike the HiveScale ESP32 firmware, there is **no serial/UART flashing** for a
fresh nRF52840 — it's SWD or (after bootloader) USB/UF2 only.

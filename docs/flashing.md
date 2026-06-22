# Flashing HiveInside

Both target boards flash over **USB-C with no external programmer** — that's a
deliberate reason for choosing the XIAO modules.

## Prototype: XIAO ESP32-C6

The C6 flashes over its native USB. In [`firmware/`](../firmware):

```bash
pio run -e c6_gatt -t upload
```

or use **PlatformIO: Upload** in VSCodium. If the port isn't found, hold **BOOT**,
tap **RESET**, then release BOOT to enter download mode and upload again.

## Final: XIAO nRF52840

The XIAO nRF52840 ships with the **Adafruit nRF52 UF2 bootloader**, so it flashes
like a USB drive — no SWD/J-Link needed even for the first flash:

1. Double-tap **RESET** to enter the bootloader; a `XIAO-SENSE` (or `XIAO BLE`)
   USB drive appears.
2. Copy the `.uf2` across (PlatformIO / Arduino can also upload directly), and the
   board reboots into the new firmware.

> The nRF52840 firmware target is the planned final build; today the buildable
> project is the ESP32-C6 prototype above. Using a XIAO module rather than a bare
> nRF52840 is what removes the SWD-programmer step — a bare chip has no
> bootloader and would need a J-Link/DK for its first flash.

After deployment, both boards also update over the air — see
[`ota-over-ble.md`](ota-over-ble.md).

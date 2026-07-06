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

## Final: XIAO nRF54LM20A Sense

The XIAO nRF54LM20A Sense builds with the **nRF Connect SDK (Zephyr)** — see
[`firmware-nrf54lm20a/`](../firmware-nrf54lm20a). It carries an **on-board
CMSIS-DAP debugger (SAMD11)**, so it flashes over plain USB-C with no external
J-Link:

```bash
west build -b xiao_nrf54lm20a/nrf54lm20a/cpuapp firmware-nrf54lm20a
west flash
```

Some board revisions also expose a **UF2 drag-and-drop** path: double-tap
**RESET** to mount the board as a USB drive and copy `build/zephyr/zephyr.uf2`
across. Check the [Seeed wiki](https://wiki.seeedstudio.com/xiao_nrf54lm20a_getting_started/)
for your revision.

> The nRF54LM20A Sense firmware target is the planned final build; today the
> buildable project is the ESP32-C6 prototype above. The
> [**XIAO nRF54L15 Sense**](https://www.seeedstudio.com/XIAO-nRF54L15-Sense-p-6494.html)
> is a drop-in alternative — same on-board sensors, same Zephyr/CMSIS-DAP flow;
> only the `west` board target changes (`xiao_nrf54l15/...`).

After deployment, both boards also update over the air — see
[`ota-over-ble.md`](ota-over-ble.md).

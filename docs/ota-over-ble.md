# Firmware-over-BLE (OTA) for HiveInside

HiveInside has no WiFi — it is a battery-powered BLE-only sensor. HiveScale is
the only node on the network with internet access, so it acts as the OTA relay:

```
backend (HTTPS .bin + CRC-32)  ──►  HiveScale  ──(BLE GATT)──►  HiveInside
        firmware_releases             (WiFi)        OTA service     (dual OTA slots)
```

This mirrors the way HiveScale already relays a BeeCounter image over I²C
(`updateBeeCounter` / `bee_counter_client`); only the last-mile transport differs
(GATT writes instead of I²C registers).

## Why a streaming relay

Firmware images may be too large for the relay to retain alongside its normal
workload. The HiveScale ESP32 (WROOM, no PSRAM) cannot buffer that in RAM, so
HiveScale **streams** the HTTPS download
straight into the GATT characteristics a chunk at a time and never holds the
whole image. HiveInside likewise buffers nothing: each chunk is written directly
to its inactive OTA slot. Nothing is committed until the final CRC check passes,
so a dropped or corrupted transfer always leaves the device on its old image.

## Partition layout and release artifact

The Seeed nRF54LM20A board definition already partitions its 2 MiB RRAM for
MCUboot: a 64 KiB boot partition, 449 KiB application slot 0, a matching 449 KiB
slot 1, corresponding non-secure slots, and storage. HiveInside deliberately
uses that board layout rather than carrying a second, easily-diverged overlay.
The application must remain below the 449 KiB slot limit.

`sysbuild.conf` enables MCUboot and `CONFIG_BOOTLOADER_MCUBOOT` causes the
application to be linked with its MCUboot header. A sysbuild release produces
`build/hiveinside/zephyr/zephyr.signed.bin` (or the PlatformIO build directory's
`zephyr/zephyr.signed.bin`); **that signed file**, including its MCUboot header,
is the object uploaded to the backend and whose CRC/size are sent in BEGIN.

## GATT protocol

All OTA characteristics live in the existing custom HiveInside service
`8e8b0001-7a1c-4b9e-9a2f-1d6e0b9c1a01`. UUIDs and framing must stay in sync with
HiveScale `firmware/src/ble_sensor.cpp` (the `HI_OTA_*` constants) and the
deprecated HiveInside ESP32-C6 prototype's
`firmware-esp32-c6/src/ble_link.cpp` (the `CHR_OTA_*` / `OTA_OP_*` constants).

| Characteristic | UUID | Props | Payload |
|---|---|---|---|
| OTA control | `8e8b0010-…` | Write | framed command (see below) |
| OTA data | `8e8b0011-…` | Write / Write-NR | raw firmware bytes, in order |
| OTA status | `8e8b0013-…` | Read / Notify | `state(1) + received(4 LE) + error(1)` |

### Control frames (first byte = opcode)

| Opcode | Bytes | Meaning |
|---|---|---|
| `0x01` BEGIN | `0x01 + size(4 LE) + crc32(4 LE)` | initialize slot stream; store expected CRC |
| `0x03` END | `0x03` | verify size + CRC, request test upgrade, reboot |
| `0x04` ABORT | `0x04` | cancel transfer; stay on current image |

### Status `state` byte

| Value | Meaning |
|---|---|
| `0x00` | idle |
| `0x01` | receiving |
| `0x02` | done (verified, rebooting) |
| `0x10`–`0x15` | error: begin / sequence / write / CRC / size / end |

### CRC-32

IEEE 802.3 (reflected poly `0xEDB88320`, init/final `0xFFFFFFFF`) — identical to
`zlib.crc32` on the backend and `beecnt::crc32_buf` on HiveScale, so the value the
backend computes at release time is verified unchanged on the device. End-to-end:
if HiveScale's download is corrupt, the device's CRC check fails and it aborts —
HiveScale cannot brick the sensor.

## Transfer sequence

1. Backend has an active `firmware_releases` row with `target = 'hiveinside'`
   (filename + crc32). An operator queues the update:
   `POST /api/v1/devices/{device_id}/commands/update-hiveinside?slot=1`.
2. On its next command poll HiveScale receives `update_hiveinside` with
   `{ slot, url, crc32 }`, resolves the slot's paired BLE MAC
   (`bleSensorMac0`/`bleSensorMac1`), and calls `updateHiveInside()`.
3. HiveScale opens the HTTPS GET, then opens the BLE session
   (`blesensor::otaBegin` → locate, connect, MTU exchange, BEGIN), and pumps the
   body into the DATA characteristic (`otaWrite`). Each DATA write is
   flow-controlled: the device only sends the ATT write-response after it has
   flashed the chunk.
4. HiveScale sends END (`otaFinish`) and polls STATUS. The device verifies size +
   CRC, flushes `flash_img`, requests `BOOT_UPGRADE_TEST`, and reports `done`.
5. A delayed Zephyr work item reboots HiveInside into the new slot ~1.5 s later (after the
   central has had a chance to read `done`).

The nRF54 advertises continuously with legacy connectable/scannable `ADV_IND`
at the same interval used by the former `ADV_SCAN_IND` beacon. Its primary
manufacturer payload and name+identity scan response are unchanged. A connection
which does not send BEGIN within six seconds is disconnected, and a short link
supervision timeout releases a vanished central. `ota_is_active()` gates the
sensor loop while connected or transferring.

## Safety and lifecycle

DATA callbacks call `flash_img_buffered_write()` synchronously before returning,
so HiveHub's write-with-response is flash-level flow control. CRC accumulation
starts at literal zero and chains `crc32_ieee_update()`, matching
`zlib.crc32`. END checks the exact received size and CRC before marking slot 1
pending. It then schedules a reset 1.5 seconds later, leaving time to read DONE.
The new image is a MCUboot **test** upgrade and confirms itself only after sensor,
Bluetooth, and application initialization; failure to boot causes MCUboot to
revert automatically.

Only the three OTA characteristics are exposed. A filter accept list would be a
stronger connection guardrail, but requires a future HiveHub bonding change.

## Build

From `firmware-nrf54lm20a/`, `pio run` builds the PlatformIO target. For an
explicit upstream Zephyr sysbuild, use `west build -b
xiao_nrf54lm20a/nrf54lm20a/cpuapp --sysbuild .`. Release automation must publish
the generated **signed** application binary, never the raw `zephyr.bin`.

## Deprecated ESP32-C6 reference

The ESP32-C6 `Update.h` implementation and its dual-OTA CSV remain only as a
protocol/state-machine reference. The primary nRF54 implementation is
`firmware-nrf54lm20a/src/ota.c`; it streams to the board's MCUboot secondary
slot, validates size and CRC, requests a test swap, and self-confirms after a
healthy boot. No placeholder GATT implementation exists.

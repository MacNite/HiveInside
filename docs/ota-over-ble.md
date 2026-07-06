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

A HiveInside XIAO ESP32-C6 image is well over 1 MB. The HiveScale ESP32 (WROOM, no
PSRAM) cannot buffer that in RAM, so HiveScale **streams** the HTTPS download
straight into the GATT characteristics a chunk at a time and never holds the
whole image. HiveInside likewise buffers nothing: each chunk is written directly
to its inactive OTA slot. Nothing is committed until the final CRC check passes,
so a dropped or corrupted transfer always leaves the device on its old image.

## Partition layout

`firmware/partitions_4mb_ota_no_fs.csv` gives the C6 two app slots (`ota_0` /
`ota_1`, ~1.94 MB each). It is selected in `platformio.ini` via
`board_build.partitions`. Without a dual-OTA table `Update.begin()` has nowhere
to write the new image. This matches HiveScale's table of the same name.

## GATT protocol

All OTA characteristics live in the existing custom HiveInside service
`8e8b0001-7a1c-4b9e-9a2f-1d6e0b9c1a01`. UUIDs and framing must stay in sync with
HiveScale `firmware/src/ble_sensor.cpp` (the `HI_OTA_*` constants) and HiveInside
`firmware/src/ble_link.cpp` (the `CHR_OTA_*` / `OTA_OP_*` constants).

| Characteristic | UUID | Props | Payload |
|---|---|---|---|
| OTA control | `8e8b0010-…` | Write | framed command (see below) |
| OTA data | `8e8b0011-…` | Write / Write-NR | raw firmware bytes, in order |
| OTA status | `8e8b0013-…` | Read / Notify | `state(1) + received(4 LE) + error(1)` |

### Control frames (first byte = opcode)

| Opcode | Bytes | Meaning |
|---|---|---|
| `0x01` BEGIN | `0x01 + size(4 LE) + crc32(4 LE)` | `Update.begin(size)`; store expected CRC |
| `0x03` END | `0x03` | verify size + CRC, `Update.end(true)`, reboot |
| `0x04` ABORT | `0x04` | `Update.abort()`, stay on current image |

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
   CRC, calls `Update.end(true)` and reports `done`.
5. `ble::loopOta()` reboots HiveInside into the new slot ~1.5 s later (after the
   central has had a chance to read `done`).

During a transfer HiveInside suppresses deep sleep and skips measurement
(`ble::isOtaActive()` gates `loop()` in `main.cpp`).

## Build flags

* HiveInside: `-DHIVEINSIDE_OTA_ENABLED=1` (default in `platformio.ini`).
* HiveScale: `HIVEINSIDE_OTA_ENABLED` (default `1` in `firmware/include/config.h`).
  Independent of `HIVEINSIDE_USE_GATT`, which only selects how *measurements* are
  read — OTA needs only a GATT-client connection.

## Limitations / notes

* The relay needs the device paired (its MAC stored in a HiveScale BLE slot) and
  reachable during HiveScale's wake window. If HiveInside is asleep, the locate
  scan may miss it; re-queue or widen its connect/listen window.
* Transfer time is roughly `image_size / (chunk × writes-per-second)`. With a
  negotiated MTU of ~247 (chunk ≈ 244 B) and write-with-response flow control,
  expect a few minutes for a ~1.3 MB image — acceptable for an infrequent update.
* This is prototype firmware (XIAO ESP32-C6). The final XIAO nRF54LM20A Sense
  board runs the nRF Connect SDK (Zephyr), so its on-device flash handling is
  Zephyr **MCUboot/DFU** rather than ESP `Update.h`. Note the `firmware-nrf54lm20a/`
  bring-up currently favours a **non-connectable BLE beacon** for lowest power
  (see its README); the connectable-GATT OTA relay described here maps cleanly to
  the ESP32-C6 build, and would apply to the nRF54 build only if/when it adds a
  connectable GATT service.

# Firmware-over-BLE (OTA) for HiveInside

HiveInside has no Wi-Fi. HiveHub downloads a release over HTTPS and streams it
through BLE to the XIAO nRF54LM20A Sense. The device writes each GATT DATA write
directly to MCUboot's inactive slot; it never buffers an image in RAM and never
marks an image bootable until END verifies its length and CRC.

## MCUboot layout and safety

The XIAO board definition divides its 2 MiB RRAM into a 64 KiB MCUboot region,
two 449 KiB application slots (plus their secure/non-secure companions), and
storage. `sysbuild.conf` enables MCUboot and `CONFIG_BOOTLOADER_MCUBOOT` builds
the application for slot 0. Zephyr's `flash_img` API targets slot 1.

An interrupted, aborted, oversized, or corrupt upload may leave bytes in slot 1,
but it cannot replace the running image: only a correctly sized, CRC-verified
image reaches `boot_request_upgrade(BOOT_UPGRADE_TEST)`. MCUboot then boots it
as a test image, retaining rollback protection.

## GATT wire contract

All characteristics use service UUID
`8e8b0001-7a1c-4b9e-9a2f-1d6e0b9c1a01`. The abbreviated UUIDs below retain the
same `-7a1c-4b9e-9a2f-1d6e0b9c1a01` suffix.

| Characteristic | UUID | Properties | Payload |
|---|---|---|---|
| OTA control | `8e8b0010-…` | Write | framed command; first byte is opcode |
| OTA data | `8e8b0011-…` | Write / Write Without Response | raw firmware bytes, strictly in order |
| OTA status | `8e8b0013-…` | Read / Notify | `state(1) + received(4 LE) + error(1)` |

### Control frames

| Opcode | Exact bytes | Meaning |
|---|---|---|
| `0x01` BEGIN | `0x01 + size(4 LE) + crc32(4 LE)` | initialise slot-1 stream and expected values |
| `0x03` END | `0x03` | verify size and CRC, request test upgrade, reboot |
| `0x04` ABORT | `0x04` | discard the session and remain on the current image |

### Status state byte

| Value | Meaning |
|---|---|
| `0x00` | idle |
| `0x01` | receiving |
| `0x02` | done (verified, rebooting) |
| `0x10` | begin error |
| `0x11` | sequence error |
| `0x12` | flash write error |
| `0x13` | CRC error |
| `0x14` | size error |
| `0x15` | end/finalisation error |

The `error` byte is zero outside an error state and otherwise repeats the error
state value. `received` counts bytes accepted into the flash stream.

### CRC-32

CRC is IEEE 802.3 CRC-32: reflected polynomial `0xEDB88320`, initial value
`0xFFFFFFFF`, final XOR `0xFFFFFFFF`. This is byte-identical to `zlib.crc32` and
the value stored with a backend release.

## Transfer sequence

1. Connect to the node's connectable advertising set and discover the service.
2. Write BEGIN and confirm STATUS is receiving with zero bytes.
3. Stream the image in order to DATA. Writes with response provide natural flash
   flow control; Write Without Response is also part of the contract.
4. Write END. The node checks exact size and CRC, asks MCUboot for a test upgrade,
   reports done, and reboots after 1.5 seconds so the relay can observe status.
5. On cancellation write ABORT. On disconnect during reception, treat the
   session as failed and begin a fresh transfer later.

## Coexistence with measurement advertising

Zephyr runs two legacy advertising sets. The first remains the unchanged,
non-connectable scannable measurement beacon: its full 29-byte manufacturer
record is in the primary packet and its identity stays in the scan response.
The second set is connectable and advertises the OTA service. Thus HiveHub's
passive scanner keeps receiving the existing wire format while an OTA relay can
open GATT independently. While STATUS is receiving (or a verified reboot is
pending), the main loop skips sensor acquisition and avoids long sleep.

## Build flag

`HIVEINSIDE_OTA_ENABLED` defaults to `1` in the firmware and PlatformIO build.
Set it to `0` at compile time to omit the GATT service and OTA application code.

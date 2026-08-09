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
application to be linked with its MCUboot header. Only a `west --sysbuild` build
produces the signed release artifact `build/<app>/zephyr/zephyr.signed.bin`;
**that signed file**, including its MCUboot header, is the object uploaded to the
backend and whose CRC/size are sent in BEGIN.

The build also drops a byte-identical copy beside it under a name carrying the
firmware version and the image variant, for example:

```
hiveinside-nrf54lm20a-v0.4.5-bringup.signed.bin     # normal build, console on
hiveinside-nrf54lm20a-v0.4.5-lowpower.signed.bin    # docs/low-power.md profile
```

Upload that copy rather than `zephyr.signed.bin`. Every build configuration
writes the same `zephyr.signed.bin` filename, so once two of them leave their
build directories nothing distinguishes them — and serving the bring-up image to
a sealed hive fails silently, because it boots and advertises exactly like the
image it replaced. The variant suffix is derived from `CONFIG_SERIAL` in the
image that was actually built, not from how the build was requested. The version comes from
`HIVEINSIDE_FW_VERSION_MAJOR`/`_MINOR`/`_PATCH` in `src/hive_config.h` — the
same numbers the node advertises — so bumping them there moves the artifact
name and the on-air version together.

> A build without `--sysbuild` never emits a signed image or a bootable merged
> hex — it builds the application alone. Build and flash releases with
> `west --sysbuild` (see [`flashing.md`](flashing.md)).

## GATT protocol

All OTA characteristics live in the custom HiveInside service
`8e8b0001-7a1c-4b9e-9a2f-1d6e0b9c1a01`, implemented in
[`firmware-nrf54lm20a/src/ota.c`](../firmware-nrf54lm20a/src/ota.c). UUIDs and
framing must stay in sync with HiveScale `firmware/src/ble_sensor.cpp` (the
`HI_OTA_*` constants). These three characteristics are the **only** GATT
attributes the device exposes — there is no measurement characteristic; the
measurement is read from the advertisement (see
[`../firmware-nrf54lm20a/README.md`](../firmware-nrf54lm20a/README.md)).

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
which does not send BEGIN within six seconds is disconnected, a transfer that
goes `HIVE_OTA_STALL_TIMEOUT_MS` (30 s) without a single DATA write is
abandoned, and a short link supervision timeout releases a vanished central.
`ota_is_active()` gates the sensor loop while connected or transferring.

## Safety and lifecycle

DATA callbacks call `flash_img_buffered_write()` synchronously before returning,
so HiveHub's write-with-response is flash-level flow control. CRC accumulation
starts at literal zero and chains `crc32_ieee_update()`, matching
`zlib.crc32`. END checks the exact received size and CRC before marking slot 1
pending. It then schedules a reset 1.5 seconds later, leaving time to read DONE.
The new image is a MCUboot **test** upgrade. It confirms itself only after one
complete sensor-read cycle and a successful BLE measurement publication; a
failure before that point leaves the image unconfirmed so MCUboot can revert it
on the next reset. A test image does not revert merely because a peripheral is
absent: individual sensor drivers deliberately degrade to `n/a`, while successful
BLE publication is the health gate.

Every state that gates the sensor loop is bounded, which matters because a
gated node is off the air entirely: it takes no measurements, and because it is
connected it is not advertising either, so HiveHub sees nothing at all. The
watchdog cannot rescue it — the OTA path feeds the watchdog deliberately, since
a real upload takes minutes. The bounds are: six seconds connected without
BEGIN (arm timeout), thirty seconds receiving without a DATA write (stall
timeout), and 1.5 seconds in `done` before the reboot fires. A transfer that is
slow but still progressing is never cut short, and it cannot run forever either:
`received` is capped at the size declared in BEGIN, after which any further DATA
write is rejected.

This health gate proves that the image can boot, sample, and advertise; it cannot
prove that a future central can discover and complete another OTA session. A
regression isolated to the OTA GATT path can therefore still require SWD recovery.
Confirmation is attempted at most once per boot so a persistent trailer-write
failure cannot cause periodic flash writes indefinitely.

To bound how long an unhealthy image can run before it rolls back, a test image
arms a confirmation deadline at boot (`OTA_CONFIRM_DEADLINE_MS`, 120 s). If it has
not confirmed within that window — radio never came up, the main loop wedged in a
sensor driver, or the trailer write kept failing — it reboots so MCUboot reverts
to the previous image, rather than running unconfirmed until the next unrelated
reset (up to a full `MEASURE_INTERVAL_MS` away). The deadline is armed only when
MCUboot reports a real test swap (`BOOT_SWAP_TYPE_REVERT`); a directly
SWD-flashed image has no rollback target, is never armed, and cannot boot loop.

Only the three OTA characteristics are exposed. A filter accept list would be a
stronger connection guardrail, but requires a future HiveHub bonding change.

## Build

Only a sysbuild build produces a release artifact: `west build --sysbuild -b
xiao_nrf54lm20a/nrf54lm20a/cpuapp -d debug firmware-nrf54lm20a`. A build without
`--sysbuild` compile-checks the application and stops there. Release automation
must publish the generated **signed** application binary, never the raw
`zephyr.bin`.

## Troubleshooting: the relay runs but the version never changes

The most confusing OTA failure reports no error anywhere. The relay completes,
the backend shows the update queued or sent, and the node keeps advertising the
old version. Four different causes produce that identical picture, so work down
the list rather than guessing.

Watch the node's serial console across a relay attempt — build the normal
(console) image for this, not the [`low-power`](low-power.md) profile, which has
no output at all. A healthy transfer prints:

```
[OTA] central connected; waiting for BEGIN
[OTA] BEGIN size=<n> crc=0x<...>
[OTA] image verified; test upgrade requested
[OTA] rebooting into test image
... reboot ...
[HiveInside] nrf54lm20a fw <new version> | USB + HiveHub BLE beacon
[OTA] test image confirmed after first complete cycle
```

Where it stops tells you which of these you have:

0. **`BEGIN` then silence, and the node disappears from HiveHub entirely.**
   The relay opened a session and stopped writing. Until the stall timeout was
   added the node stayed gated indefinitely — no measurements, no advertising,
   for as long as the relay held the link. Recover by making the relay drop the
   connection (resetting HiveScale is enough) or by power-cycling the node;
   nothing is damaged and the node stays on its old image. Firmware carrying the
   stall timeout abandons the transfer itself after thirty seconds and prints
   `[OTA] no data for 30000 ms; abandoning transfer`.

1. **Nothing at all.** The relay never opened a BLE connection. The image never
   reached the node, so nothing on the node can report it. This is a relay-side
   or pairing problem — check that the node's identity address matches the slot
   the relay is targeting.

2. **`connected` then `connection not armed; disconnecting`.** The central
   connected but never sent BEGIN within six seconds. The relay reached the node
   and gave up, usually because the release row or the download was missing on
   the backend side.

3. **`BEGIN` then an error state.** The node rejected the transfer and set an
   error in the STATUS characteristic (`0x10`–`0x15`, table above). If the
   backend does not surface STATUS, this looks like silence from the outside.
   `0x13` CRC and `0x14` size mean the bytes that arrived are not the bytes the
   release row describes.

4. **The full happy trace, then the old version comes back.** The image
   installed and MCUboot reverted it, or it never really differed:

   * **Wrong artifact.** Uploading `zephyr.bin` instead of the signed image
     gives a payload MCUboot cannot validate. The node reports `done` and
     reboots, MCUboot rejects the slot, and the old image runs again. Upload the
     version-stamped `…signed.bin` described above.
   * **Rollback.** A test image confirms only after one complete sensor cycle
     *and* a successful BLE publication. If it boots but cannot advertise, it
     never confirms and reverts within `OTA_CONFIRM_DEADLINE_MS` (two minutes).
     The banner line appears with the new version, then the node resets back.
   * **The version was never bumped in the firmware.** The number the backend
     sees comes from `HIVEINSIDE_FW_VERSION_MAJOR`/`_MINOR`/`_PATCH` in
     `src/hive_config.h`, not from the version typed into the upload form. If
     the binary was built without changing those, the OTA succeeded and the node
     is genuinely running the new image — it just advertises the old number. The
     banner line above is the giveaway: it shows the same version before and
     after. Bump the three numbers, rebuild, and re-upload.

## Production release and recovery checklist

The SDK's default MCUboot development key is suitable only for bring-up. It is
publicly known and therefore provides image formatting, **not production
authenticity**. Before deploying devices, provision a project-owned signing key,
configure both MCUboot and sysbuild signing to use it, keep the private key out of
the repository and build logs, and archive the matching public key and recovery
procedure. Changing the key later requires an SWD recovery image whose MCUboot
contains the new public key.

For every release:

1. Start from a clean `west build --pristine --sysbuild ...` build and retain the
   build manifest/configuration alongside the artifact.
2. Check the generated partition report and the signed image size against the
   actual secondary-slot capacity; do not rely only on the nominal 449 KiB figure.
3. Compute the backend size and CRC from the exact signed binary that is
   uploaded (never from `zephyr.bin` or `merged.hex`). Prefer the
   version-stamped copy — `hiveinside-<board>-v<version>-<variant>.signed.bin` —
   so the artifact on the backend still identifies itself after it is detached
   from the build directory, and check its `-bringup`/`-lowpower` suffix is the
   one you intended before publishing.
4. Flash `merged.hex` onto a representative board over SWD, perform a complete
   BLE OTA with the release artifact, and verify the version after reboot.
5. Run a rollback test with an intentionally non-confirming test application and
   keep an SWD mass-erase/reflash path available. BLE OTA cannot recover a device
   whose bootloader, signing key, partition layout, or radio/application startup
   is broken.

## Implementation

`firmware-nrf54lm20a/src/ota.c` streams into the board's MCUboot secondary slot,
validates size and CRC, requests a test swap, and self-confirms after a healthy
boot. There is no placeholder or alternative OTA implementation in this
repository.

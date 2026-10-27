# Security

## Reporting a vulnerability

Please report security problems privately through GitHub's
**Security → Report a vulnerability** form on this repository rather than in a
public issue. Include the firmware version (it is in the node's scan response
and in the release asset names), what you observed, and how to reproduce it.
You will get an answer as soon as the maintainer can look at it; this is a
one-person hobby project, so please allow a little time.

## Security model

What the firmware does and does not protect, so a report can be measured
against it:

* **Firmware authenticity is the trust boundary.** MCUboot boots and swaps in
  only images signed with the project key. Release CI refuses to build without
  that key and fails unless the bootloader embeds it and no MCUboot development
  key (`tools/release/check_bootloader_key.py`). Factory images **up to v0.6**
  trust the public development key and must be re-provisioned over SWD
  ([docs/flashing.md](docs/flashing.md#releases-up-to-v06-trust-the-public-development-key)).
* **Downgrades are refused** by the OTA service: an image whose signed version
  is older than the running one is rejected at END.
* **The OTA service accepts any central.** Anyone in radio range can start a
  transfer, which costs battery, slot writes and time off the air. That is
  bounded — arm, stall and whole-session timeouts, and a cooldown after
  repeated failures ([docs/ota-over-ble.md](docs/ota-over-ble.md)) — but not
  prevented. Restricting OTA to a bonded HiveHub needs HiveHub support and is
  planned.
* **Audio needs an HMAC-authenticated START, but is not encrypted.** The
  pre-shared key decides who may start a recording; the PCM itself crosses the
  air in clear unless the central encrypts the link. Recordings near a hive can
  contain speech. Treat audio as readable by anyone listening nearby until
  encrypted, bonded links are required (planned together with OTA bonding);
  [docs/audio-over-ble.md](docs/audio-over-ble.md) has the details.
* **The measurement beacon is public by design.** Its readings are broadcast
  unencrypted for passive scanning.
* **The throughput-spike build is never deployable.** It adds an
  unauthenticated service; release CI refuses to package it.

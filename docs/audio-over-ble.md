# Audio over BLE

*Firmware **0.6.2** or later. 0.6.0/0.6.1 stream the protocol below correctly but
capture only the first few tens of milliseconds of real sound — see [One session
at a time](#one-session-at-a-time).*

HiveInside exposes an authenticated, on-request microphone stream to HiveHub:

```
MSM261 PDM mic -> nRF PDM PCM16 -> 32 KiB ring -> BLE notifications -> HiveHub
                         32 kB/s                 measured 137 kB/s
```

The stream remains raw 16 kHz, 16-bit little-endian mono PCM. The measured 15 ms,
2M-PHY link carries 137 kB/s, over four times the 32 kB/s capture rate, so a
codec would add failure state without fitting a constrained pipe. Streaming uses
about 32 KiB rather than the 320 KiB or more needed for a useful whole clip.

## GATT service

All UUIDs end in `-7a1c-4b9e-9a2f-1d6e0b9c1a01`.

| Characteristic | UUID prefix | Properties | Value |
|---|---|---|---|
| Service | `8e8b0002` | primary | — |
| CTRL | `8e8b0020` | write | commands below |
| DATA | `8e8b0021` | notify | sequenced PCM packets |
| STATUS | `8e8b0023` | read, notify | 30-byte session record |

Subscribe to DATA before START. A DATA notification is `seq(2 LE), flags(1),
reserved(1), PCM16`. The PCM portion is `min(MTU-3-4, 240)` bytes, rounded down
to an even length. Sequence numbers are free-running unsigned 16-bit values.
Flag bit 0 marks the final, header-only packet. Bit 1 says whole audio blocks
were dropped immediately before this packet; do not splice across that hole.

CTRL commands are:

| Opcode | Exact bytes | Meaning |
|---|---|---|
| `0x01` START | `op, duration_ds(2 LE), rate_code, format, gain_db, hmac(8)` | authenticate and capture |
| `0x02` STOP | `op` | end the current stream |

`duration_ds=0` means live/open-ended. Every session still stops at 60 seconds.
The only rate code is 0 (16 kHz); the only implemented format is 0 (PCM16 LE
mono). Format 1 is reserved for ADPCM and returns unsupported-format. Gain is a
signed byte clamped to -20..+20 dB, applied before transmission with saturation.

STATUS is always exactly 30 bytes:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 1 | state: IDLE `00`, ARMED `01`, STREAMING `02`, DONE `03` |
| 1 | 1 | error |
| 2 | 8 | current authentication nonce |
| 10 | 4 | transmitted PCM bytes, LE |
| 14 | 4 | elapsed milliseconds, LE |
| 18 | 4 | bytes lost to ring overrun, LE |
| 22 | 4 | IEEE CRC-32 of transmitted, post-gain PCM, LE |
| 26 | 2 | sample rate (16000), LE |
| 28 | 1 | format (0) |
| 29 | 1 | clipped samples as integer percent |

Errors are bad parameters `10`, authentication `11`, busy `12`, microphone
`13`, DATA not subscribed `14`, notification stall `15`, and unsupported format
`16`. STATUS notifies on each state change and at completion.

## Authentication and client sequence

### Provisioning the key

The node and HiveHub share one random 32-byte key. They want it in different
notations — a C array here, a hex string on the hub — so generate it once and
print both forms rather than converting by hand:

```bash
KEY=$(openssl rand -hex 32)
echo "HiveHub  secrets.h:  #define HIVEINSIDE_AUDIO_PSK_HEX \"$KEY\""
echo "HiveInside audio_secret.h:"
echo "$KEY" | sed 's/../0x&, /g' | fold -sw 48 | sed 's/^/\t/;s/ $//'
```

Copy `src/audio_secret.example.h` to `src/audio_secret.h` (gitignored) and paste
the array rows in place of the zeros:

```c
#pragma once
#include <stdint.h>
static const uint8_t hive_audio_psk[32] = {
	0x40, 0xd8, 0x13, 0xad, 0x97, 0x4c, 0x15, 0x06,
	0x27, 0x03, 0xa0, 0x4e, 0x1f, 0xa3, 0x67, 0x25,
	0xf7, 0x09, 0x33, 0x30, 0xa8, 0xd3, 0xd1, 0x01,
	0x81, 0xfc, 0xdf, 0xe5, 0x59, 0x19, 0x42, 0x6b,
};
```

The hub takes the same key as the 64-character hex line in its
`firmware/include/secrets.h`; HiveHub's `docs/audio-recording.md` covers that
side, including the fact that its `FORCE_RESEED` flag is **not** involved — that
only re-seeds a claim code into NVS, while the audio key is compiled in and read
afresh at every session.

A missing or all-zero key fails closed: every START is refused with
authentication error `11` and, on a console build, a line saying the key is
absent. That is deliberate. The alternative is an in-hive microphone any BLE
device in range can switch on.

Flash both sides together. The key is checked per session, so in between the two
flashes audio fails authentication while the beacon and its measurements carry on
exactly as before — which is what makes a key mismatch look like a radio problem
rather than a configuration one.

### Client sequence

1. Connect, subscribe to DATA (and optionally STATUS), then read STATUS.
2. Take its fresh eight-byte nonce.
3. Compute `HMAC-SHA256(key, nonce || op || duration_ds_LE || rate_code ||
   format || gain_db)` and append the first eight digest bytes to START.
4. Write START and consume DATA until its final flag; verify byte count and
   CRC against final STATUS. Treat nonzero dropped bytes as an incomplete clip.

The node compares the tag without early exit. A failed attempt rotates the
nonce, so reread STATUS before retrying. The nonce also rotates on connection
and after every completed session.

## One session at a time

The node has **one** PDM controller and **one** switchable sensor rail (LDO1,
plus the P1.12 gate upstream of it), and two things want them: the five-minute
measurement cycle and an audio session. Nothing may run both at once.

Until 0.6.2 that was enforced only by a `link_is_busy()` check at the top of the
measurement loop, which is a decision, not a lock. A cycle that had already
passed that check went on to read the IMU and microphone and then call
`power_sensor_rail_disable()` — underneath a session that had started in the
meantime. The rail went away with the audio still streaming, and because the
"enabled" flag was a plain boolean, the session's own `power_sensor_rail_enable()`
had returned success without doing anything. The result was a full-length
recording of which only the first ~50 ms carried signal; the rest was the ±4 LSB
of an unpowered microphone. The transport reported it as perfectly clean,
because it was: every byte the node sent arrived.

0.6.2 replaces the check with two mechanisms:

* **A session gate** (`link_measurement_begin()` / `link_measurement_end()`, a
  binary semaphore in `link.c`). The measurement cycle holds it from before the
  rail is powered until after it is dropped, so `link_claim()` — and with it
  every START — reports **busy `12`** rather than cutting in. GATT callbacks take
  it with `K_NO_WAIT` and never block on a capture; the main loop waits on it and
  feeds the watchdog while it does, because a session can last a minute.
* **A reference-counted sensor rail.** `power_sensor_rail_enable()` /
  `_disable()` now count users under a mutex, so no holder can switch the rail
  out from under another. `power_init()` owns the boot-time reference and
  `main()` drops it once, which is why the counter starts balanced.

Session setup also moved out of the GATT write callback into the capture thread:
`ctrl_write()` now answers **ARMED**, and the thread powers the rail, waits for
it to settle and starts PDM before publishing **STREAMING**. A client that waits
for STREAMING is therefore waiting for a microphone that is genuinely running.

The visible symptom of the old bug is worth knowing, because it does not look
like a bug in the audio path at all: length, checksum and dropped-byte counters
all come back clean, and only listening reveals that the hive went silent a
twentieth of a second in.

## Bounded states and privacy

The microphone is off unless an authenticated START owns the exclusive link.
OTA and audio cannot overlap, and periodic sensing is held off for the whole of
a session by the gate above (and, conversely, a session cannot start while a
measurement cycle is in flight).
A connection that claims no service is disconnected after 10 seconds. Capture
ends at the requested duration, STOP, disconnect, or the absolute 60-second
cap. Five seconds without a notification completion is a stalled session. Each
end path stops PDM, removes sensor-rail power, publishes final STATUS where the
link remains, and releases ownership. Authentication is application-level
access control; audio is not encrypted unless the BLE connection itself is
secured by the client.

## Troubleshooting

* **Error 14:** subscribe to DATA before START; enabling STATUS alone is not enough.
* **Error 11:** provision a nonzero key on BOTH sides (see [Provisioning the
  key](#provisioning-the-key) — the commonest cause is flashing one device and
  not the other), reread the rotated nonce, and ensure the signed gain byte and
  little-endian duration are included exactly once.
* **Error 16:** request format 0; ADPCM is only a reserved wire value.
* **Short packets:** negotiate ATT MTU 247; smaller MTUs are valid and reduce PCM
  per notification while preserving sample boundaries.
* **Holes or CRC mismatch:** inspect sequence numbers, DATA flag bit 1, final
  `dropped_bytes`, and CRC. Nonzero drops mean the recording is not clean.
* **Error 12 (busy):** a measurement cycle holds the session gate. Cycles are
  short; retry. A `12` that never clears means something claimed the link and
  did not release it — reconnecting rearms the 10-second window.
* **Audio that starts and then goes silent:** the node is running 0.6.0 or
  0.6.1. Update to 0.6.2 — see [One session at a time](#one-session-at-a-time).
  The counters cannot show this: the bytes that arrived are exactly the bytes
  that were sent, they just contain no sound.
* **Error 15:** verify the central negotiated notifications, 15 ms interval,
  251-byte data length and 2M PHY and continues acknowledging controller TX.
* **Disconnect near startup:** complete STATUS/nonce/START within the 10-second
  arm window.

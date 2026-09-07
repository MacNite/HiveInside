# Audio over BLE

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

Provision the same random 32-byte key on node and HiveHub. Copy
`src/audio_secret.example.h` to the gitignored `src/audio_secret.h` and replace
the zeros. A missing or all-zero key fails closed.

1. Connect, subscribe to DATA (and optionally STATUS), then read STATUS.
2. Take its fresh eight-byte nonce.
3. Compute `HMAC-SHA256(key, nonce || op || duration_ds_LE || rate_code ||
   format || gain_db)` and append the first eight digest bytes to START.
4. Write START and consume DATA until its final flag; verify byte count and
   CRC against final STATUS. Treat nonzero dropped bytes as an incomplete clip.

The node compares the tag without early exit. A failed attempt rotates the
nonce, so reread STATUS before retrying. The nonce also rotates on connection
and after every completed session.

## Bounded states and privacy

The microphone is off unless an authenticated START owns the exclusive link.
OTA and audio cannot overlap, and periodic sensing pauses for any connection.
A connection that claims no service is disconnected after 10 seconds. Capture
ends at the requested duration, STOP, disconnect, or the absolute 60-second
cap. Five seconds without a notification completion is a stalled session. Each
end path stops PDM, removes sensor-rail power, publishes final STATUS where the
link remains, and releases ownership. Authentication is application-level
access control; audio is not encrypted unless the BLE connection itself is
secured by the client.

## Troubleshooting

* **Error 14:** subscribe to DATA before START; enabling STATUS alone is not enough.
* **Error 11:** provision a nonzero key, reread the rotated nonce, and ensure the
  signed gain byte and little-endian duration are included exactly once.
* **Error 16:** request format 0; ADPCM is only a reserved wire value.
* **Short packets:** negotiate ATT MTU 247; smaller MTUs are valid and reduce PCM
  per notification while preserving sample boundaries.
* **Holes or CRC mismatch:** inspect sequence numbers, DATA flag bit 1, final
  `dropped_bytes`, and CRC. Nonzero drops mean the recording is not clean.
* **Error 15:** verify the central negotiated notifications, 15 ms interval,
  251-byte data length and 2M PHY and continues acknowledging controller TX.
* **Disconnect near startup:** complete STATUS/nonce/START within the 10-second
  arm window.

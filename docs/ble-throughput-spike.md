# BLE notification throughput — measurement spike

**Temporary — and now answered.** Everything described here exists to answer one
question for [HiveInside issue #71](https://github.com/MacNite/HiveInside/issues/71)
(record an audio sample on request).

> ## The answer: 137 kB/s
>
> Measured node → XIAO ESP32-C6, 15 ms connection interval, 2M PHY, 251-byte
> PDUs. That is over four times the 32 kB/s a 16 kHz PCM16 microphone produces,
> and roughly a hundred times the OTA relay's ~1.4 kB/s — so the audio feature
> ships **raw PCM16, streamed**, with no codec and no whole-clip buffering. See
> [`audio-over-ble.md`](audio-over-ble.md).
>
> The thresholds table below was written before the run and is left as it was;
> the result cleared its top row by a factor of seventeen. Nothing here needs to
> be re-run, and the spike is now safe to delete — see [Removing it](#removing-it).

The rest of this page is kept as the record of how the number was obtained.

## The question

The audio feature has to move a recording from the node to the HiveHub relay:
peripheral → central, out of RAM, as GATT **notifications**. Nothing in this
firmware does that today, so nothing measures it either.

The only throughput number the project has is the OTA relay's, and on a real
deployment that came out at **≈1–1.5 kB/s** (172 kB image in 120–160 s). If that
were the ceiling for audio, streaming would be dead on arrival — ten seconds of
16 kHz IMA-ADPCM is 80 kB, which would take a minute to ship.

But the OTA path is a poor proxy for an audio stream, in three ways that all
push the same direction:

1. **Wrong direction, wrong primitive.** `gatt_ota.cpp` writes each chunk with
   `response=true` and blocks on the ATT response, so a 244-byte chunk costs at
   least two connection intervals — a hard ceiling near 3 kB/s at the 30–50 ms
   interval `ota.c` requests. Notifications have no per-packet round trip, and
   Zephyr can queue several into one connection event.
2. **Flash writes inside the ATT callback.** `data_write()` calls
   `flash_img_buffered_write()` synchronously before it returns the response —
   deliberately, because that *is* the OTA flow control. On this SoC each
   512-byte flush executes inside a radio timeslot that pre-empts connection
   events (see the `CONFIG_NRF_RRAM_WRITE_BUFFER_SIZE` discussion in
   `prj.conf`). An audio stream touches no flash at all.
3. **WiFi and BLE at once.** The relay holds a TLS download open while writing
   GATT, on one shared 2.4 GHz radio.

So the OTA figure largely measures RRAM write scheduling, not the radio. This
spike measures the radio.

## What it adds

| File | What it is |
|---|---|
| `firmware-nrf54lm20a/src/throughput.c/.h` | GATT service that blasts a counter pattern and logs the negotiated link parameters |
| `firmware-nrf54lm20a/throughput-spike.conf` | Kconfig fragment that compiles it in |
| `tools/throughput/ble_throughput.py` | laptop client (bleak) — upper bound |
| HiveHub `tools/ble-throughput/` | ESP32 client — **the number that decides the design** |

A build that does not apply the fragment is byte-identical with these files
present: `ENABLE_THROUGHPUT_SPIKE` defaults to 0 in `src/hive_config.h` and the
whole module compiles away.

## Protocol

Same UUID family as the OTA service, in the `0x8e8b00fx` range so it cannot
collide with a future audio service.

| Characteristic | UUID | |
|---|---|---|
| CTRL | `8e8b00f1-7a1c-4b9e-9a2f-1d6e0b9c1a01` | write |
| DATA | `8e8b00f2-…` | notify |
| STATUS | `8e8b00f3-…` | read / notify |

```
CTRL   0x01 duration_s payload_len [interval_units]   start a run
       0x02                                           stop early
DATA   seq(4 LE) + filler, payload_len bytes total
STATUS state(1) bytes(4 LE) ms(4 LE) notifs(4 LE) payload_len(1) mtu(2 LE)
```

`interval_units` is in the BLE 1.25 ms unit (12 = 15 ms, 24 = 30 ms) and is
applied by the **node**, via `bt_conn_le_param_update()` — the direction a real
audio session would ask in, and it means the interval can be swept from the
client without a reflash.

The 32-bit sequence number at the head of every notification is what separates
"the link is slow" from "the link is dropping packets": the node reports what it
handed to its controller, the client reports what arrived, and a gap between the
two is loss, not throughput.

## Running it

### 1. Build and flash the spike image

Layer the fragment on `prj.conf` as an **extra** Kconfig fragment — never a base
one (`docs/vscode-build.md` explains why that distinction bites):

```sh
west build --sysbuild -b xiao_nrf54lm20a/nrf54lm20a/cpuapp -d spike \
    firmware-nrf54lm20a -- -DEXTRA_CONF_FILE=throughput-spike.conf
west flash -d spike
```

In VS Code, add a third build configuration named `spike` with
`throughput-spike.conf` in **Extra Kconfig fragments**, exactly as the
`lowpower` configuration is set up.

The console will print, at boot:

```
[TP] BLE throughput spike compiled in — measurement build, NOT for deployment
```

If that line is missing, the fragment did not take and everything below will
measure a device that has no throughput service.

### 2. Laptop first (cheap sanity check)

```sh
pip install bleak
tools/throughput/ble_throughput.py --duration 15 --payload 244 --interval 12
```

This is an **upper bound**: a desktop Bluetooth stack is not the relay's
controller. Its value is diagnostic — if the laptop also sees 1–2 kB/s the limit
is on the node and no amount of work on the hub will help; if the laptop sees
30 kB/s and the ESP32 sees 3, the limit is the relay.

### 3. The ESP32 client (the real answer)

In the HiveHub repository, `tools/ble-throughput/` is a standalone PlatformIO
project — deliberately separate from `firmware/` so it can never be linked into
a hub image. Flash it to a **spare** ESP32:

```sh
pio run -e esp32dev -t upload -t monitor
```

Run it on both boards. The classic ESP32 is BLE 4.2 with **no 2M PHY**; the
ESP32-C6 has it, so the two can legitimately differ by roughly a factor of two.

### 4. Sweep the two knobs that matter

- **Connection interval** — `--interval` / `-DTP_INTERVAL_UNITS`. Try 12 (15 ms)
  against 24 (30 ms).
- **Controller TX buffers** — `CONFIG_BT_BUF_ACL_TX_COUNT` in
  `throughput-spike.conf` (3 is the `prj.conf` value, the fragment ships 6).
  `throughput.c` takes its in-flight credit count straight from this symbol, so
  changing it sweeps notifications-per-connection-event. Raising it costs about
  251 bytes of RAM per buffer — watch the build's RAM report.

### 5. Repeat with WiFi busy

A pass-through streaming design has BLE and WiFi active at the same time. Set
`-DTP_WIFI_LOAD=1` in the ESP32 project (plus SSID, password and a URL to pull)
and re-run. A large gap between the idle and loaded numbers is an argument for
buffering the clip to SD and uploading **after** disconnecting, rather than
streaming straight through.

## Reading the result

Both clients print a verdict, against these thresholds:

| Sustained | What it means for #71 |
|---|---|
| ≥ 8 kB/s | real-time streaming of 16 kHz ADPCM is viable |
| 4–8 kB/s | stream at 8 kHz ADPCM, or buffer the clip at 16 kHz |
| < 4 kB/s | buffer the whole clip on the node; streaming is out |

Also check the console lines the spike logs on connect:

```
[TP] connected: interval=… latency=… timeout=…
[TP] PHY now tx=2 rx=2                    1 = 1M, 2 = 2M, 4 = Coded
[TP] data length now tx=251 B/…           27 here means DLE did not take
```

`tx_max_len = 27` would mean every notification is being fragmented across
link-layer packets whatever `BT_CTLR_DATA_LENGTH_MAX` says — worth knowing
before blaming the design.

## Safety notes

- **Never release a spike image.** The extra service lets anything in radio
  range make the node transmit continuously, which is both a battery and a
  privacy problem. It is a bench build, on the bench, with a console.
- The measurement holds the connection for its whole duration, and a connected
  node neither advertises nor samples (`ota_is_active()` gates the sensor loop
  in `main.c`), so the hub will see nothing from that hive while a run is going.
  That is expected, and it is also a useful preview of the off-air cost a real
  recording will have.
- `ota_release_arm_timeout()` exists only for this spike: `ota.c` otherwise drops
  any central that has not sent BEGIN within six seconds, which would cut every
  run short. It is compiled out of normal builds.

## Removing it

The question is answered, so this is now a live to-do rather than a
someday-note: delete `src/throughput.c/.h`,
`throughput-spike.conf`, `tools/throughput/`, this document, the
`ENABLE_THROUGHPUT_SPIKE` block in `src/hive_config.h`, the `src/throughput.c`
line in `CMakeLists.txt`, the `ota_release_arm_timeout()` pair in
`src/ota.c`/`src/ota.h`, and HiveHub's `tools/ble-throughput/`.

Nothing is compiled into a normal build in the meantime — the spike needs
`throughput-spike.conf` on the build command line — so leaving it in place is
harmless, just untidy.

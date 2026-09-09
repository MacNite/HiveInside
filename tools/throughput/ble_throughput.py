#!/usr/bin/env python3
"""Measure HiveInside's BLE notification throughput from a laptop (issue #71,
phase 0e).

This drives the temporary throughput service in firmware-nrf54lm20a/src/
throughput.c: it subscribes to DATA, asks the node to blast for N seconds, and
reports what actually arrived. A desktop Bluetooth stack is not the ESP32 relay,
so treat the number here as an UPPER BOUND — what the node's radio and firmware
can do with a capable central. The number that decides the design is the one
from the ESP32 client in HiveHub's tools/ble-throughput/.

Its real value is separating two failure modes cheaply: if the laptop also sees
1-2 kB/s, the limit is on the node and no amount of work on the relay will help;
if the laptop sees 30 kB/s and the ESP32 sees 3, the limit is the relay's
controller or WiFi coexistence.

Usage:
    pip install bleak
    ./ble_throughput.py                       # find the first HiveInside node
    ./ble_throughput.py --address AA:BB:..    # or name one
    ./ble_throughput.py --duration 20 --payload 244 --interval 12

Requires the node to be running an image built with throughput-spike.conf.
"""

import argparse
import asyncio
import struct
import sys
import time

from bleak import BleakClient, BleakScanner

BASE = "-7a1c-4b9e-9a2f-1d6e0b9c1a01"
SVC_UUID = "8e8b00f0" + BASE
CTRL_UUID = "8e8b00f1" + BASE
DATA_UUID = "8e8b00f2" + BASE
STATUS_UUID = "8e8b00f3" + BASE

OP_START = 0x01
OP_STOP = 0x02

STATE_NAMES = {
    0x00: "idle",
    0x01: "running",
    0x02: "done",
    0x10: "error: bad parameters",
    0x11: "error: no link",
    0x12: "error: client not subscribed",
}


class Counter:
    """Bytes and sequence numbers seen, so loss is distinguishable from slowness."""

    def __init__(self) -> None:
        self.packets = 0
        self.total_bytes = 0
        self.gaps = 0
        self.missing = 0
        self.first_at: float | None = None
        self.last_at: float | None = None
        self._next_seq: int | None = None

    def feed(self, payload: bytes) -> None:
        now = time.monotonic()
        if self.first_at is None:
            self.first_at = now
        self.last_at = now
        self.packets += 1
        self.total_bytes += len(payload)

        if len(payload) >= 4:
            seq = struct.unpack_from("<I", payload)[0]
            if self._next_seq is not None and seq != self._next_seq:
                # A jump forward is loss; anything else is reordering, which BLE
                # does not do on a single connection, so treat it as loss too.
                self.gaps += 1
                self.missing += max(0, seq - self._next_seq)
            self._next_seq = seq + 1

    @property
    def elapsed(self) -> float:
        if self.first_at is None or self.last_at is None:
            return 0.0
        return self.last_at - self.first_at


async def find_device(name_prefix: str):
    print(f"scanning for a device whose name starts with {name_prefix!r} ...")
    devices = await BleakScanner.discover(timeout=8.0)
    for d in devices:
        if d.name and d.name.startswith(name_prefix):
            print(f"found {d.name} at {d.address}")
            return d.address
    print("no matching device found", file=sys.stderr)
    return None


async def run(args) -> int:
    address = args.address or await find_device(args.name)
    if not address:
        return 1

    counter = Counter()

    def on_notify(_handle, data: bytearray) -> None:
        counter.feed(bytes(data))

    async with BleakClient(address) as client:
        print(f"connected; negotiated MTU {client.mtu_size}")
        services = client.services
        if services.get_service(SVC_UUID) is None:
            print(
                "the throughput service is not present — is the node running an "
                "image built with throughput-spike.conf?",
                file=sys.stderr,
            )
            return 2

        await client.start_notify(DATA_UUID, on_notify)

        cmd = bytes([OP_START, args.duration, args.payload, args.interval])
        await client.write_gatt_char(CTRL_UUID, cmd, response=True)
        print(
            f"running for {args.duration}s "
            f"(payload {args.payload} B, interval {args.interval or 'unchanged'})"
        )

        # Poll once a second so a run that dies early is visible while it happens
        # rather than only in the totals.
        last_bytes = 0
        for _ in range(args.duration + 3):
            await asyncio.sleep(1.0)
            delta = counter.total_bytes - last_bytes
            last_bytes = counter.total_bytes
            print(f"  {delta / 1024:7.1f} kB/s   total {counter.total_bytes / 1024:8.1f} kB")

        try:
            await client.write_gatt_char(CTRL_UUID, bytes([OP_STOP]), response=True)
        except Exception:
            pass
        await client.stop_notify(DATA_UUID)

        status = await client.read_gatt_char(STATUS_UUID)

    report(args, counter, bytes(status))
    return 0


def report(args, counter: Counter, status: bytes) -> None:
    print()
    print("── received by this client ──────────────────────────────")
    rate = counter.total_bytes / counter.elapsed if counter.elapsed else 0.0
    print(f"  bytes      : {counter.total_bytes}")
    print(f"  packets    : {counter.packets}")
    print(f"  elapsed    : {counter.elapsed:.2f} s")
    print(f"  throughput : {rate / 1024:.1f} kB/s")
    if counter.gaps:
        print(f"  LOSS       : {counter.gaps} gap(s), ~{counter.missing} packet(s) missing")

    if len(status) >= 16:
        state, sent, ms, notifs, payload, mtu = struct.unpack("<BIIIBH", status[:16])
        dev_rate = (sent / (ms / 1000.0)) if ms else 0.0
        print("── reported by the node ─────────────────────────────────")
        print(f"  state      : {STATE_NAMES.get(state, hex(state))}")
        print(f"  bytes      : {sent}  ({notifs} notifications of {payload} B)")
        print(f"  elapsed    : {ms} ms")
        print(f"  throughput : {dev_rate / 1024:.1f} kB/s")
        print(f"  MTU        : {mtu}")
        if sent and counter.total_bytes < sent:
            lost = sent - counter.total_bytes
            print(f"  NOTE: {lost} B handed to the controller never reached this client")

    print()
    print("── what this means for issue #71 ────────────────────────")
    rate_kb = rate / 1024
    if rate_kb >= 8:
        print("  >= 8 kB/s: real-time streaming of 16 kHz ADPCM is viable.")
    elif rate_kb >= 4:
        print("  4-8 kB/s: stream at 8 kHz ADPCM, or buffer the clip at 16 kHz.")
    else:
        print("  < 4 kB/s: buffer the whole clip on the node; streaming is not viable.")
    print("  Remember this is a desktop central — the ESP32 number decides.")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--address", help="BLE address of the node (skips the scan)")
    ap.add_argument("--name", default="HiveInside", help="name prefix to scan for")
    ap.add_argument("--duration", type=int, default=15, help="seconds to blast (1-60)")
    ap.add_argument("--payload", type=int, default=244,
                    help="notification payload in bytes (20-244)")
    ap.add_argument("--interval", type=int, default=0,
                    help="connection interval in 1.25 ms units; 0 leaves it alone "
                         "(12 = 15 ms, 24 = 30 ms)")
    args = ap.parse_args()

    if not 1 <= args.duration <= 60:
        ap.error("--duration must be between 1 and 60")
    if not 20 <= args.payload <= 244:
        ap.error("--payload must be between 20 and 244")

    try:
        return asyncio.run(run(args))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())

# HiveInside — XIAO nRF54LM20A Sense firmware (nRF Connect SDK / Zephyr)

The next-generation HiveInside node, moving from the XIAO ESP32-C6 prototype
(`firmware-esp32-c6/`) to the **Seeed XIAO nRF54LM20A Sense** for much lower
power and a single integrated board (on-board IMU + PDM mic).

- **MCU:** nRF54LM20A — Cortex-M33 @ 128 MHz, FPU + DSP, 2 MB RRAM, 512 KB RAM,
  Bluetooth LE 6.0. Plus a RISC-V (FLPR) coprocessor we can ignore for now.
- **On-board IMU:** ST **LSM6DS3TR-C** (6-axis) — used for the swarm-band
  vibration FFT (the ~8–30 Hz pre-swarm signal from Ramsey et al. 2020).
- **On-board mic:** **MSM261DGT006** PDM MEMS microphone — acoustic FFT.
  The nRF54LM20A has a **hardware PDM peripheral** that decodes PDM → PCM in
  hardware, so we drop the software sinc decimator the ESP32-C6 needed.
- **Later:** external **SHT40** (temp/humidity) over I²C.

## Why nRF Connect SDK (and yes, it's an RTOS)

nRF Connect SDK (NCS) **is** Zephyr RTOS + Nordic's BLE stack and drivers —
there is no supported bare-metal path for the nRF54L family, and you don't want
one. For a low-power beacon the RTOS *helps*:

- The kernel is **tickless** and the idle thread automatically drops the SoC
  into low-power modes whenever no thread is runnable. Between measurements the
  chip sits in System ON idle with RAM retention at single-digit µA — without
  you hand-rolling sleep logic like the ESP32-C6 `enterDeepSleep()`.
- Battery, you don't fight it: a beacon can live almost entirely in `main()`
  plus a `k_timer`, so the RTOS stays out of the way while still giving you the
  Nordic BLE controller, the PDM/DMIC driver, the LSM6DS3TR-C sensor driver and
  CMSIS-DSP for the FFT — all as drop-in modules.

So: **use NCS (Zephyr). Don't disable the RTOS.** Low power comes from short
wake windows and the built-in idle, not from going bare-metal.

---

## Step 0 — Install the toolchain + SDK (one time)

You said the Nordic VS Code plugins are already installed. Finish the setup:

1. Open **nRF Connect for VS Code** → the **nRF Connect** sidebar icon.
2. **Install Toolchain** — pick the version that matches the SDK below (the
   extension pairs them). This is the compiler + `west` + native tools.
3. **Install SDK** — install a recent **nRF Connect SDK** release that has
   nRF54LM20A production support and that the Seeed board files target (the
   current Seeed `platform-seeedboards` samples track the recent NCS line; pick
   the newest offered unless their wiki pins an exact version).
4. **Add the Seeed board files** (the XIAO nRF54LM20A board isn't in your SDK tree
   until you point Zephyr at Seeed's definitions):
   ```bash
   git clone https://github.com/Seeed-Studio/platform-seeedboards.git
   ```
   Then in VS Code: **Settings → search "nRF Connect: Board Roots" → Edit in
   settings.json** and add the path to `platform-seeedboards/zephyr` to the
   `boardRoots` array. Reload the window.
   *(Check the Seeed wiki "Getting Started" page for the exact current path —
   board file layouts move between SDK versions.)*

## Step 1 — Blinky (this folder)

Confirms the toolchain, board target and flashing path before any sensors.

**Build (VS Code):** nRF Connect sidebar → **Add build configuration** → select
board target **`xiao_nrf54lm20a/nrf54lm20a/cpuapp`** → **Build Configuration**.

**Build (CLI):**
```bash
west build -b xiao_nrf54lm20a/nrf54lm20a/cpuapp firmware-nrf54lm20a
```

**Flash:** the XIAO has no on-board debugger, so either:
- **UF2 bootloader (no extra hardware):** double-tap **RESET** to mount the
  board as a USB drive, then drag `build/zephyr/zephyr.uf2` onto it; **or**
- **SWD debugger** (J-Link / other) on the XIAO's SWD pads → VS Code **Flash**
  button, or `west flash`.

> Confirm the flashing method on the Seeed wiki for your board revision before
> relying on it — some XIAO revisions ship with the UF2 bootloader, some expect
> an external debugger.

✅ **Success = the on-board user LED blinks at 1 Hz** and `HiveInside nRF54LM20A
blinky up …` prints on the serial console (115200 baud over the USB CDC-ACM).

---

## Roadmap to the real firmware (after blinky works)

Planned build order — each step is independently testable on the bench:

1. **BLE beacon shell.** Non-connectable **extended advertising** with a
   manufacturer-data payload (dummy values first). Verify in the nRF Connect
   mobile app. This is the low-power transport: no connection, radio on only for
   a short advertising burst each cycle.
2. **IMU (LSM6DS3TR-C).** Enable the Zephyr sensor driver from the Seeed
   devicetree node; capture a fixed buffer at a known ODR (~400 Hz) for the FFT.
3. **Mic (PDM/DMIC).** Use the Zephyr DMIC API on the nRF PDM peripheral —
   hardware PDM→PCM, 16 kHz. Short capture window (~100–250 ms) to keep power
   down; the mic is the biggest consumer.
4. **DSP features (CMSIS-DSP).** `arm_rfft_fast_f32` on the M33 for both
   domains. Compute the **same band energies as HiveScale** so values mean the
   same thing across the ecosystem (acc: swarm 8–30 / fanning 30–100 /
   activity 100–200 Hz; sound: sub-bass / hum / piping / stress / high — see
   `firmware-esp32-c6/include/config.h`).
5. **Pack + broadcast.** RMS sound, RMS acc, the FFT **band energies + peak
   bin** per domain, temp, humidity, battery — see payload note below.
6. **Power management.** `k_timer` wake cadence, short adv burst, idle between;
   tune the interval. Measure with a Power Profiler Kit if available.
7. **SHT40** over I²C (external, added last as you noted).
8. **Battery voltage** via SAADC — *needs schematic confirmation* of whether the
   XIAO nRF54LM20A Sense exposes a BAT+ divider net and an enable GPIO.

### ⚠️ Important design note: you can't beacon a *full* FFT

A legacy BLE advert is 31 bytes total; even BLE 5 **extended advertising** tops
out around 254 bytes. A full spectrum (hundreds of bins) neither fits nor is
worth the airtime/power. So the beacon carries **features, not raw FFT**:

- RMS sound, RMS acc (≈2 bytes each)
- per-band energies — the HiveScale bands above (≈1–2 bytes/band)
- peak frequency + magnitude per domain
- temp, humidity, battery

That's ~30–40 bytes — comfortable in one extended advert. The on-device FFT
still runs at full resolution; we just transmit the distilled result. If you
ever need the *raw* spectrum off the device, that's the point to add an optional
**connectable GATT** characteristic (like the ESP32-C6 build), accepting the
extra power cost of a connection — but the default stays pure beacon.

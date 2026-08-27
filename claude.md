# Project instructions for Claude Code

## Delivery policy — ask if data should be pushed to GitHub directly

This repository uses a "no remote writes" workflow. When you finish making
changes, do not publish them to GitHub automatically — always ask if it should
be pushed and a PR should be created.

You MUST NOT, under any circumstances unless I explicitly ask in that very
message:

* run `git push` (to any branch or remote);
* create, update, merge, or comment on pull requests;
* create or move remote branches or tags;
* push files through the GitHub API / MCP tools (`create_or_update_file`,
  `push_files`, `create_pull_request`, etc.);
* otherwise transmit repository contents to GitHub or any external service.

Working locally is fine: edit files, run builds/tests, and commit to the local
branch if it helps you organize work. Just never send anything to the remote.

## About this project

HiveInside is a battery-powered in-hive sensor node built on the Seeed **XIAO
nRF54LM20A Sense**. It measures climate, vibration and sound inside a beehive,
reduces vibration and audio to the FFT bands shared across the ecosystem, and
broadcasts each result as a 29-byte BLE manufacturer-data beacon that HiveHub
picks up with a passive scan — no connection, pairing window, or wake-sync
schedule. Sensors: SHT40 climate (external, I²C), on-board LSM6DS3TR-C IMU
(vibration, for ~20 Hz swarm prediction), on-board MSM261DGT006 PDM microphone,
and the nPM1300 PMIC fuel gauge. The firmware boots through MCUboot and accepts
firmware-over-BLE updates.

* `firmware-nrf54lm20a/` — the Zephyr application (`src/main.c` is the main
  source; one module per sensor plus `beacon`, `ota`, `fft`, `power`,
  `watchdog`).
* `west.yml` — west manifest, pinning the upstream Zephyr revision to build
  against.
* `enclosure/` — 3D-printable enclosure (STL/STEP/F3Z).
* `tools/flash/` — OpenOCD flashing scripts shipped with the release images
  (`.github/workflows/release.yml` builds and publishes those images).
* `website/` — the project page published to GitHub Pages.
* `docs/` — wiring, VS Code build, flashing, OTA-over-BLE, low-power profile,
  Home Assistant.

**`west --sysbuild` is the only supported build path.** The firmware boots
through MCUboot, so a bootable image is the bootloader *plus* the signed
application; a plain `west build` produces an application-only image that
leaves nothing at `0x0` and does not boot. Keep configuration single-sourced —
there is exactly one `prj.conf` and one `app.overlay`, both in
`firmware-nrf54lm20a/`, and CI fails if a second application root or a retired
build frontend (`platformio.ini`, a nested `zephyr/`, `parts/`) reappears.
Layer profiles through `EXTRA_CONF_FILE` / `EXTRA_DTC_OVERLAY_FILE`, never the
base variables, which replace those files instead of merging onto them.

## Always bump the firmware version

Any change that alters the built firmware image gets a version bump in the same
change — patch by default (`0.4.7` → `0.4.8`), minor for a new feature or a
changed BLE payload layout. Never leave two different builds carrying the same
version.

The version lives in exactly one place, `firmware-nrf54lm20a/src/hive_config.h`:

```c
#define HIVEINSIDE_FW_VERSION_MAJOR 0
#define HIVEINSIDE_FW_VERSION_MINOR 4
#define HIVEINSIDE_FW_VERSION_PATCH 7
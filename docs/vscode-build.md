# Building in VS Code / VSCodium (nRF Connect extension)

This is the complete build setup for HiveInside on the Seeed XIAO nRF54LM20A
Sense: two build configurations side by side — **`debug`**, the image with a
serial console, and **`lowpower`**, the deployment image built with the
[low-power profile](low-power.md). Both produce their own bootable
`merged.hex` and their own OTA payload, and you switch between them in the
sidebar. The build directory name *is* the configuration name the sidebar
shows, so the two names below are the only handle you have on them — keep them
as written.

For the command-line equivalent see [`flashing.md`](flashing.md); for what the
low-power profile actually changes and why, see [`low-power.md`](low-power.md).

## Prerequisites

* **nRF Connect for VS Code.** Published to the
  [Open VSX registry](https://open-vsx.org/extension/nordic-semiconductor/nrf-connect),
  so VSCodium can install it from its built-in marketplace. Nordic only tests
  Visual Studio Code and does not prioritise issues in other editors, but the
  extension does work in VSCodium.
* **An nRF Connect SDK install** (v3.3.1 is known to work here) and its matching
  toolchain, both selectable at the top of the build-configuration form.
* **The board definition.** `xiao_nrf54lm20a` is not in NCS's `sdk-zephyr`
  fork, so the NCS workspace needs it added out of tree — see the board-root
  section of [`flashing.md`](flashing.md). Once the board target appears in the
  dropdown, this is already sorted.

## Configuration 1 — `debug` (serial console)

**APPLICATIONS → `firmware-nrf54lm20a` → Add build configuration.**

| Field | Value |
|---|---|
| Board target | `xiao_nrf54lm20a/nrf54lm20a/cpuapp` |
| **Build directory name** | **`debug`** |
| Base configuration files (Kconfig fragments) | *(empty)* |
| Extra Kconfig fragments | *(empty)* |
| Base Devicetree overlays | *(empty)* |
| Extra Devicetree overlays | *(empty)* |
| Snippets | *(empty)* |
| Optimization level | Use project default |
| System build (sysbuild) | **Use sysbuild** |

Everything empty is correct. Zephyr picks up `prj.conf` and `app.overlay` on
its own, and the application's `CMakeLists.txt` selects the fixup overlay for
the active SDK (`ncs_fixups.overlay` here). This is the image to use for
anything you need to debug — hence the name: it is the only one that prints.

## Configuration 2 — `lowpower` (deployment)

**Add build configuration** a second time. Only three fields differ:

| Field | Value |
|---|---|
| Board target | `xiao_nrf54lm20a/nrf54lm20a/cpuapp` |
| **Build directory name** | **`lowpower`** |
| Base configuration files (Kconfig fragments) | *(empty)* |
| **Extra Kconfig fragments** | **`low-power.conf`** |
| Base Devicetree overlays | *(empty)* |
| **Extra Devicetree overlays** | **`low-power.overlay`** |
| Snippets | *(empty)* |
| System build (sysbuild) | **Use sysbuild** |

The build directory name is what keeps the two apart: each configuration owns
its own directory, so both images exist at once and the sidebar switches which
one **Build**, **Flash** and **Debug** act on.

### The one mistake this form invites

**Never put these files in a "Base" field.** The base fields are `CONF_FILE`
and `DTC_OVERLAY_FILE`, which *replace* `prj.conf` and `app.overlay`. The
"Extra" fields are `EXTRA_CONF_FILE` and `EXTRA_DTC_OVERLAY_FILE`, which merge
on top. The first field is labelled *"Base configuration files (Kconfig
fragments)"* — the parenthetical makes it read like the right choice, and it is
not.

Both mistakes are quiet in different ways:

* **`low-power.conf` in the base configuration field** replaces `prj.conf`, so
  the build has no Bluetooth, no MCUboot image manager and no sensor stack. This
  one at least fails loudly, while compiling `ota.c`:

  ```
  error: 'CONFIG_IMG_BLOCK_BUF_SIZE' undeclared here (not in a function)
  ```

* **`low-power.overlay` in the base devicetree field** replaces `app.overlay`,
  and **this one compiles cleanly and produces a broken image.** Lost with it:

  | Dropped from `app.overlay` | Consequence |
  |---|---|
  | `&pmic_i2c` pin correction (P1.18/P1.17) | nPM1300 unreachable; LDO1 stays at 1.8 V |
  | `&pmic` LDO1 at 3.3 V, `regulator-boot-on` | sensor rail under-volted |
  | `&pwm20` disabled, `&pdm20` enabled | PDM microphone never samples |
  | `&wdt31` enabled | no watchdog — a hang is permanent |

  In a deployment image there is no console to report any of it. The node boots,
  advertises, and reports `n/a` for sound.

## Verify before trusting a build

The extension prints the full `west build` command at the top of the build
output. Check all four:

1. The command line contains **`-DEXTRA_CONF_FILE`** and
   **`-DEXTRA_DTC_OVERLAY_FILE`** — no bare `-DCONF_FILE` or
   `-DDTC_OVERLAY_FILE`.
2. The application image merges **both** configurations, in this order:
   ```
   Merged configuration '.../prj.conf'
   Merged configuration '.../low-power.conf'
   ```
3. **All three** overlays are listed (the SDK-specific one is
   `ncs_fixups.overlay` in an nRF Connect SDK workspace):
   ```
   -- Found devicetree overlay: .../app.overlay
   -- Found devicetree overlay: .../ncs_fixups.overlay
   -- Found devicetree overlay: .../low-power.overlay
   ```
4. The `lowpower` image's `FLASH` figure is **visibly smaller** than the
   `debug` build's. The UART driver, console, `printk()` and the cbprintf
   floating-point formatter are all gone. Two near-identical sizes mean the
   fragment was not applied.

On the `debug` image, the console after flashing should show the watchdog
arming and the version you expect:

```
[HiveInside] nrf54lm20a fw 0.4.5 | USB + HiveHub BLE beacon
[WDT] armed: 60000 ms timeout, fed every 20000 ms
```

`[WDT] no enabled watchdog devicetree node` instead means `app.overlay` was
replaced — see the table above.

## Output artifacts

Each configuration writes to its own build directory:

```
debug/                              lowpower/
└── merged.hex                      └── merged.hex          ← SWD flashing
└── firmware-nrf54lm20a/zephyr/     └── firmware-nrf54lm20a/zephyr/
    ├── zephyr.signed.bin               ├── zephyr.signed.bin
    └── hiveinside-nrf54lm20a-          └── hiveinside-nrf54lm20a-
        v0.4.5-bringup.signed.bin           v0.4.5-lowpower.signed.bin
```

`merged.hex` is MCUboot plus the signed application and is what **Flash**
programs over SWD. The version-stamped `.signed.bin` is the OTA payload — upload
that rather than `zephyr.signed.bin`, because every configuration writes the
same `zephyr.signed.bin` filename and nothing distinguishes two of them once
they leave their build directories. See [`ota-over-ble.md`](ota-over-ble.md).

The `debug` configuration's payload is stamped **`-bringup`**, not `-debug`:
that suffix is derived from `CONFIG_SERIAL` in the image that was actually
built, so it describes the image rather than the configuration that requested
it, and it stays `-bringup` for any console-enabled build however it was
produced.

## Notes

* **Pristine builds.** After changing any field in the form, or after pulling
  changes that touch `prj.conf` or `app.overlay`, use **Pristine Build**. A warm
  build directory is exactly where Zephyr's caches mislead.
* **`Include debug thread information`** adds `-DCONFIG_DEBUG_THREAD_INFO=y` for
  debugger thread awareness. Harmless, and not part of the low-power profile —
  leave it on for `debug`, and off for `lowpower` if you want the smallest
  build.
* **Neither directory is `build/`.** `west` defaults to `build/`, so a
  command-line `west build`, `west flash` or `west debug` against a directory
  these configurations produced needs `-d debug` or `-d lowpower`. Without it
  `west` creates or reuses a third, unrelated build directory —
  [`flashing.md`](flashing.md) describes that plain command-line flow.
* **Browse vs typing.** The **Browse** button inserts absolute paths. Those work
  fine; plain relative filenames are resolved against the application directory
  and are easier to read.
* **Extra CMake arguments** stays empty. Passing
  `-DEXTRA_CONF_FILE=low-power.conf -DEXTRA_DTC_OVERLAY_FILE=low-power.overlay`
  by hand is exactly equivalent if you prefer it.
* **CI builds both configurations.** `.github/workflows/build.yml` runs the same
  `west --sysbuild` build for `debug` and `lowpower` against the Zephyr revision
  pinned in `west.yml`. It checks the MCUboot hex, signed application hex and
  payload with the expected variant suffix. Upstream Zephyr keeps the two flash
  domains separate; the nRF Connect SDK extension additionally presents their
  `merged.hex`. A green CI run means the fragments were actually applied — but
  it is not a substitute for flashing a deployment image before sealing a hive.

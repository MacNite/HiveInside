# Building in VS Code / VSCodium (nRF Connect extension)

This is the complete build setup for HiveInside on the Seeed XIAO nRF54LM20A
Sense: two build configurations side by side — the **bring-up** image with a
serial console, and the **deployment** image built with the
[low-power profile](low-power.md). Both produce their own bootable
`merged.hex` and their own OTA payload, and you switch between them in the
sidebar.

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

## Configuration 1 — bring-up (serial console)

**APPLICATIONS → `firmware-nrf54lm20a` → Add build configuration.**

| Field | Value |
|---|---|
| Board target | `xiao_nrf54lm20a/nrf54lm20a/cpuapp` |
| Build directory name | `build` |
| Base configuration files (Kconfig fragments) | *(empty)* |
| Extra Kconfig fragments | *(empty)* |
| Base Devicetree overlays | *(empty)* |
| Extra Devicetree overlays | *(empty)* |
| Snippets | *(empty)* |
| Optimization level | Use project default |
| System build (sysbuild) | **Use sysbuild** |

Everything empty is correct. Zephyr picks up `prj.conf` and `app.overlay` on
its own, and the application's `CMakeLists.txt` adds `ncs_fixups.overlay`. This
is the image to use for anything you need to debug: it is the only one that
prints.

## Configuration 2 — deployment (low power)

**Add build configuration** a second time. Only three fields differ:

| Field | Value |
|---|---|
| Board target | `xiao_nrf54lm20a/nrf54lm20a/cpuapp` |
| **Build directory name** | **`build_lowpower`** |
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
3. **All three** overlays are listed:
   ```
   -- Found devicetree overlay: .../app.overlay
   -- Found devicetree overlay: .../ncs_fixups.overlay
   -- Found devicetree overlay: .../low-power.overlay
   ```
4. The deployment image's `FLASH` figure is **visibly smaller** than the
   bring-up build's. The UART driver, console, `printk()` and the cbprintf
   floating-point formatter are all gone. Two near-identical sizes mean the
   fragment was not applied.

On the bring-up image, the console after flashing should show the watchdog
arming and the version you expect:

```
[HiveInside] nrf54lm20a fw 0.4.3 | USB + HiveHub BLE beacon
[WDT] armed: 60000 ms timeout, fed every 20000 ms
```

`[WDT] no enabled watchdog devicetree node` instead means `app.overlay` was
replaced — see the table above.

## Output artifacts

Each configuration writes to its own build directory:

```
build/                              build_lowpower/
└── merged.hex                      └── merged.hex          ← SWD flashing
└── firmware-nrf54lm20a/zephyr/     └── firmware-nrf54lm20a/zephyr/
    ├── zephyr.signed.bin               ├── zephyr.signed.bin
    └── hiveinside-nrf54lm20a-          └── hiveinside-nrf54lm20a-
        v0.4.3-bringup.signed.bin           v0.4.3-lowpower.signed.bin
```

`merged.hex` is MCUboot plus the signed application and is what **Flash**
programs over SWD. The version-stamped `.signed.bin` is the OTA payload — upload
that rather than `zephyr.signed.bin`, because every configuration writes the
same `zephyr.signed.bin` filename and nothing distinguishes two of them once
they leave their build directories. See [`ota-over-ble.md`](ota-over-ble.md).

## Notes

* **Pristine builds.** After changing any field in the form, or after pulling
  changes that touch `prj.conf` or `app.overlay`, use **Pristine Build**. A warm
  build directory is exactly where Zephyr's caches mislead.
* **`Include debug thread information`** adds `-DCONFIG_DEBUG_THREAD_INFO=y` for
  debugger thread awareness. Harmless, and not part of the low-power profile —
  leave it on for bring-up, and off for a deployment image if you want the
  smallest build.
* **Browse vs typing.** The **Browse** button inserts absolute paths. Those work
  fine; plain relative filenames are resolved against the application directory
  and are easier to read.
* **Extra CMake arguments** stays empty. Passing
  `-DEXTRA_CONF_FILE=low-power.conf -DEXTRA_DTC_OVERLAY_FILE=low-power.overlay`
  by hand is exactly equivalent if you prefer it.
* **CI does not build the low-power configuration.** CI compiles only the
  default configuration, through PlatformIO against `firmware-nrf54lm20a/zephyr/`.
  Build the deployment image yourself after changing anything it touches.

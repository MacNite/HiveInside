"""PlatformIO pre-build guard: refuse `pio run -t upload` when MCUboot is on.

PlatformIO's Zephyr builder only ever builds and flashes the *application*
image. Once `CONFIG_BOOTLOADER_MCUBOOT=y` is set, that application is linked at
the MCUboot slot-0 offset (behind an image header), so flashing it standalone
leaves nothing at address 0x0: the CPU faults before main() runs and the device
goes silent (no serial, no BLE) — the "builds fine but never boots" trap.

A bootable image is MCUboot + the signed application merged together, which only
a `west --sysbuild` build produces (build/merged.hex, flashed with `west flash`).
See docs/flashing.md. This guard aborts the upload so nobody bricks a board by
reflex; it stays out of the way when MCUboot is disabled (a plain dev build) and
never touches the compile-only `pio run`.
"""
import os

from SCons.Script import COMMAND_LINE_TARGETS, Exit, Import

Import("env")


def _mcuboot_enabled():
    # zephyr/prj.conf is the copy PlatformIO's Seeed Zephyr builder reads.
    conf = os.path.join(env.subst("$PROJECT_DIR"), "zephyr", "prj.conf")
    try:
        with open(conf, encoding="utf-8") as handle:
            for line in handle:
                stripped = line.strip()
                if stripped.startswith("#"):
                    continue
                if stripped.replace(" ", "") == "CONFIG_BOOTLOADER_MCUBOOT=y":
                    return True
    except OSError:
        pass
    return False


if "upload" in COMMAND_LINE_TARGETS and _mcuboot_enabled():
    print("\n" + "=" * 72)
    print("ERROR: `pio run -t upload` is disabled — MCUboot is enabled for this")
    print("firmware, so PlatformIO's application-only image is NOT bootable and")
    print("flashing it would brick the device (no serial output on boot).")
    print("")
    print("Build and flash the merged MCUboot + signed-app image instead:")
    print("    west build --sysbuild -b <board-target> path/to/firmware-nrf54lm20a")
    print("    west flash        # programs build/merged.hex over the debugger")
    print("See docs/flashing.md for details.")
    print("=" * 72 + "\n")
    Exit(1)

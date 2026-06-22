"""
rename_fw.py — PlatformIO post-build hook that stamps the firmware version onto
the built binary.

After every successful build PlatformIO produces
    .pio/build/<env>/firmware.bin
This hook reads the firmware version from the

    #ifndef HIVEINSIDE_FW_VERSION
    #define HIVEINSIDE_FW_VERSION "x.y.z"
    #endif

line in include/config.h and writes a copy named `hiveinside_x.y.z.bin` next to
it, so every build yields a clearly-labelled artifact ready to hand to the OTA
backend (see docs/ota-over-ble.md) or to archive.

The original `firmware.bin` is left untouched on purpose: `pio run -t upload`,
the debugger and any other tooling that expects the default name keep working.
(A literal rename/move would break those, since they look for `firmware.bin`.)

Wired in from platformio.ini:

    [env]
    extra_scripts = post:rename_fw.py
"""

import os
import re
import shutil

Import("env")  # noqa: F821  — `env` is injected by PlatformIO/SCons.

VERSION_MACRO = "HIVEINSIDE_FW_VERSION"


def read_fw_version(config_path):
    """Return the x.y.z string from `#define HIVEINSIDE_FW_VERSION "x.y.z"`.

    Returns None if the file or the macro can't be found.
    """
    pattern = re.compile(
        r'^\s*#\s*define\s+' + re.escape(VERSION_MACRO) + r'\s+"([^"]+)"',
        re.MULTILINE,
    )
    try:
        with open(config_path, "r", encoding="utf-8") as f:
            match = pattern.search(f.read())
    except OSError as exc:
        print("[rename_fw] WARNING: could not read %s (%s)" % (config_path, exc))
        return None
    return match.group(1) if match else None


def make_versioned_copy(source, target, env):
    """Post-action: copy firmware.bin -> hiveinside_<version>.bin."""
    config_path = os.path.join(env.subst("$PROJECT_INCLUDE_DIR"), "config.h")

    version = read_fw_version(config_path)
    if not version:
        print("[rename_fw] WARNING: %s not found in %s; skipping versioned copy."
              % (VERSION_MACRO, config_path))
        return

    firmware_path = target[0].get_abspath()
    versioned_path = os.path.join(
        os.path.dirname(firmware_path), "hiveinside_%s.bin" % version
    )
    shutil.copyfile(firmware_path, versioned_path)
    print("[rename_fw] Wrote %s" % versioned_path)


# Run after the application image (firmware.bin) has been built for this env.
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", make_versioned_copy)

# Conventional Zephyr entry point retained for west builds. PlatformIO uses
# zephyr/CMakeLists.txt because that is the Seeed Zephyr builder layout.
cmake_minimum_required(VERSION 3.20.0)

# west/NCS-only devicetree fixups (see ncs_fixups.overlay). Applied on top of
# the auto-included app.overlay and only here, so the PlatformIO build under
# zephyr/ — which uses a different board tree — is untouched. Must be set before
# find_package(Zephyr).
#
# A plain set() is correct and does not hide a caller's overlay: Zephyr reads
# this with zephyr_get(EXTRA_DTC_OVERLAY_FILE SYSBUILD LOCAL MERGE REVERSE),
# which merges every scope (sysbuild, CMake cache, environment, and this local
# variable) instead of letting the highest-precedence one win. A deployment
# overlay passed as -DEXTRA_DTC_OVERLAY_FILE=... therefore lands in the DTS list
# alongside the fixup below, with the fixup applied first.
set(EXTRA_DTC_OVERLAY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/ncs_fixups.overlay")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hiveinside_nrf54lm20a)

target_sources(app PRIVATE
  src/main.c
  src/fft.c
  src/hive_i2c.c
  src/power.c
  src/sht40.c
  src/accel.c
  src/mic.c
  src/battery.c
  src/beacon.c
  src/ota.c
)

# Conventional Zephyr entry point retained for west builds. PlatformIO uses
# zephyr/CMakeLists.txt because that is the Seeed Zephyr builder layout.
cmake_minimum_required(VERSION 3.20.0)

# west/NCS-only devicetree fixups (see ncs_fixups.overlay). Appended on top of
# the auto-included app.overlay and applied only here, so the PlatformIO build
# under zephyr/ — which uses a different board tree — is untouched. Must be set
# before find_package(Zephyr).
list(APPEND EXTRA_DTC_OVERLAY_FILE
  "${CMAKE_CURRENT_SOURCE_DIR}/ncs_fixups.overlay"
)

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

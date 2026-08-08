cmake_minimum_required(VERSION 3.20.0)

# Devicetree fixups (see ncs_fixups.overlay), applied on top of the
# auto-included app.overlay. Must be set before find_package(Zephyr).
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
  src/watchdog.c
  src/accel.c
  src/mic.c
  src/battery.c
  src/beacon.c
  src/ota.c
)

# ── Version-stamped OTA payload ────────────────────────────────────────────
#
# Zephyr always names the signed application `zephyr.signed.bin`, so every build
# configuration produces a file that is indistinguishable from the others the
# moment it leaves its build directory. That is a real hazard here rather than a
# cosmetic one: the bring-up image and the low-power deployment image
# (docs/low-power.md) are both valid OTA payloads, and pushing the wrong one to
# a sealed hive fails silently — the node boots, advertises, and simply behaves
# like the other build. Copy the signed image next to itself under a name that
# carries the firmware version and the variant.

set(hive_config_header ${CMAKE_CURRENT_SOURCE_DIR}/src/hive_config.h)
set_property(DIRECTORY APPEND
  PROPERTY CMAKE_CONFIGURE_DEPENDS ${hive_config_header})

file(READ ${hive_config_header} hive_config_contents)

# Read the same MAJOR/MINOR/PATCH numbers beacon.c advertises, not the derived
# HIVEINSIDE_FW_VERSION string. Those bytes are what the backend sees, so an
# artifact named v0.4.3 is only honest if it advertises 0.4.3.
set(hive_fw_version "")
foreach(component MAJOR MINOR PATCH)
  string(REGEX MATCH
    "define[ \t]+HIVEINSIDE_FW_VERSION_${component}[ \t]+([0-9]+)"
    hive_component_matched "${hive_config_contents}")
  if(hive_component_matched)
    list(APPEND hive_version_parts "${CMAKE_MATCH_1}")
  endif()
endforeach()
list(LENGTH hive_version_parts hive_version_part_count)
if(hive_version_part_count EQUAL 3)
  list(JOIN hive_version_parts "." hive_fw_version)
endif()

string(REGEX MATCH
  "define[ \t]+HIVEINSIDE_BOARD[ \t]+\"([A-Za-z0-9_]+)\""
  hive_board_matched "${hive_config_contents}")
if(hive_board_matched)
  set(hive_board "${CMAKE_MATCH_1}")
else()
  set(hive_board "")
endif()

# CONFIG_SERIAL is the honest discriminator. It is exactly what low-power.conf
# turns off, and it describes what the image *is* rather than how the build was
# requested, so a deployment image cannot end up carrying the bring-up name
# because a flag was passed in an unexpected way.
if(CONFIG_SERIAL)
  set(hive_image_variant "bringup")
else()
  set(hive_image_variant "lowpower")
endif()

if(NOT hive_fw_version OR NOT hive_board)
  message(WARNING
    "HiveInside: could not read HIVEINSIDE_FW_VERSION/HIVEINSIDE_BOARD from "
    "${hive_config_header}; skipping the version-stamped OTA payload.")
elseif(CONFIG_BOOTLOADER_MCUBOOT AND TARGET zephyr_final)
  set(hive_payload_name
    "hiveinside-${hive_board}-v${hive_fw_version}-${hive_image_variant}.signed.bin")

  add_custom_target(hiveinside_ota_payload ALL
    COMMAND ${CMAKE_COMMAND}
      -DHIVE_SRC=${PROJECT_BINARY_DIR}/zephyr/zephyr.signed.bin
      -DHIVE_DST=${PROJECT_BINARY_DIR}/zephyr/${hive_payload_name}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/stamp_ota_payload.cmake
    BYPRODUCTS ${PROJECT_BINARY_DIR}/zephyr/${hive_payload_name}
    COMMENT "Stamping OTA payload ${hive_payload_name}"
    VERBATIM
  )

  # Depend on the final image target rather than adding another POST_BUILD
  # command of our own. Signing is itself a POST_BUILD step of zephyr_final —
  # NCS appends it to Zephyr's `extra_post_build_commands` — and a dependent
  # target is guaranteed to run after all of them, whatever order the module
  # hooks happened to be registered in.
  add_dependencies(hiveinside_ota_payload zephyr_final)

  message(STATUS "HiveInside OTA payload: ${hive_payload_name}")
endif()

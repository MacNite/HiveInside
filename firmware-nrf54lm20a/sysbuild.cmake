# Sysbuild hook for the application image (sysbuild includes this file itself).
#
# The MCUboot image header carries a version of its own, and it is the one
# version covered by the image signature: imgtool signs it with the image, and
# ota.c compares it to refuse downgrades. Left alone it is 0.0.0+0 for every
# build, so stamp it from src/hive_config.h — the version the node advertises.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/hive_version.cmake)
hive_read_identity(${CMAKE_CURRENT_LIST_DIR}/src/hive_config.h
  hive_sign_version hive_sign_board)
if(hive_sign_version)
  set_config_string(${DEFAULT_IMAGE} CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION
    "${hive_sign_version}+0")
else()
  message(FATAL_ERROR "HiveInside: cannot read the firmware version from "
    "src/hive_config.h, so the signed image would carry version 0.0.0")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_LIST_DIR}/src/hive_config.h)

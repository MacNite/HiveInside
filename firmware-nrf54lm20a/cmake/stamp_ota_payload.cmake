# stamp_ota_payload.cmake — copy the signed application image to a
# version- and variant-stamped name.
#
# Run in script mode (cmake -P) from the post-build target in ../CMakeLists.txt,
# which is where the naming scheme and the reason for it are documented.
# Expects HIVE_SRC and HIVE_DST on the command line.
#
# A missing signed image is a warning, not an error: if a future SDK moves the
# artifact, an otherwise good build must not start failing over a convenience
# copy. The absence of the stamped file next to zephyr.signed.bin is the signal
# that something moved.

if(NOT DEFINED HIVE_SRC OR NOT DEFINED HIVE_DST)
  message(FATAL_ERROR "stamp_ota_payload.cmake: HIVE_SRC and HIVE_DST are required")
endif()

if(NOT EXISTS "${HIVE_SRC}")
  message(WARNING
    "HiveInside: no signed image at ${HIVE_SRC}; "
    "no version-stamped OTA payload was produced.")
  return()
endif()

execute_process(
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${HIVE_SRC}" "${HIVE_DST}"
  RESULT_VARIABLE hive_copy_result
)

if(NOT hive_copy_result EQUAL 0)
  message(FATAL_ERROR
    "HiveInside: failed to copy ${HIVE_SRC} to ${HIVE_DST} (${hive_copy_result})")
endif()

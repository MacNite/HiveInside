# Host regression tests for the firmware's protocol, lifecycle and DSP code.
#
# Each test compiles the real module sources from firmware-nrf54lm20a/src
# against the small Zephyr stand-ins in shim/, under AddressSanitizer and
# UndefinedBehaviorSanitizer, and runs them on the build machine — no Zephyr
# workspace or board needed:
#
#   cmake -S tests/host -B build-host && cmake --build build-host
#   ctest --test-dir build-host --output-on-failure
cmake_minimum_required(VERSION 3.20)
project(hiveinside_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
enable_testing()

set(FW_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware-nrf54lm20a/src)
set(SANITIZE -fsanitize=address,undefined -fno-sanitize-recover=all
             -fno-omit-frame-pointer)

add_library(zephyr_shim STATIC shim/shim.c)
target_include_directories(zephyr_shim PUBLIC shim/include)
target_compile_definitions(zephyr_shim PUBLIC _GNU_SOURCE)
target_compile_options(zephyr_shim PUBLIC ${SANITIZE} -g -O1 -Wall -Wextra)
target_link_options(zephyr_shim PUBLIC ${SANITIZE})
target_link_libraries(zephyr_shim PUBLIC pthread m)

function(hive_host_test name)
  add_executable(${name} ${name}.c)
  # The module sources are #included by the test, so the firmware directory
  # is on the quote path and their own headers resolve as in the real build.
  target_include_directories(${name} PRIVATE ${FW_SRC})
  target_compile_options(${name} PRIVATE -Werror -Wno-unused-function
                                         ${ARGN})
  target_link_libraries(${name} PRIVATE zephyr_shim)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()

hive_host_test(test_ota)
# A short stall timeout keeps the stalled-link case quick.
hive_host_test(test_audio -DCONFIG_BT_BUF_ACL_TX_COUNT=6
                          -DHIVE_AUDIO_STALL_TIMEOUT_MS=300U)

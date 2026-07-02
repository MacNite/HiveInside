# HiveInside — XIAO nRF54LM20A Sense firmware (nRF Connect SDK / Zephyr).
#
# Build target board: xiao_nrf54lm20a/nrf54lm20a/cpuapp
#
#   west build -b xiao_nrf54lm20a/nrf54lm20a/cpuapp firmware-nrf54lm20a
#
# or use the "Add build configuration" button in the nRF Connect for VS Code
# extension and pick that board target.
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hiveinside_nrf54lm20a)

target_sources(app PRIVATE src/main.c)

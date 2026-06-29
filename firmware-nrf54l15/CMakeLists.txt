# HiveInside — XIAO nRF54L15 Sense firmware (nRF Connect SDK / Zephyr).
#
# Build target board: xiao_nrf54l15/nrf54l15/cpuapp
#
#   west build -b xiao_nrf54l15/nrf54l15/cpuapp firmware-nrf54l15
#
# or use the "Add build configuration" button in the nRF Connect for VS Code
# extension and pick that board target.
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hiveinside_nrf54l15)

target_sources(app PRIVATE src/main.c)

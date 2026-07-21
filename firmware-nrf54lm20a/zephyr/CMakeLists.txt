# PlatformIO's Seeed Zephyr builder uses this directory as the application root.
# The source remains in ../src so the root CMakeLists.txt can serve west builds.
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hiveinside_nrf54lm20a)

target_sources(app PRIVATE
  ../src/main.c
  ../src/fft.c
  ../src/hive_i2c.c
  ../src/power.c
  ../src/sht40.c
  ../src/accel.c
  ../src/mic.c
  ../src/battery.c
  ../src/beacon.c
  ../src/gatt_hive.c
)

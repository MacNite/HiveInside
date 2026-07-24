# PlatformIO's Seeed Zephyr builder treats this directory as its application
# root. Keep the source in ../src so the root CMakeLists.txt also works with
# a normal west build.
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nrf54lm20a_bt_smoketest)

target_sources(app PRIVATE ../src/main.c)

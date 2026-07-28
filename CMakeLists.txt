# tab5-caster — top-level ESP-IDF project file
#
# Target: ESP32-P4 (M5Stack Tab5).  Build on a machine with ESP-IDF 5.4+:
#   idf.py set-target esp32p4
#   idf.py menuconfig     # verify USB host / PSRAM / lwIP sockets
#   idf.py build flash monitor
#
# NOTE: These sources have NOT been compiled — no IDF toolchain on the authoring
# machine. Treat as a skeleton; the usb_host_cdc_acm API field set may need to
# match the exact component version pulled by the component manager.

cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tab5-caster)

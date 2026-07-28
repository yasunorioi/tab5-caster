# tab5-caster — top-level ESP-IDF project file
#
# Target: ESP32-P4 (M5Stack Tab5).  Build on a machine with ESP-IDF 5.4+:
#   idf.py set-target esp32p4
#   idf.py menuconfig     # verify USB host / PSRAM / lwIP sockets
#   idf.py build flash monitor
#
# NOTE: Builds clean for esp32p4 on ESP-IDF 5.4.4 (usb_host_cdc_acm 2.4.0).
# Runtime untested — see TODO(hw) markers. Compilation is not hardware proof.

cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tab5-caster)

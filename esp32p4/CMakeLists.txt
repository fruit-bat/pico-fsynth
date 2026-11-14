# pico-fsynth for the ESP32-P4 (ESP-IDF project). See README.md.
#
#   . ~/esp/esp-idf-v6.1.1/export.sh
#   idf.py build flash monitor
#
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(fsynth_p4)

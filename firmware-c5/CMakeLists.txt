# SPDX-License-Identifier: GPL-3.0-or-later
# SignalSweep C5 — the radio. ESP-IDF >= 5.5, target esp32c5.
#   idf.py set-target esp32c5
#   idf.py build
#   idf.py -p <port> flash monitor
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../shared")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(signalsweep_c5)

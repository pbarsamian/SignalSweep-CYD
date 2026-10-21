# SPDX-License-Identifier: GPL-3.0-or-later
# SignalSweep C5 — the radio. ESP-IDF >= 5.5, target esp32c5.
#   idf.py set-target esp32c5
#   idf.py build
#   idf.py -p <port> flash monitor
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../shared")
# Firmware version = the git revision, reported to the CYD in HELLO and shown on its About page.
# Evaluated when cmake configures (a new commit re-runs it); uncommitted edits show as -dirty only after a reconfigure.
execute_process(COMMAND git describe --always --dirty --abbrev=7 --match=NOTHING
                WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                OUTPUT_VARIABLE SS_GIT_VER OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT SS_GIT_VER)
  set(SS_GIT_VER "unknown")
endif()
set(PROJECT_VER "${SS_GIT_VER}")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../.git/index")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../.git/index")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(signalsweep_c5)

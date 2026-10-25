# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT
# Independent protocol/state tests; no GUI, driver or third-party media library.
cmake_minimum_required(VERSION 3.24)
project(DeskflowDisplayFoundation LANGUAGES CXX)
enable_testing()
get_filename_component(DESKFLOW_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
add_subdirectory("${DESKFLOW_SOURCE_ROOT}/src/lib/display" display)
add_subdirectory("${DESKFLOW_SOURCE_ROOT}/src/unittests/display" tests)

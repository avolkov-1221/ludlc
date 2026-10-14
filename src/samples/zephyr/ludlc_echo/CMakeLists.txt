# SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only)
#
# @file CMakeLists.txt
#
# @brief Builds the Zephyr echo sample.
#
# @copyright
# Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(ludlc_echo)

zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# Add the application sources
target_sources(app PRIVATE
	ludlc_echo.c
)

# Link against the ludlc library, which is provided by the
# OOT module (this project).
#target_link_libraries(app PRIVATE ludlc)

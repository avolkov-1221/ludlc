# SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
#
# This is the main Zephyr application CMakeLists.txt
#
# Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
#
# This file is licensed under either the Apache License, Version 2.0,
# or the GNU General Public License, version 2 or (at your option)
# any later version.


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

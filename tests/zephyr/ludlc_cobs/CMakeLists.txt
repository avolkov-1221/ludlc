# SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only)
#
# @file CMakeLists.txt
#
# @brief Builds the Zephyr COBS test.
#
# @copyright
# Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ludlc_cobs)

target_sources(app PRIVATE src/main.c)

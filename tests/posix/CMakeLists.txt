# SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
#
# @file CMakeLists.txt
#
# @brief Adds the POSIX test suites: COBS and the framings.
#
# @copyright
# Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors

add_definitions(${PLATFORM_DEFS})

add_subdirectory(ludlc_cobs)
add_subdirectory(ludlc_framing)

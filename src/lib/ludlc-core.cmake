# SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
#
# @file ludlc-core.cmake
#
# @brief The core translation units, for a CMake-based port.
#
# @copyright
# Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
#
# @details
# The list is kept here, so that a port outside the tree keeps building when
# the core gains or loses a file.
#
# LUDLC_CORE_SRC holds absolute paths, and lists nothing from platform/: a
# port declares its own files.

set(LUDLC_CORE_SRC ${CMAKE_CURRENT_LIST_DIR}/ludlc.c)

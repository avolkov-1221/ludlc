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

# The protocol, and whichever framings the port needs. A codec file has no
# #ifdef on its own config, so a framing is left out of the image by dropping
# its file here. Pass the same setting to the compiler as
# CONFIG_LUDLC_FRAMING_*, which is what the headers read.
set(LUDLC_CORE_SRC ${CMAKE_CURRENT_LIST_DIR}/ludlc.c)

if(CONFIG_LUDLC_FRAMING_HDLC)
        list(APPEND LUDLC_CORE_SRC ${CMAKE_CURRENT_LIST_DIR}/ludlc_hdlc.c)
endif(CONFIG_LUDLC_FRAMING_HDLC)

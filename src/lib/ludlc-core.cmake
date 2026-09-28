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
# Every CMake build in the tree takes the list from here, so a port outside
# the tree keeps building when the core gains or loses a file.
#
# LUDLC_CORE_SRC holds absolute paths, and lists nothing from platform/: a
# port declares its own files. See ludlc-core.mk for the make equivalent.

# The protocol, and whichever framings the port needs. A codec file has no
# #ifdef on its own config, so a framing is left out of the image by dropping
# its file here. Pass the same setting to the compiler as
# CONFIG_LUDLC_FRAMING_*, which is what the headers read.
set(LUDLC_CORE_SRC ${CMAKE_CURRENT_LIST_DIR}/ludlc.c)
set(LUDLC_FRAMINGS_ON 0)

if(CONFIG_LUDLC_FRAMING_COBS)
        list(APPEND LUDLC_CORE_SRC ${CMAKE_CURRENT_LIST_DIR}/ludlc_cobs.c)
        math(EXPR LUDLC_FRAMINGS_ON "${LUDLC_FRAMINGS_ON} + 1")
endif(CONFIG_LUDLC_FRAMING_COBS)

if(CONFIG_LUDLC_FRAMING_HDLC)
        list(APPEND LUDLC_CORE_SRC ${CMAKE_CURRENT_LIST_DIR}/ludlc_hdlc.c)
        math(EXPR LUDLC_FRAMINGS_ON "${LUDLC_FRAMINGS_ON} + 1")
endif(CONFIG_LUDLC_FRAMING_HDLC)

if(CONFIG_LUDLC_FRAMING_RAW)
        list(APPEND LUDLC_CORE_SRC ${CMAKE_CURRENT_LIST_DIR}/ludlc_raw.c)
        math(EXPR LUDLC_FRAMINGS_ON "${LUDLC_FRAMINGS_ON} + 1")
endif(CONFIG_LUDLC_FRAMING_RAW)

# ludlc_framing.c turns a name into one of the framings built, which a port
# carrying a single one has nothing to do with.
if(LUDLC_FRAMINGS_ON GREATER 1)
        list(APPEND LUDLC_CORE_SRC ${CMAKE_CURRENT_LIST_DIR}/ludlc_framing.c)
endif()

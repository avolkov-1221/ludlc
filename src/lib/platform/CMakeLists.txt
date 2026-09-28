# SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
#
# @file CMakeLists.txt
#
# @brief Adds the port of the selected platform to the library build.
#
# @copyright
# Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
#
# @details
# The port is the in-tree directory TARGET_PLATFORM names, or the one
# LUDLC_PLATFORM_DIR gives. Either must hand its sources back in PLATFORM_SRC,
# and the build stops here if it did not; see doc/PORTING.md.

# add_subdirectory() needs an explicit binary directory when the source is
# outside this tree, so an external port is given one named after it. An
# in-tree port is added by name.
if(LUDLC_PLATFORM_EXTERNAL)
  add_subdirectory(${LUDLC_PLATFORM_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_PLATFORM})
else()
  add_subdirectory(${TARGET_PLATFORM})
endif()

if(NOT PLATFORM_SRC)
  message(FATAL_ERROR
    "The '${TARGET_PLATFORM}' port set no PLATFORM_SRC. Its CMakeLists.txt "
    "must set it to the sources to compile; see "
    "${LUDLC_TOP_DIR}/doc/PORTING.md")
endif()

# SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only)
#
# @file CMakeLists.txt
#
# @brief Builds the LuDLC sources: the public headers, the library and the
# samples.
#
# @copyright
# Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
#
# @details
# The public headers in include/ become an interface target for the library and
# its users, or Zephyr include directories in a Zephyr build. The library comes
# from lib/. The samples are added unless LuDLC is built as part of another
# project or, outside Zephyr, LUDLC_ENABLE_BINARIES is off.

set(LUDLC_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(LUDLC_BUILD_LIBRARY)
  # Define a global LUDLC interface target. A port adds its own directory to
  # it, which is how ludlc_platform.h and friends are found.
  add_library(ludlc_includes INTERFACE)

  # Add the include directory
  target_include_directories(ludlc_includes INTERFACE
    ${LUDLC_INCLUDE}
  )

  # The version, suffix included, for the library and its users to print.
  target_compile_definitions(ludlc_includes INTERFACE
    LUDLC_VERSION_STRING="${LUDLC_VERSION_STRING}")
elseif(TARGET_PLATFORM_ZEPHYR)
  zephyr_include_directories(${LUDLC_INCLUDE})
  zephyr_compile_definitions(
    LUDLC_VERSION_STRING="${LUDLC_VERSION_STRING}")
endif()

add_subdirectory(lib)

# Samples are standalone applications.
# Skip them when LuDLC is used as a dependency
if(NOT LUDLC_AS_SUBPROJECT AND (LUDLC_ENABLE_BINARIES OR TARGET_PLATFORM_ZEPHYR))
  add_subdirectory(samples)
endif()

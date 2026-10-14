# SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only)
#
# @file CMakeLists.txt
#
# @brief Adds the tests that run on the build host.
#
# @copyright
# Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors

if(TARGET_PLATFORM_POSIX)
  add_subdirectory(posix)
endif()

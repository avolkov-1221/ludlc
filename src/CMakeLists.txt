# SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
#
# Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
#
# This file is licensed under either the Apache License, Version 2.0,
# or the GNU General Public License, version 2 or (at your option)
# any later version.

set(LUDLC_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(TARGET_PLATFORM_POSIX)
	# Define a global LUDLC interface target
	add_library(ludlc_includes INTERFACE)

	# Add the include directory
	target_include_directories(ludlc_includes INTERFACE
		${LUDLC_INCLUDE}
	)
elseif(TARGET_PLATFORM_ZEPHYR)
	zephyr_include_directories(${LUDLC_INCLUDE})
endif()

add_subdirectory(lib)

if(CONFIG_ENABLE_BINARIES OR TARGET_PLATFORM_ZEPHYR)
	add_subdirectory(samples)
endif()

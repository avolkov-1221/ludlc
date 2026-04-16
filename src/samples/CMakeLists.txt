# SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
#
# Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
#
# This file is licensed under either the Apache License, Version 2.0,
# or the GNU General Public License, version 2 or (at your option)
# any later version.

add_definitions(${PLATFORM_DEFS})

if(TARGET_PLATFORM_POSIX)
	set(SRC
		ludlc_demo.c
	)
	set(DEMO_BIN ${CMAKE_PROJECT_NAME}_demo)

	add_executable(${DEMO_BIN} ${SRC} ${HDR})

	target_link_libraries(${DEMO_BIN}
		PUBLIC
		${CMAKE_THREAD_LIBS_INIT}
		${LIBS}
	)

	target_link_libraries(${DEMO_BIN}
		PRIVATE
		ludlc_includes
		${CMAKE_PROJECT_NAME}
	)
elseif(TARGET_PLATFORM_ZEPHYR)
	add_subdirectory(zephyr)
endif()

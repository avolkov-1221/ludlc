# SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
#
# @file CMakeLists.txt
#
# @brief Builds the LuDLC samples for the selected platform.
#
# @copyright
# Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors

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

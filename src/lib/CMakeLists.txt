# SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
#
# Copyright (C) 2025 Andrey VOLKOV <andrey@volkov.fr> and LuDLC Contributors
#
# This file is licensed under either the Apache License, Version 2.0,
# or the GNU General Public License, version 2 or (at your option)
# any later version.

set(CMAKE_C_STANDARD_REQUIRED ON)

# Set the project root directory if it's not already defined.
if(NOT DEFINED LUDLC_TOP_DIR)
	set(LUDLC_TOP_DIR ${CMAKE_SOURCE_DIR})
endif()

if(NOT TARGET_PLATFORM_ZEPHYR)
	option(CONFIG_LUDLC_MT
		"Build LuDLC with multithreading support." OFF)
	option(CONFIG_STATIC_LUDLC_LIBRARY
		"Build LuDLC static library." ON)
	option(CONFIG_SHARED_LUDLC_LIBRARY
		"Build LuDLC shared library." OFF)
	option(CONFIG_LUDLC_SERIAL_CONN
		"Build LuDLC with serial connections support." ON)
	option(CONFIG_LUDLC_SPI_CONN
		"Build LuDLC with SPI connections support." OFF)
	option(CONFIG_LUDLC_CAN_CONN
		"Build LuDLC with CAN connections support." OFF)
	set(target_libraries ${ludlc_target})
endif()

add_subdirectory(platform)
add_definitions(${PLATFORM_DEFS})

set(LUDLC_SRC
	ludlc.c
	${PLATFORM_SRC}
)

set(ludlc_target ludlc)
set(ludlc_target ${ludlc_target} PARENT_SCOPE)

if(TARGET_PLATFORM_POSIX)
	set(target_libraries ${ludlc_target})

	if(CONFIG_STATIC_LUDLC_LIBRARY)
		set(ludlc_static_target	${ludlc_target})
		if(CONFIG_SHARED_LUDLC_LIBRARY)
			string(APPEND ludlc_static_target	"_static")
			list(APPEND target_libraries
				${ludlc_static_target}
				)
		endif(CONFIG_SHARED_LUDLC_LIBRARY)
	endif(CONFIG_STATIC_LUDLC_LIBRARY)

	target_include_directories(ludlc_includes INTERFACE
		${CMAKE_CURRENT_SOURCE_DIR}/include
	)

	set(libs "${libs}" ludlc_includes)

	if(CONFIG_STATIC_LUDLC_LIBRARY)
		add_library(${ludlc_static_target} STATIC ${LUDLC_SRC})
		set_target_properties(${ludlc_static_target}
			PROPERTIES OUTPUT_NAME ludlc)
		target_link_libraries(${ludlc_static_target} PUBLIC ${libs})
	endif(CONFIG_STATIC_LUDLC_LIBRARY)

	if(CONFIG_SHARED_LUDLC_LIBRARY)
		set(CMAKE_LIBRARY_PATH ${CMAKE_CURRENT_BINARY_DIR})
		add_library(${ludlc_lib} SHARED ${LUDLC_SRC})
		set_target_properties(${ludlc_lib} PROPERTIES VERSION
				${CMAKE_PROJECT_VERSION} SOVERSION 1)
		target_link_libraries(${ludlc_lib} PUBLIC ${libs})
	endif(CONFIG_SHARED_LUDLC_LIBRARY)

elseif(TARGET_PLATFORM_ZEPHYR)
	zephyr_library()
	if(CONFIG_LUDLC)
		zephyr_library_sources(${LUDLC_SRC})
		zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
	endif()
endif()

# SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
#
# Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
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
  option(LUDLC_STATIC_LIBRARY
    "Build LuDLC static library." ON)
  option(LUDLC_SHARED_LIBRARY
    "Build LuDLC shared library." OFF)
  option(CONFIG_LUDLC_SERIAL_CONN
    "Build LuDLC with serial connections support." ON)
## TODO: uncomment when it will be ready
##	option(CONFIG_LUDLC_SPI_CONN
##		"Build LuDLC with SPI connections support." OFF)
##	option(CONFIG_LUDLC_CAN_CONN
##		"Build LuDLC with CAN connections support." OFF)
  option(LUDLC_STATIC_CONN
    "POSIX: allocate serial connections from a fixed pool (no heap)." OFF)
  set(LUDLC_STATIC_CONN_NUM 1 CACHE STRING
    "POSIX: number of static serial connection slots")
  set(target_libraries ${ludlc_target})
endif()

if(TARGET_PLATFORM_POSIX AND LUDLC_STATIC_CONN)
  if(NOT CONFIG_LUDLC_SERIAL_CONN)
    message(FATAL_ERROR
      "LUDLC_STATIC_CONN requires CONFIG_LUDLC_SERIAL_CONN")
  endif()
  if(LUDLC_STATIC_CONN_NUM LESS 1)
    message(FATAL_ERROR
      "LUDLC_STATIC_CONN_NUM must be at least 1")
  endif()
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

  if(LUDLC_STATIC_LIBRARY)
    set(ludlc_static_target ${ludlc_target})
    if(LUDLC_SHARED_LIBRARY)
      string(APPEND ludlc_static_target "_static")
      list(APPEND target_libraries
        ${ludlc_static_target}
      )
    endif(LUDLC_SHARED_LIBRARY)
  endif(LUDLC_STATIC_LIBRARY)

  target_include_directories(ludlc_includes INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  )

  set(libs "${libs}" ludlc_includes)

  if(LUDLC_STATIC_LIBRARY)
    add_library(${ludlc_static_target} STATIC ${LUDLC_SRC})
    set_target_properties(${ludlc_static_target}
      PROPERTIES OUTPUT_NAME ludlc)
    target_link_libraries(${ludlc_static_target} PUBLIC ${libs})
  endif(LUDLC_STATIC_LIBRARY)

  if(LUDLC_SHARED_LIBRARY)
    set(CMAKE_LIBRARY_PATH ${CMAKE_CURRENT_BINARY_DIR})
    add_library(${ludlc_lib} SHARED ${LUDLC_SRC})
    set_target_properties(${ludlc_lib} PROPERTIES VERSION
        ${CMAKE_PROJECT_VERSION} SOVERSION 1)
    target_link_libraries(${ludlc_lib} PUBLIC ${libs})
  endif(LUDLC_SHARED_LIBRARY)

  # timer_create / clock_gettime may live in librt on some POSIX systems
  find_library(LUDLC_POSIX_RT_LIB NAMES rt)
  if(LUDLC_POSIX_RT_LIB)
    if(LUDLC_STATIC_LIBRARY)
      target_link_libraries(${ludlc_static_target}
        PRIVATE ${LUDLC_POSIX_RT_LIB})
    endif()
    if(LUDLC_SHARED_LIBRARY)
      target_link_libraries(${ludlc_lib}
        PRIVATE ${LUDLC_POSIX_RT_LIB})
    endif()
  endif()

elseif(TARGET_PLATFORM_ZEPHYR)
  zephyr_library()
  if(CONFIG_LUDLC)
    zephyr_library_sources(${LUDLC_SRC})
    zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
  endif()
endif()

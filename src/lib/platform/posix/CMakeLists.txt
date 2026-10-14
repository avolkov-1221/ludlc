# SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only)
#
# @file CMakeLists.txt
#
# @brief Adds the POSIX port to the library build.
#
# @copyright
# Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors

set(CONFIG_LUDLC_MT ON CACHE BOOL "Enable multithreading" FORCE)
find_package(Threads REQUIRED)

set(CONFIG_LUDLC_FRAMING_HDLC ON CACHE BOOL "Enable hdlc coding" FORCE)
set(CONFIG_LUDLC_FRAMING_COBS OFF CACHE BOOL "Enable cobs coding" FORCE)
set(CONFIG_LUDLC_FRAMING_RAW OFF CACHE BOOL "Enable raw coding" FORCE)

configure_file(
	${CMAKE_CURRENT_SOURCE_DIR}/config.h.in
	${CMAKE_CURRENT_BINARY_DIR}/ludlc_autoconf.h
)

target_include_directories(ludlc_includes INTERFACE
	"${CMAKE_CURRENT_SOURCE_DIR}"
	"${CMAKE_CURRENT_BINARY_DIR}"
)

# Any other name, such as LOG_INFO from log.h, reads as 0 in the #if tests of
# ludlc_logger.h and would silently compile all logging out.
set(MAX_LOG_LEVEL LUDLC_LOG_LVL_INFO CACHE STRING
	"POSIX: most verbose log level compiled in (LUDLC_LOG_LVL_*)")
if(NOT MAX_LOG_LEVEL MATCHES
   "^LUDLC_LOG_LVL_(FATAL|ERROR|WARN|INFO|DEBUG|TRACE)$")
	message(FATAL_ERROR "MAX_LOG_LEVEL must be a LUDLC_LOG_LVL_* level")
endif()
target_compile_definitions(ludlc_includes INTERFACE
	MAX_LOG_LEVEL=${MAX_LOG_LEVEL})

# ISO C11 + POSIX.1-2008 (no _GNU_SOURCE — library stays libc-agnostic).
# Requires: clock_gettime(CLOCK_MONOTONIC), timer_create/timer_settime (link librt
# where the platform keeps those symbols), pthreads, C11 atomics.
set(PLATFORM_DEFS
	"-D_POSIX_C_SOURCE=200809L"
	CACHE INTERNAL "POSIX feature macros for LuDLC library")

# Require C11 standard
set(CMAKE_C_STANDARD 11 CACHE STRING  "Require C standard globally" FORCE)
set(CMAKE_C_STANDARD_REQUIRED ON CACHE BOOL "Require C standard globally" FORCE)
set(CMAKE_C_EXTENSIONS ON CACHE BOOL "Enable C extensions globally" FORCE)

set(COMMON
	${CMAKE_CURRENT_SOURCE_DIR}/log.h
	${CMAKE_CURRENT_SOURCE_DIR}/log.c
	${CMAKE_CURRENT_SOURCE_DIR}/ludlc_posix_platform.c
	${CMAKE_CURRENT_SOURCE_DIR}/ludlc_platform.h
	${CMAKE_CURRENT_SOURCE_DIR}/ludlc_platform_args.h
	${CMAKE_CURRENT_SOURCE_DIR}/kfifo.h
)

if(CONFIG_LUDLC_SERIAL_CONN)
	set(SERIAL_SRC
		${CMAKE_CURRENT_SOURCE_DIR}/ludlc_posix_serial.c
	)
endif(CONFIG_LUDLC_SERIAL_CONN)

set(PLATFORM_SRC
	${COMMON}
	${SERIAL_SRC}
	${CMAKE_CURRENT_BINARY_DIR}/ludlc_autoconf.h
	CACHE INTERNAL "posix sources"
)

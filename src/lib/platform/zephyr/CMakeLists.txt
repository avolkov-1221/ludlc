# SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
#
# @file CMakeLists.txt
#
# @brief Adds the Zephyr port to the library build.
#
# @copyright
# Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors

# Zephyr platform

zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR})

set(COMMON
	${CMAKE_CURRENT_SOURCE_DIR}/ludlc_platform.h
	${CMAKE_CURRENT_SOURCE_DIR}/ludlc_platform_args.h
	${CMAKE_CURRENT_SOURCE_DIR}/ludlc_zephyr_platform.c
)

if(CONFIG_LUDLC_SERIAL_CONN)
	set(SERIAL_SRC
		${CMAKE_CURRENT_SOURCE_DIR}/ludlc_zephyr_serial.c
	)
endif(CONFIG_LUDLC_SERIAL_CONN)

if(CONFIG_LUDLC_SPI_CONN)
	set(SPI_SRC
		${CMAKE_CURRENT_SOURCE_DIR}/ludlc_spi_slave.h
		${CMAKE_CURRENT_SOURCE_DIR}/ludlc_zephyr_spi_slave.c
	)
endif(CONFIG_LUDLC_SPI_CONN)

# The checksums belong to the link, whichever transport carries it.
if(CONFIG_LUDLC_SERIAL_CONN OR CONFIG_LUDLC_SPI_CONN)
	set(CSUM_SRC
		${CMAKE_CURRENT_SOURCE_DIR}/ludlc_zephyr_csum.c
	)
endif()

# Propagate variables up to the parent(s)
set(PLATFORM_SRC
	${COMMON}
	${SERIAL_SRC}
	${SPI_SRC}
	${CSUM_SRC}
	CACHE INTERNAL "zephyr sources"
)

# Set the include path for zephyr-specific headers
#target_include_directories(ludlc_includes INTERFACE
#	"${CMAKE_CURRENT_SOURCE_DIR}"
#)

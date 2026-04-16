# SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
#
# Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
#
# This file is licensed under either the Apache License, Version 2.0,
# or the GNU General Public License, version 2 or (at your option)
# any later version.

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

# Propagate variables up to the parent(s)
set(PLATFORM_SRC
	${COMMON}
	${SERIAL_SRC}
	CACHE INTERNAL "zephyr sources"
)

# Set the include path for zephyr-specific headers
#target_include_directories(ludlc_includes INTERFACE
#	"${CMAKE_CURRENT_SOURCE_DIR}"
#)

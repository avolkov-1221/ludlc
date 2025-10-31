# SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
#
# Copyright (C) 2025 Andrey VOLKOV <andrey@volkov.fr> and LuDLC Contributors
#
# This file is licensed under either the Apache License, Version 2.0,
# or the GNU General Public License, version 2 or (at your option)
# any later version.

add_subdirectory(${TARGET_PLATFORM})
include_directories(
	${TARGET_PLATFORM} 
)

target_include_directories(ludlc_includes INTERFACE
	${TARGET_PLATFORM}
	"${CMAKE_CURRENT_BINARY_DIR}/${TARGET_PLATFORM}"
)

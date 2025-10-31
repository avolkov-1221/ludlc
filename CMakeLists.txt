# SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
#
# Copyright (C) 2025 Andrey VOLKOV <andrey@volkov.fr> and LuDLC Contributors
#
# This file is licensed under either the Apache License, Version 2.0,
# or the GNU General Public License, version 2 or (at your option)
# any later version.

cmake_minimum_required(VERSION 3.15)

project(ludlc LANGUAGES C VERSION 1.0.0)

include(CheckLibraryExists)
include(CheckIncludeFiles)

# Default to Debug build type if not specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	message(STATUS "No build type specified. Setting CMAKE_BUILD_TYPE=Debug by default.")
	set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type (default Debug)" FORCE)

	# Optional: Provide a list of valid build types
	set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release")
endif()

# Check if LUDLC is being built as part of an another project (via add_subdirectory)
if(NOT DEFINED LUDLC_AS_SUBPROJECT)
	set(LUDLC_AS_SUBPROJECT ON)
	if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
		set(LUDLC_AS_SUBPROJECT OFF)
	endif()
endif()

# Set the project top directory.
set(LUDLC_TOP_DIR ${CMAKE_CURRENT_SOURCE_DIR})

option(CONFIG_ENABLE_BINARIES "Build LuDLC test and demo programs." ON)
option(DISABLE_PACKAGE_CONFIG_AND_INSTALL
	"Disable package configuration, target export and installation"
	${LUDLC_AS_SUBPROJECT})

# If this is the root project add longer list of available CMAKE_BUILD_TYPE values
if(NOT LUDLC_AS_SUBPROJECT)
	set(CMAKE_BUILD_TYPE ${CMAKE_BUILD_TYPE}
	CACHE STRING "Choose the type of build: Debug Release"
	FORCE)
endif()

# Arch selection (example default)
set(TARGET_PLATFORM "posix" CACHE STRING
	"Select target architecture (directory name under src/arch)")
set(TARGET_PLATFORM_POSIX 1)

find_program(CCACHE_FOUND ccache)
if(CCACHE_FOUND)
	set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE ccache)
	set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK ccache)
endif(CCACHE_FOUND)

add_subdirectory(src)
if(NOT DEFINED DISABLE_PACKAGE_CONFIG_AND_INSTALL)
add_subdirectory(doc)
endif()

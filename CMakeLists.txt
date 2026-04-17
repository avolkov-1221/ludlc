# SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
#
# Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
#
# This file is licensed under either the Apache License, Version 2.0,
# or the GNU General Public License, version 2 or (at your option)
# any later version.

cmake_minimum_required(VERSION 3.20.0)

include(CheckLibraryExists)
include(CheckIncludeFiles)

set(project_name ludlc)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

#--------------------------------------------------------------------
# A simple function to parse a "key = value" version file.
#
# It reads the specified file line by line, skips comments,
# and sets a variable in the parent scope for each "KEY = VALUE"
# pair it finds.
#
# @param FILE_PATH The path to the version file.
#--------------------------------------------------------------------
function(parse_version_file FILE_PATH)
  if(NOT EXISTS ${FILE_PATH})
    message(FATAL_ERROR "Version file not found: ${FILE_PATH}")
    return()
  endif()

  file(STRINGS ${FILE_PATH} VERSION_LINES)

  foreach(LINE ${VERSION_LINES})

    # Skip empty lines or comments
    if(line MATCHES "^[ \t]*#" OR line STREQUAL "")
      continue()
    endif()

    # Split at the first '='
    string(REGEX MATCH "^[^=]+" KEY "${LINE}")
    string(REGEX REPLACE "^[^=]+=" "" VALUE "${LINE}")

    string(STRIP "${KEY}" KEY)
    if(KEY STREQUAL "")
      message(WARNING "Could not parse line in "
        "${FILE_PATH}: ${LINE}")
      continue()
    endif()

    string(STRIP "${VALUE}" VALUE)

    set(${KEY} "${VALUE}" PARENT_SCOPE)
  endforeach()
endfunction()
#--------------------------------------------------------------------
# End of parser function
#--------------------------------------------------------------------

if(NOT DEFINED TARGET_PLATFORM)
  if(DEFINED ZEPHYR_BASE)
    set(TARGET_PLATFORM "zephyr" CACHE INTERNAL "target platform")
  else()
    set(TARGET_PLATFORM "posix" CACHE INTERNAL "target platform")
  endif()

  message(STATUS
    "Setting target platform to '${TARGET_PLATFORM}' "
    "as none was specified.")
endif()

string(TOLOWER "${TARGET_PLATFORM}" TARGET_PLATFORM)
message(STATUS "The selected platform is '${TARGET_PLATFORM}'")

if(NOT TARGET_PLATFORM STREQUAL "zephyr")
  set(VERSION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
  # Call the parser. This will create variables like
  #    VERSION_MAJOR, VERSION_MINOR, etc. in this scope.
  parse_version_file(${VERSION_FILE})

  # Check that the required variables were loaded
  if(NOT DEFINED VERSION_MAJOR OR NOT DEFINED VERSION_MINOR OR
    NOT DEFINED PATCHLEVEL)
    message(FATAL_ERROR "VERSION_MAJOR, VERSION_MINOR, or "
      "PATCHLEVEL not found in ${VERSION_FILE}")
  endif()

  set(PROJECT_VERSION "${VERSION_MAJOR}.${VERSION_MINOR}.${PATCHLEVEL}")

  # Optionally add the extra version info if it's defined and not empty
  if(DEFINED EXTRAVERSION AND NOT EXTRAVERSION STREQUAL "")
    set(PROJECT_VERSION "${PROJECT_VERSION}${EXTRAVERSION}")
  endif()

  project(${project_name} VERSION ${PROJECT_VERSION} LANGUAGES C)
endif()

if(TARGET_PLATFORM STREQUAL "posix")

  set(TARGET_PLATFORM_POSIX 1)
  option(LUDLC_ENABLE_BINARIES "Build LuDLC test and demo programs." ON)

elseif(TARGET_PLATFORM STREQUAL "zephyr")
  find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
  project(${project_name})

  set(WITH_ZEPHYR 1)
  set(TARGET_PLATFORM_ZEPHYR 1)

  # Keep a concrete SB_CONFIG_* reference for sysbuild compliance checks.
  if(SB_CONFIG_WARN_DEPRECATED)
    message(STATUS "Sysbuild deprecated warnings are enabled")
  endif()
else()
  message(FATAL_ERROR "Unknown platform ${TARGET_PLATFORM}, bail out!")
endif()

# Default to Debug build type if not specified (non-Zephyr only).
if(NOT TARGET_PLATFORM_ZEPHYR AND
  NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "No build type specified. "
    "Setting CMAKE_BUILD_TYPE=Debug by default.")
  set(CMAKE_BUILD_TYPE Debug CACHE STRING
    "Build type (default Debug)" FORCE)

  # Optional: Provide a list of valid build types
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release")
endif()

# Check if LuDLC is being built as part of an another project
# (via add_subdirectory)
if(NOT DEFINED LUDLC_AS_SUBPROJECT)
  set(LUDLC_AS_SUBPROJECT ON)
  if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(LUDLC_AS_SUBPROJECT OFF)
  endif()
endif()

# Set the project top directory.
set(LUDLC_TOP_DIR ${CMAKE_CURRENT_SOURCE_DIR})

option(DISABLE_PACKAGE_CONFIG_AND_INSTALL
  "Disable package configuration, target export and installation"
  ${LUDLC_AS_SUBPROJECT})

# If this is the root project add longer list of available
# CMAKE_BUILD_TYPE values (non-Zephyr only).
if(NOT TARGET_PLATFORM_ZEPHYR AND NOT LUDLC_AS_SUBPROJECT)
  set(CMAKE_BUILD_TYPE ${CMAKE_BUILD_TYPE}
  CACHE STRING "Choose the type of build: Debug Release"
  FORCE)
endif()

find_program(CCACHE_FOUND ccache)
if(CCACHE_FOUND)
  set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE ccache)
  set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK ccache)
endif(CCACHE_FOUND)

add_subdirectory(src)
if(NOT DEFINED DISABLE_PACKAGE_CONFIG_AND_INSTALL)
  add_subdirectory(doc)
endif()

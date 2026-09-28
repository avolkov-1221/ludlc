# SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
#
# @file CMakeLists.txt
#
# @brief Top-level CMake build of LuDLC.
#
# @copyright
# Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
#
# @details
# It selects the platform the library is built for: TARGET_PLATFORM if given,
# else the one LUDLC_PLATFORM_DIR names, else Zephyr where ZEPHYR_BASE is set,
# else POSIX. Every platform but Zephyr builds an ordinary library, versioned
# from the VERSION file; Zephyr builds a Zephyr library instead. The sources are
# added from src/. See doc/PORTING.md for a port kept outside this tree.

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

# A port may live outside this tree. LUDLC_PLATFORM_DIR gives its path and,
# when TARGET_PLATFORM is not set, its name. See doc/PORTING.md for the
# contents such a directory must have.
if(DEFINED LUDLC_PLATFORM_DIR AND NOT DEFINED TARGET_PLATFORM)
  get_filename_component(TARGET_PLATFORM "${LUDLC_PLATFORM_DIR}" NAME)
endif()

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

# The port's directory: the one given above, or the in-tree directory with
# that name.
if(DEFINED LUDLC_PLATFORM_DIR)
  get_filename_component(LUDLC_PLATFORM_DIR "${LUDLC_PLATFORM_DIR}"
    ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
  set(LUDLC_PLATFORM_EXTERNAL 1)
else()
  set(LUDLC_PLATFORM_DIR
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lib/platform/${TARGET_PLATFORM})
endif()

if(NOT EXISTS ${LUDLC_PLATFORM_DIR}/CMakeLists.txt)
  message(FATAL_ERROR
    "No port for '${TARGET_PLATFORM}': ${LUDLC_PLATFORM_DIR} holds no "
    "CMakeLists.txt. Pass -DLUDLC_PLATFORM_DIR=<dir> for a port kept "
    "outside this tree.")
endif()

message(STATUS "The selected platform is '${TARGET_PLATFORM}' "
  "(${LUDLC_PLATFORM_DIR})")

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
elseif(LUDLC_PLATFORM_EXTERNAL)
  # A port outside this tree. Its target is unknown here, so it is built as a
  # plain library and declares its own sources, include paths and link
  # dependencies in its CMakeLists.txt.
  message(STATUS "Building against an out-of-tree port")
else()
  message(FATAL_ERROR "Unknown platform ${TARGET_PLATFORM}, bail out!")
endif()

# Every port except Zephyr is built as an ordinary static or shared library.
# Zephyr uses zephyr_library() instead.
if(NOT TARGET_PLATFORM_ZEPHYR)
  set(LUDLC_BUILD_LIBRARY 1)
endif()

# Set for the selected port, so that a port's CMakeLists.txt can test for
# itself as the in-tree ones do.
string(TOUPPER "${TARGET_PLATFORM}" ludlc_platform_upper)
set(TARGET_PLATFORM_${ludlc_platform_upper} 1)
unset(ludlc_platform_upper)

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

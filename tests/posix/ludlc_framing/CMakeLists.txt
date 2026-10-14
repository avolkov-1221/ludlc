# SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only)
#
# @file CMakeLists.txt
#
# @brief Builds the framing test suite in its variants.
#
# @copyright
# Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
#
# @details
# The three framings are compiled with the test, once per width of
# ludlc_payload_size_t and twice more for big payloads: the largest an 8-bit
# size allows, and a megabyte. The comment below says why they are compiled
# here rather than linked from the library.

# The framings live in source files rather than headers, so this compiles them
# along with the test instead of linking the library: the library is built once
# for the platform, while these variants build the same code against several
# widths of ludlc_payload_size_t. All three are included, since this suite is
# about a link picking its own framing and the three coexisting. They call into
# no other translation unit, so nothing else has to follow.
#
# All three are enabled below. There is no default: ludlc_framing.h takes the
# framings from ludlc_autoconf.h, which names the ones the library was built
# with and may be fewer than this suite needs.
#
# name;payload bits;frame bits;max payload
set(variants
  "p8:8:16:32"
  "p16:16:16:32"
  "p32:32:16:32"
  "p8_full:8:16:251"
  "p32_1m:32:32:1048576"
)

foreach(variant IN LISTS variants)
  string(REPLACE ":" ";" fields "${variant}")
  list(GET fields 0 tag)
  list(GET fields 1 bits)
  list(GET fields 2 frame_bits)
  list(GET fields 3 payload)

  set(test_name ludlc_framing_${tag})

  add_executable(${test_name}
    ludlc_framing_test.c
    ${LUDLC_TOP_DIR}/src/lib/ludlc_framing.c
    ${LUDLC_TOP_DIR}/src/lib/ludlc_hdlc.c
    ${LUDLC_TOP_DIR}/src/lib/ludlc_cobs.c
    ${LUDLC_TOP_DIR}/src/lib/ludlc_raw.c)

  target_compile_definitions(${test_name} PRIVATE
    CONFIG_LUDLC_PAYLOAD_BITS=${bits}
    CONFIG_LUDLC_FRAME_BITS=${frame_bits}
    CONFIG_LUDLC_MAX_PAYLOAD_SIZE=${payload}U
    CONFIG_LUDLC_FRAMING_HDLC=1
    CONFIG_LUDLC_FRAMING_COBS=1
    CONFIG_LUDLC_FRAMING_RAW=1)

  target_link_libraries(${test_name} PRIVATE ludlc_includes)

  target_compile_options(${test_name} PRIVATE -Wall -Wextra)

  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

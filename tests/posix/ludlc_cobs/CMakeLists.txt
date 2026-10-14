# SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only)
#
# @file CMakeLists.txt
#
# @brief Builds the COBS test suite in its variants.
#
# @copyright
# Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
#
# @details
# One variant per width of ludlc_payload_size_t, and two more at either end of
# what a payload may be. The comment below says why each one is there.

# Each variant compiles ludlc_cobs.c for itself rather than linking the
# library, since what varies below is what the library was built with. Nothing
# else is needed, as the framing calls into no other translation unit.
#
# COBS alone, so LUDLC_FRAMING_COUNT is one and every call reaches
# ludlc_cobs.c directly rather than through an ops table, as in a port that
# carries one framing only.
#
# Built once per width of ludlc_payload_size_t, and then at either end of what
# a payload may be. Each variant sets its own widths and framing below, rather
# than take them from ludlc_autoconf.h, which says nothing about the widths and
# names the framings the library was built with.
#
# The narrow widths promote to int in every comparison and hide sign and
# overflow mistakes that the wide ones report, so a single build would not have
# found the ones already fixed here. The two named variants are about size
# rather than width:
#
#   p8_full  The largest payload an 8-bit size type allows, whose frame
#            therefore does not fit in one - the framing counts a frame in
#            size_t for exactly this reason, and this build would not have
#            compiled before it did. 251 is 255 less the packet header, which
#            is the one thing that has to share the type; see ludlc_proto.h.
#
#   p32_1m   A megabyte. Nothing is gathered on a stack on the way to the wire,
#            and this is what says so: it would need a megabyte of stack in
#            ludlc_cobs_encode() alone if anything still were. Frame bits must
#            be wide enough for the stuffed length (SPI announce, encoded_max).
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

  set(test_name ludlc_cobs_${tag})

  add_executable(${test_name}
    ludlc_cobs_test.c
    ${LUDLC_TOP_DIR}/src/lib/ludlc_cobs.c)

  target_compile_definitions(${test_name} PRIVATE
    CONFIG_LUDLC_PAYLOAD_BITS=${bits}
    CONFIG_LUDLC_FRAME_BITS=${frame_bits}
    CONFIG_LUDLC_MAX_PAYLOAD_SIZE=${payload}U
    CONFIG_LUDLC_FRAMING_COBS=1
    CONFIG_LUDLC_FRAMING_HDLC=0
    CONFIG_LUDLC_FRAMING_RAW=0)

  target_link_libraries(${test_name} PRIVATE ludlc_includes)

  # Warnings are half of what building this several times is for,
  # -Wsign-compare in particular. Not -Wconversion: these functions return a
  # size or a negative error in one value by design, and it has nothing else to
  # say here.
  target_compile_options(${test_name} PRIVATE -Wall -Wextra)

  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

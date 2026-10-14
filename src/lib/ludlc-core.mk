# SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only)
#
# @file ludlc-core.mk
#
# @brief The core translation units, for a make-based port.
#
# @copyright
# Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
#
# @details
# Every make-based build in the tree takes the list from here, so a port
# outside the tree keeps building when the core gains or loses a file.
#
# Include it with LUDLC_LIB_DIR pointing at src/lib:
#
#	LUDLC_LIB_DIR := $(LUDLC_DIR)/src/lib
#	include $(LUDLC_LIB_DIR)/ludlc-core.mk
#
# LUDLC_CORE_SRCS then holds the paths to compile and LUDLC_CORE_NAMES the
# bare file names, which Kbuild needs to name its objects. Neither lists
# anything from platform/: a port declares its own files.

# The protocol, and whichever framings the port asked for.
LUDLC_CORE_NAMES := ludlc.c

# A codec file has no #ifdef on its own config, so a framing is left out of
# the image by dropping its file here. Set each variable to y (Kconfig) or 1
# for the framings the port needs, and pass the same setting to the compiler
# as -DCONFIG_LUDLC_FRAMING_*, which is what the headers read. A port that
# selects none fails to build: the core needs at least one framing.
ludlc-framing-on = $(filter-out 0 n N no,$(1))

LUDLC_FRAMINGS_ON :=						\
	$(call ludlc-framing-on,$(CONFIG_LUDLC_FRAMING_HDLC))	\
	$(call ludlc-framing-on,$(CONFIG_LUDLC_FRAMING_COBS))	\
	$(call ludlc-framing-on,$(CONFIG_LUDLC_FRAMING_RAW))

LUDLC_CORE_NAMES += \
	$(if $(call ludlc-framing-on,$(CONFIG_LUDLC_FRAMING_HDLC)),ludlc_hdlc.c)
LUDLC_CORE_NAMES += \
	$(if $(call ludlc-framing-on,$(CONFIG_LUDLC_FRAMING_COBS)),ludlc_cobs.c)
LUDLC_CORE_NAMES += \
	$(if $(call ludlc-framing-on,$(CONFIG_LUDLC_FRAMING_RAW)),ludlc_raw.c)

# ludlc_framing.c turns a name into one of the framings built, which a port
# carrying a single one has nothing to do with.
LUDLC_CORE_NAMES += $(if $(word 2,$(LUDLC_FRAMINGS_ON)),ludlc_framing.c)

LUDLC_CORE_SRCS := $(addprefix $(LUDLC_LIB_DIR)/,$(LUDLC_CORE_NAMES))

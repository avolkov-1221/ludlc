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
# anything from platform/: a port declares its own files. LUDLC_VERSION and
# LUDLC_VERSION_STRING hold the version of the tree, the latter with its
# suffix, and LUDLC_VERSION_CFLAGS passes the string to the code as the
# definition of the same name.

# This directory, for an includer that did not name it.
LUDLC_LIB_DIR ?= $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))

# The version, from the VERSION file at the top of the tree. Read with sed
# rather than included: the file is make syntax, but its names are those of
# the kernel's own Makefile, which a Kbuild include must not disturb. The
# suffix, EXTRAVERSION, is spelled as Zephyr spells it, rc1, and goes after
# a dash: 1.0.0-rc1.
LUDLC_VERSION_FILE := $(LUDLC_LIB_DIR)/../../VERSION
ludlc-version-field = $(strip $(shell \
	sed -n 's/^$(1)[[:space:]]*=[[:space:]]*//p' $(LUDLC_VERSION_FILE)))

LUDLC_VERSION_MAJOR := $(call ludlc-version-field,VERSION_MAJOR)
LUDLC_VERSION_MINOR := $(call ludlc-version-field,VERSION_MINOR)
LUDLC_PATCHLEVEL := $(call ludlc-version-field,PATCHLEVEL)
LUDLC_EXTRAVERSION := $(call ludlc-version-field,EXTRAVERSION)

LUDLC_VERSION := \
	$(LUDLC_VERSION_MAJOR).$(LUDLC_VERSION_MINOR).$(LUDLC_PATCHLEVEL)
LUDLC_VERSION_STRING := \
	$(LUDLC_VERSION)$(if $(LUDLC_EXTRAVERSION),-$(LUDLC_EXTRAVERSION))
LUDLC_VERSION_CFLAGS := -DLUDLC_VERSION_STRING=\"$(LUDLC_VERSION_STRING)\"

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

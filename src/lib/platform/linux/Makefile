# SPDX-License-Identifier: GPL-2.0-only
#
# @file Makefile
#
# @brief Kbuild fragment for the LuDLC Linux drivers.
#
# @copyright
# Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
#
# @details
# The top-level Makefile descends here via `obj-m += src/lib/platform/linux/`,
# so module composition and the per-directory build flags live in this file.

# In an out-of-tree (make M=) build the LuDLC Kconfig symbols are not part of
# the kernel .config, so define them here. Flip a value to n to drop the
# corresponding module from the build.
CONFIG_LUDLC_CORE       ?= m

# The checksum a link runs when neither the crc= module parameter, firmware nor
# configfs names one. Both ends of a link must agree on it, so change it only
# to match the peers. One of:
#
#   CCITT_PR0   CRC-16/KERMIT
#   X25_PR1     HDLC FCS-16, as X.25 and PPP send it
#   CRC16_PR0   CRC-16/ARC
#   CRC32_PR0   CRC-32, zero seed
#   NONE        no checksum
#
# Each but NONE needs its kernel CRC library, checked for below. A name outside
# the list would leave the wrong checksum on every link, so stop rather than
# let it through.
CONFIG_LUDLC_CRC_DFLT ?= CCITT_PR0

ifeq ($(filter $(CONFIG_LUDLC_CRC_DFLT),\
	CCITT_PR0 X25_PR1 CRC16_PR0 CRC32_PR0 NONE),)
$(error CONFIG_LUDLC_CRC_DFLT: no checksum called \
	"$(CONFIG_LUDLC_CRC_DFLT)")
endif

# Header search paths and the defines a user of these headers must match,
# shared with those users through ludlc.mk. $(KBUILD_EXTMOD) is the
# external-module root (the make M= directory).
LUDLC_SRC := $(KBUILD_EXTMOD)
include $(LUDLC_SRC)/ludlc.mk

ccflags-y += $(LUDLC_CCFLAGS)
ccflags-y += -DCONFIG_LUDLC_CRC_DFLT_$(CONFIG_LUDLC_CRC_DFLT)=1

# Core module: protocol core + software bus and module entry point
# (ludlc_bus.o, the replacement of other platforms ludlc_platform.c) + the
# checksums a link can run (ludlc_csum.o), which belong to the link rather
# than to whatever carries it and are therefore shared by every transport.
#
# The core objects are derived from ludlc-core.mk, which every build system in
# the tree reads, so that this list does not have to track the core's files.
# Kbuild needs them relative to this directory, two levels below src/lib.
include $(LUDLC_SRC)/src/lib/ludlc-core.mk

ludlc_core-y := $(addprefix ../../,$(LUDLC_CORE_NAMES:.c=.o))	\
		ludlc_bus.o					\
		ludlc_csum.o

obj-$(CONFIG_LUDLC_CORE) += ludlc_core.o

# No transport needs a CRC library of its own: each checksum on offer follows
# the library it is computed with, and the one below is only there for the
# default to fall back on. Our `select` lines are inert out of tree - nothing
# here parses the LuDLC Kconfig, so ask the target kernel plainly, rather than
# leave an undefined CRC table to modpost.
ludlc-crc-lib-CCITT_PR0 := CRC_CCITT
ludlc-crc-lib-X25_PR1   := CRC_CCITT
ludlc-crc-lib-CRC16_PR0 := CRC16
ludlc-crc-lib-CRC32_PR0 := CRC32
ludlc-crc-lib := $(ludlc-crc-lib-$(CONFIG_LUDLC_CRC_DFLT))

# What each module needs from the target kernel. A target that only deletes
# files is run without the kernel's .config, which would leave every CONFIG_
# above empty and every check below wrong, so ask only when something is
# actually being compiled. CONFIG_MODULES is set either way in that case, since
# these are modules. `filter m y` rather than "not n" for the same reason: a
# module that is not being built has nothing to require.
ifneq ($(CONFIG_MODULES),)

ifneq ($(filter m y,$(CONFIG_LUDLC_CORE)),)
ifneq ($(ludlc-crc-lib),)
ifeq ($(CONFIG_$(ludlc-crc-lib)),)
$(error LuDLC $(CONFIG_LUDLC_CRC_DFLT) needs \
	CONFIG_$(ludlc-crc-lib) (y or m) in the target kernel)
endif
endif
endif

endif

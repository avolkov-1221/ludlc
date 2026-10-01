# SPDX-License-Identifier: GPL-2.0-only
#
# @file ludlc.mk
#
# @brief Build settings shared by LuDLC and by out-of-tree modules that use it.
#
# @copyright
# Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
#
# @details
# Include it from a Kbuild file with LUDLC_SRC pointing at the LuDLC tree:
#
#	LUDLC_SRC ?= /path/to/ludlc
#	include $(LUDLC_SRC)/ludlc.mk
#	ccflags-y += $(LUDLC_CCFLAGS)
#
# The values below pick the width of ludlc_channel_t and ludlc_payload_size_t
# and the shape of struct ludlc_connection, so every module built against these
# headers has to be given the same ones. That is what this file is for: change
# a value here and both LuDLC and its users follow.

# Largest payload a packet may carry. Both ends of a link must agree on it too,
# so raise it only to match the peers.
CONFIG_LUDLC_MAX_PAYLOAD_SIZE ?= 32

CONFIG_LUDLC_PAYLOAD_BITS ?= 8
CONFIG_LUDLC_CHANNEL_BITS ?= 8

# A link here is worked on by whoever the kernel sends: the transport's receive
# path, the watchdog's work, a configfs write, a PM transition, and remove().
# The lock between them is a mutex, and a user callback has no business running
# under it, so the core must be the build that reports its confirmations after
# releasing it. Not a choice a command line gets to make, hence the override -
# and the core asks with #ifdef, so a 0 here would read as "yes" and only look
# like one.
override CONFIG_LUDLC_MT := 1

# The bus hosts a link per port, and each port names its own frame size, window,
# periods and channel tag from firmware or configfs. None of that can be a
# build-wide constant, so this port always reads the per-connection cfg: not a
# choice a command line gets to make, hence the override. For the same reason
# CONFIG_LUDLC_COMMON_CHAN_TAG is left out.
override CONFIG_LUDLC_MULTI_CONN := 1

# Framings compiled into the drivers. All three, because a port takes its
# framing from firmware or configfs at runtime and the driver offers the list
# to userspace. Set one to 0 to leave it out; at least one has to stay.
CONFIG_LUDLC_FRAMING_HDLC ?= 1
CONFIG_LUDLC_FRAMING_COBS ?= 1
CONFIG_LUDLC_FRAMING_RAW  ?= 1

# Framing objects preallocated per framing. None by default: which framings
# the ports end up running is not known until they are configured, so
# preallocating for all three would hold memory that mostly goes unused, and
# the kernel has a heap to take them from instead.
CONFIG_LUDLC_STATIC_FRAMING_NUM ?= 0

LUDLC_CCFLAGS :=							\
	-I$(LUDLC_SRC)/src/include					\
	-I$(LUDLC_SRC)/src/lib/include					\
	-I$(LUDLC_SRC)/src/lib/platform/linux				\
	-DCONFIG_LUDLC_MT=$(CONFIG_LUDLC_MT)				\
	-DCONFIG_LUDLC_MULTI_CONN=$(CONFIG_LUDLC_MULTI_CONN)		\
	-DCONFIG_LUDLC_MAX_PAYLOAD_SIZE=$(CONFIG_LUDLC_MAX_PAYLOAD_SIZE) \
	-DCONFIG_LUDLC_PAYLOAD_BITS=$(CONFIG_LUDLC_PAYLOAD_BITS)	\
	-DCONFIG_LUDLC_CHANNEL_BITS=$(CONFIG_LUDLC_CHANNEL_BITS)	\
	-DCONFIG_LUDLC_FRAMING_HDLC=$(CONFIG_LUDLC_FRAMING_HDLC)	\
	-DCONFIG_LUDLC_FRAMING_COBS=$(CONFIG_LUDLC_FRAMING_COBS)	\
	-DCONFIG_LUDLC_FRAMING_RAW=$(CONFIG_LUDLC_FRAMING_RAW)		\
	-DCONFIG_LUDLC_STATIC_FRAMING_NUM=$(CONFIG_LUDLC_STATIC_FRAMING_NUM)

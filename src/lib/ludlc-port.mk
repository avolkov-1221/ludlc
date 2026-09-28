# SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
#
# @file ludlc-port.mk
#
# @brief What a make-based port needs from LuDLC: the core sources and the
# three include paths.
#
# @copyright
# Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
#
# @details
# Works the same for a port in platform/ and for one outside this tree.
#
# Include it with LUDLC_DIR pointing at the LuDLC checkout and
# LUDLC_PORT_DIR at the directory holding ludlc_platform.h:
#
#	LUDLC_DIR := ../../ludlc
#	LUDLC_PORT_DIR := $(CURDIR)/ludlc_port
#	include $(LUDLC_DIR)/src/lib/ludlc-port.mk
#
#	SRCS += $(LUDLC_SRCS)
#	CFLAGS += $(LUDLC_INCLUDES) $(LUDLC_CONFIG)
#
# LUDLC_SRCS then holds the core sources plus the port's own LUDLC_PORT_SRCS.
# See doc/PORTING.md for the rest of the contract, including how to keep the
# core's objects out of the LuDLC checkout.

LUDLC_LIB_DIR := $(LUDLC_DIR)/src/lib

ifeq ($(wildcard $(LUDLC_LIB_DIR)/ludlc.c),)
$(error LUDLC_DIR ($(LUDLC_DIR)) is not a LuDLC checkout)
endif

ifeq ($(LUDLC_PORT_DIR),)
$(error LUDLC_PORT_DIR is unset: name the directory holding ludlc_platform.h)
endif

ifeq ($(wildcard $(LUDLC_PORT_DIR)/ludlc_platform.h),)
$(error LUDLC_PORT_DIR ($(LUDLC_PORT_DIR)) holds no ludlc_platform.h)
endif

include $(LUDLC_LIB_DIR)/ludlc-core.mk

LUDLC_SRCS := $(LUDLC_CORE_SRCS) $(LUDLC_PORT_SRCS)

# The port's directory comes first: the core includes ludlc_platform.h,
# ludlc_platform_args.h and ludlc_logger.h by those names, and the port
# provides them for this target.
LUDLC_INCLUDES := \
	-I$(LUDLC_PORT_DIR) \
	-I$(LUDLC_LIB_DIR)/include \
	-I$(LUDLC_DIR)/src/include

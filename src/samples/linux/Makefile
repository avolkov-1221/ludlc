# SPDX-License-Identifier: GPL-2.0-only
#
# @file Makefile
#
# @brief Kbuild fragment for the Linux echo sample.
#
# @copyright
# Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
#
# @details
# Echo responder, a client of the LuDLC bus. The top Makefile descends here
# the same way it descends into the transports.

LUDLC_SRC := $(KBUILD_EXTMOD)
include $(LUDLC_SRC)/ludlc.mk

ccflags-y += $(LUDLC_CCFLAGS)

obj-m += ludlc_echo.o

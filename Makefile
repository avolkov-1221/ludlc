# SPDX-License-Identifier: GPL-2.0-only
#
# @file Makefile
#
# @brief Builds the LuDLC Linux drivers as external kernel modules.
#
# @copyright
# Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
#
# @details
# Run here, it calls the build of the kernel in KDIR with M= set to this tree.
# Kbuild then descends into src/lib/platform/linux/, where the modules are
# composed, and into src/samples/linux/ for the echo client. The modules go to
# OUT. The targets are all, debug, modules-warn, modules-sparse and clean.

ifneq ($(KERNELRELEASE),)

# Descend into the Linux platform driver directory. Module composition, the
# enable/disable switches and the per-directory build flags all live in
# src/lib/platform/linux/Makefile. The echo sample is a bus client and is
# built beside the transports.
obj-m += src/lib/platform/linux/
obj-m += src/samples/linux/

else

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)
OUT  ?= $(PWD)/out/modules
V    ?= 0

KBUILD_MAKE = $(MAKE) -C $(KDIR) M=$(PWD) MO="$(OUT)" V=$(V)

DEBUG_KCFLAGS = -g3 -Og -DDEBUG -fno-omit-frame-pointer

all modules:
	@mkdir -p "$(OUT)"
	$(KBUILD_MAKE) modules

debug modules-debug:
	@mkdir -p "$(OUT)"
	$(KBUILD_MAKE) KCFLAGS="$(DEBUG_KCFLAGS)" modules

modules-warn:
	@mkdir -p "$(OUT)"
	$(KBUILD_MAKE) W=1 modules

modules-sparse:
	@mkdir -p "$(OUT)"
	$(KBUILD_MAKE) C=2 CHECK=sparse modules

clean:
	$(KBUILD_MAKE) clean
	$(RM) -r "$(OUT)"

endif

.PHONY: all modules debug modules-debug modules-warn modules-sparse clean

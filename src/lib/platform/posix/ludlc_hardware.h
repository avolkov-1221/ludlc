// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/*
 * ludlc_hardware.h
 *
 * LuDLC Posix hardware related definitions and declararions
 *
 * Copyright (C) 2025 Andrey VOLKOV <andrey@volkov.fr> and LuDLC Contributors
 *
 * This file is licensed under either the Apache License, Version 2.0,
 * or the GNU General Public License, version 2 or (at your option)
 * any later version.
 */

#ifndef __LUDLC_HARDWARE_H__
#define __LUDLC_HARDWARE_H__

typedef struct ludlc_platform_args {
	char *port;
	unsigned long baudrate;
	unsigned int  parity;
} ludlc_platform_args_t;

#endif /* __LUDLC_HARDWARE_H__ */

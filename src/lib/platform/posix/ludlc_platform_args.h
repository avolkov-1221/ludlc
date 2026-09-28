// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file posix/ludlc_platform_args.h
 *
 * @brief LuDLC POSIX platform arguments: the TTY, its line settings and the
 * checksum a link on it runs.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * A caller fills them in, or starts from
 * ludlc_default_serial_platform_args(), and passes them to
 * ludlc_serial_connection_create(). The checksum is borrowed and must
 * outlive the connection.
 */

#ifndef __LUDLC_PLATFORM_ARGS_H__
#define __LUDLC_PLATFORM_ARGS_H__

struct ludlc_csum;

typedef struct ludlc_platform_args {
	char *port;
	unsigned long baudrate;
	unsigned int  parity;
	/**
	 * @brief Checksum the link is to run. **Mandatory.**
	 *
	 * Both ends of the wire must agree on it. Borrowed, and must outlive
	 * the connection.
	 */
	const struct ludlc_csum *csum;

} ludlc_platform_args_t;

#endif /* __LUDLC_PLATFORM_ARGS_H__ */

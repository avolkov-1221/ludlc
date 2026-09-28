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
 * ludlc_serial_connection_create(). The checksum and the framing are
 * borrowed and must outlive the connection; the connection settings are
 * copied when it is created.
 */

#ifndef __LUDLC_PLATFORM_ARGS_H__
#define __LUDLC_PLATFORM_ARGS_H__

#include <ludlc_proto.h>

struct ludlc_csum;
struct ludlc_framing;

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

	/**
	 * @brief How this link delimits its frames.
	 *
	 * NULL means @ref ludlc_framing_hdlc. Otherwise one of the
	 * ludlc_framing_* descriptors that delimits its own frames: a TTY
	 * carries a byte stream and cannot mark the end of one. Borrowed, and
	 * must outlive the connection.
	 */
	const struct ludlc_framing *framing;

	/**
	 * @brief What to open the link with: its clock and its settings.
	 *
	 * Copied into the connection at create. Without
	 * @ref CONFIG_LUDLC_MULTI_CONN only the clock is set here, the rest
	 * coming from the CONFIG_* settings; the checksum is taken from
	 * @c csum above rather than from this field.
	 */
	struct ludlc_conn_cfg cfg;
} ludlc_platform_args_t;

/**
 * @brief The cfg @p args carries, or the defaults when @p args is NULL.
 *
 * Returned as given. The checksum is not part of it: the transport holds
 * that.
 */
static inline const struct ludlc_conn_cfg *
ludlc_args_conn_cfg(const ludlc_platform_args_t *args)
{
	return args ? &args->cfg : &ludlc_conn_cfg_default;
}

#endif /* __LUDLC_PLATFORM_ARGS_H__ */

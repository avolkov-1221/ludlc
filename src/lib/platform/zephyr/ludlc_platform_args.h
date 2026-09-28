// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file zephyr/ludlc_platform_args.h
 *
 * @brief LuDLC Zephyr platform arguments: the device a link runs on, the
 * checksum it runs, and the extra settings its transport needs.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * A caller fills them in, or starts from the default arguments of its
 * transport, and passes them to its create call. The checksum and the
 * framing are borrowed and must outlive the connection; the connection
 * settings are copied when it is created.
 */

#ifndef __LUDLC_PLATFORM_ARGS_H__
#define __LUDLC_PLATFORM_ARGS_H__

#include <zephyr/device.h>
#include <ludlc_proto.h>

#if defined(CONFIG_LUDLC_SPI_CONN)
#include <zephyr/drivers/gpio.h>
#endif

struct ludlc_csum;
struct ludlc_framing;

typedef struct ludlc_platform_args {
	/**
	 * @brief The controller a link runs on: a UART or an SPI peripheral.
	 *
	 * Which of the two is required depends on the create call this is
	 * passed to.
	 */
	const struct device *dev;
	/**
	 * @brief Checksum the link runs, or NULL for none.
	 *
	 * Both ends must agree on it. With NULL no checksum is computed, sent
	 * or checked. Borrowed, and must outlive the connection.
	 */
	const struct ludlc_csum *csum;

	/**
	 * @brief How this link delimits its frames.
	 *
	 * NULL means @ref ludlc_framing_hdlc on UART, where a frame in a byte
	 * stream must mark its own end, and @ref ludlc_framing_raw on SPI,
	 * which announces the length of every frame instead. Borrowed, and
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

#if defined(CONFIG_LUDLC_SPI_CONN)
	/** @brief Settings used only by a link over SPI. */
	struct {
		/**
		 * @brief Which chip select this peripheral answers on.
		 *
		 * Zero unless the controller drives more than one chip
		 * select and this link is not on the first.
		 */
		uint16_t slave;

		/**
		 * @brief Line this end raises to ask for an exchange.
		 *
		 * Nothing moves on an SPI bus until the controller clocks it,
		 * so a peripheral with a frame ready cannot start a transfer
		 * and signals on this line instead. Leave the port NULL where
		 * the board has no such line: the controller then polls, and
		 * a frame waits for the next poll.
		 */
		struct gpio_dt_spec data_ready;
	} spi;
#endif
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

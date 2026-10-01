/* SPDX-License-Identifier: GPL-2.0-only */
/**
 * @file linux/ludlc_platform_args.h
 *
 * @brief LuDLC Linux platform arguments: the checksum a link runs, and the
 * port it is built on.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * A transport gathers these from firmware (DT/ACPI) and configfs before
 * calling its connection_create(). The checksum sits outside the union because
 * both ends of a link must agree on it whatever carries the frames, so it
 * describes the link rather than the port; everything below it is the port.
 */

#ifndef _LUDLC_PLATFORM_ARGS_H_
#define _LUDLC_PLATFORM_ARGS_H_

#include <linux/types.h>
#include <linux/string.h>

#if IS_ENABLED(CONFIG_SERIAL_DEV_BUS)
#include <linux/serdev.h>
#endif

#if IS_ENABLED(CONFIG_SPI_MASTER)
#include <linux/spi/spi.h>
#endif

#include <ludlc_proto.h>

struct ludlc_csum;
struct ludlc_framing;

typedef struct ludlc_platform_args {
	/**
	 * @brief The checksum this link runs.
	 *
	 * NULL for none, when no checksum is computed, sent or checked.
	 * Otherwise the descriptor itself: one of the ludlc_crc* the core
	 * provides, or the caller's own. Borrowed, not owned: it only has to
	 * outlive the call it is passed to, which takes its own reference for
	 * as long as the link uses it. A caller that looked up a name with
	 * ludlc_crc_find() holds a reference until then and drops it with
	 * ludlc_args_csum_put(). The system default is ludlc_csum_default().
	 * It sits outside the union because a link runs one checksum whatever
	 * carries it.
	 */
	const struct ludlc_csum *csum;

	/**
	 * @brief How this link delimits its frames.
	 *
	 * NULL selects @ref ludlc_framing_hdlc on a transport that hands over a
	 * stream of octets, since a frame in a stream must mark where it ends,
	 * and @ref ludlc_framing_raw over SPI, which announces the length of
	 * every frame. Otherwise one of the ludlc_framing_* descriptors: a
	 * transport refuses the ones it cannot carry rather than silently
	 * running another.
	 */
	const struct ludlc_framing *framing;

	/**
	 * @brief Protocol settings for this link.
	 *
	 * Copied into the connection at create. Window, payload size, ping
	 * period, default TTL and channel tag. Zero @c max_payload asks for
	 * the build max.
	 */
	struct ludlc_conn_cfg cfg;

	union {
#if IS_ENABLED(CONFIG_SERIAL_DEV_BUS)
		struct {
			struct serdev_device *dev;
			u32 baudrate;
			bool flow_control;
			enum serdev_parity parity;
		} serdev;
#endif

#if IS_ENABLED(CONFIG_SPI_MASTER)
		struct {
			struct spi_device *dev;

			/**
			 * @brief Gap between the two halves of an exchange,
			 * in microseconds.
			 *
			 * The peer learns how long the second transfer will be
			 * from the first one, and has until the next chip
			 * select to point its controller at a buffer of that
			 * length. How long that takes is a property of the
			 * peer's firmware, which states it.
			 */
			u32 turnaround_us;

			/**
			 * @brief How often to ask a peer that cannot ask us,
			 * in milliseconds, or 0 for never.
			 *
			 * Only used where no data-ready line was described:
			 * with one, an exchange happens when there is a reason
			 * for it.
			 */
			u32 poll_interval_ms;
		} spi;
#endif
	};
} ludlc_platform_args_t;

/**
 * @brief The cfg @p args carries, or the defaults where the caller brought no
 * args at all, as when a port comes up on firmware alone.
 *
 * Returned as given. The checksum is not part of it: the transport holds
 * that, through ludlc_link_csum_set().
 */
static inline const struct ludlc_conn_cfg *
ludlc_args_conn_cfg(const ludlc_platform_args_t *args)
{
	return args ? &args->cfg : &ludlc_conn_cfg_default;
}

#endif /* _LUDLC_PLATFORM_ARGS_H_ */

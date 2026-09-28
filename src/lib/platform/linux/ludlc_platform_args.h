/* SPDX-License-Identifier: GPL-2.0-only */
/**
 * @file linux/ludlc_platform_args.h
 *
 * @brief LuDLC Linux platform arguments: the checksum, the framing and the
 * settings a link runs on.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * A transport gathers these from firmware (DT/ACPI) before calling its
 * connection_create().
 */

#ifndef _LUDLC_PLATFORM_ARGS_H_
#define _LUDLC_PLATFORM_ARGS_H_

#include <linux/types.h>
#include <linux/string.h>

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
	 */
	const struct ludlc_csum *csum;

	/**
	 * @brief How this link delimits its frames.
	 *
	 * NULL selects @ref ludlc_framing_hdlc, since a frame in a stream of
	 * octets must mark where it ends. Otherwise one of the ludlc_framing_*
	 * descriptors: a transport refuses the ones it cannot carry rather
	 * than silently running another.
	 */
	const struct ludlc_framing *framing;

	/**
	 * @brief Protocol settings for this link.
	 *
	 * Copied into the connection at create. Window, payload size, ping
	 * period and default TTL. Zero @c max_payload asks for the build max.
	 */
	struct ludlc_conn_cfg cfg;
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

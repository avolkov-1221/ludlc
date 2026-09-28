// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only)
/**
 * @file ludlc_cobs.h
 *
 * @brief The stuffing: its delimiter, the room a stuffed frame needs, and its
 * entry points.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * The state machines are in ludlc_cobs.c, reached through
 * @ref ludlc_framing_cobs_ops. Their prototypes come from
 * @ref LUDLC_DECLARE_FRAMING, as every framing's do.
 */

#ifndef __LUDLC_COBS_H__
#define __LUDLC_COBS_H__

#ifdef __KERNEL__
#include <linux/build_bug.h>
#include <linux/types.h>
#else
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#endif

/* For the packet types and the counters a rejected frame is counted in. */
#include <ludlc_private.h>

/**
 * @def LUDLC_COBS_DELIMITER
 * @brief The octet that ends a frame, and the only value an encoded frame
 * never contains.
 *
 * Any value works, since the codec removes whichever is set here. Zero is the
 * default because an idle line and transfer padding read as zero too, so
 * neither starts a frame.
 */
#ifdef CONFIG_LUDLC_COBS_DELIMITER
#define LUDLC_COBS_DELIMITER	CONFIG_LUDLC_COBS_DELIMITER
#else
#define LUDLC_COBS_DELIMITER	0x00
#endif

/**
 * @def LUDLC_COBS_BLOCK_MAX
 * @brief The longest run without a delimiter one code octet can describe.
 *
 * A code octet counts itself and the octets up to the next delimiter, so 0xff
 * covers 254 of them, after which the encoder opens a new group.
 */
#define LUDLC_COBS_BLOCK_MAX	254

/**
 * @def LUDLC_COBS_ENCODED_MAX
 * @brief Room an encoded frame of @p n octets needs, delimiter included.
 *
 * One code octet to start, one more per full run after that, and the
 * delimiter. An upper bound, not the exact length.
 */
#define LUDLC_COBS_ENCODED_MAX(n)					\
	((n) + ((n) / LUDLC_COBS_BLOCK_MAX) + 2)

/**
 * @def LUDLC_COBS_FRAME_MAX
 * @brief Worst-case encoded length for the largest frame this build allows on
 * a checksum @p crc_sz octets wide.
 *
 * The link sets the frame size, at most @ref LUDLC_MAX_FRAME_SIZE for that
 * checksum, and COBS only stuffs it. Streaming transports do not allocate
 * this for TX; SPI uses @ref ludlc_cobs_frame_sz as the per-link announce cap.
 */
#define LUDLC_COBS_FRAME_MAX(crc_sz)					\
	LUDLC_COBS_ENCODED_MAX(LUDLC_MAX_FRAME_SIZE(crc_sz))

/*
 * Frame sizes are carried in ludlc_frame_size_t, stuffing included, so the
 * type must hold the largest stuffed frame this build can produce: the one
 * using the widest checksum it can compute.
 */
static_assert(LUDLC_COBS_FRAME_MAX(LUDLC_MAX_CSUM_SIZE) <=
	      (ludlc_frame_size_t)-1,
	      "CONFIG_LUDLC_FRAME_BITS is too narrow for a stuffed frame");

/**
 * @brief Room this link's largest frame needs once encoded.
 *
 * Uses the payload the link was opened with rather than the build's, so a
 * link that asked for less does not send or accept more.
 *
 * @param conn The connection.
 * @param csum_alg Checksum this link runs.
 * @return Octets, delimiter included, at most @ref LUDLC_COBS_FRAME_MAX.
 */
static inline ludlc_frame_size_t
ludlc_cobs_frame_sz(const struct ludlc_connection *conn,
		    const struct ludlc_csum *csum_alg)
{
	return (ludlc_frame_size_t)
		LUDLC_COBS_ENCODED_MAX(ludlc_conn_frame_sz(conn, csum_alg));
}

/* The encoder and decoder state is private to ludlc_cobs.c. A link gets
 * one through create() below and passes it to every other operation.
 */
LUDLC_DECLARE_FRAMING(cobs)

#endif /* __LUDLC_COBS_H__ */

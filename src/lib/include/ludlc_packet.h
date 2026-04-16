// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file ludlc_packet.h
 *
 * @brief LuDLC protocol packet definitions.
 *
 * This file defines the structure of LuDLC packets, including the headers
 * for data and ping packets, as well as flags and masks used for
 * sequence numbers and flow control.
 *
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * This file is licensed under either the Apache License, Version 2.0,
 * or the GNU General Public License, version 2 or (at your option)
 * any later version.
 */

#ifndef __LUDLC_PACKET_H__
#define __LUDLC_PACKET_H__

#include <ludlc_proto.h>

/**
 * @struct ludlc_ping_packet
 * @brief Header structure for a PING packet.
 *
 * This structure defines the minimal packet format used for keep-alives (PINGs)
 * and acknowledgments (ACKs/NAKs). It contains only the sequence and
 * acknowledgment numbers.
 */
typedef struct ludlc_ping_packet {
	/**!< Transmit ID. Contains the sender's sequence number.
	 * The highest bit may be set for @ref LUDLC_PING_FLAG. */
	ludlc_id_t tx_id;
	/**!< Acknowledgment ID. Contains the last received ID from the peer.
	 * The highest bit may be set for @ref LUDLC_NAK_FLAG. */
	ludlc_id_t ack_id;
} ludlc_ping_packet_t;

/**
 * @struct ludlc_packet_hdr
 * @brief Header structure for a standard data packet.
 *
 * This structure includes the PING packet header (for sequence/ack numbers)
 * and adds the destination channel for the payload data.
 */
typedef struct ludlc_packet_hdr {
	/**< Sequence and acknowledgment numbers. */
	struct ludlc_ping_packet id;
	/**!< Channel for the payload. */
	ludlc_channel_t 	 chan;
} ludlc_packet_hdr_t;

/**
 * @struct ludlc_packet
 * @brief Represents a full LuDLC packet with a flexible payload.
 *
 * This structure is used to represent a complete packet, with the header
 * followed by a flexible array member for the payload data.
 *
 * @note This is often used for casting a received data buffer.
 */
typedef struct ludlc_packet {
	/**< The packet header. */
	ludlc_packet_hdr_t	hdr;
	/**< The payload data. */
	uint8_t 		payload[ /*data_len */ ];
} ludlc_packet_t;

/**
 * @def LUDLC_MAX_TX_ID
 * @brief The maximum value for a transmit ID, plus one.
 *
 * Reserves the highest bit of the `ludlc_id_t` type for internal flags
 * (PING and NAK). This ensures `LUDLC_MAX_TX_ID` is always a power of 2,
 * representing the total number of available sequence numbers.
 */
#define LUDLC_MAX_TX_ID		((((ludlc_id_t)-1) >> 1) + 1U)

/**
 * @def LUDLC_ID_MASK
 * @brief Bitmask to isolate the sequence number part of an ID.
 *
 * This mask is `LUDLC_MAX_TX_ID - 1`, which effectively masks off the
 * highest bit, leaving only the sequence number.
 */
#define LUDLC_ID_MASK		(LUDLC_MAX_TX_ID - 1)

/**
 * @def LUDLC_NAK_FLAG
 * @brief Flag set in the `ack_id` field to signal a Negative Acknowledgment.
 *
 * This flag indicates a request to resend all packets starting
 * from `(ack_id & LUDLC_ID_MASK) + 1`. It uses the reserved highest bit.
 */
#define LUDLC_NAK_FLAG		LUDLC_MAX_TX_ID

/**
 * @def LUDLC_PING_FLAG
 * @brief Flag set in the `tx_id` field to mark the packet as a PING.
 *
 * A PING packet has no payload and is used for keep-alives and to
 * transmit standalone ACKs/NAKs. It uses the reserved highest bit.
 */
#define LUDLC_PING_FLAG		LUDLC_MAX_TX_ID

#endif /* __LUDLC_PACKET_H__ */

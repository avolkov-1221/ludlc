/* SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only) */
/**
 * @file ludlc_packet.h
 *
 * @brief LuDLC protocol packet definitions.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
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
	/** Transmit ID. Contains the sender's sequence number.
	 * The highest bit may be set for @ref LUDLC_PING_FLAG. */
	ludlc_id_t tx_id;
	/** Acknowledgment ID. Contains the last received ID from the peer.
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
	/** Sequence and acknowledgment numbers. */
	struct ludlc_ping_packet id;
	/** Channel for the payload. */
	ludlc_channel_t 	 chan;
} ludlc_packet_hdr_t;

/**
 * @struct ludlc_packet
 * @brief Represents a full LuDLC packet with a flexible payload.
 *
 * This structure is used to represent a complete packet, with the header
 * followed by a flexible array member for the payload data.
 *
 * @note This is used for casting a received data buffer.
 */
typedef struct ludlc_packet {
	/** The packet header. */
	ludlc_packet_hdr_t	hdr;
	/** The payload data. */
	uint8_t 		payload[ /*data_len */ ];
} ludlc_packet_t;

/*
 * A packet is sized in ludlc_payload_size_t throughout the core - see
 * ludlc_receive() - so the payload must leave room in that type for the
 * header in front of it. Either widen CONFIG_LUDLC_PAYLOAD_BITS or reduce
 * the payload by sizeof(ludlc_packet_t). Checked here because that sizeof is
 * not available to the preprocessor.
 */
#if defined(BUILD_ASSERT)
BUILD_ASSERT(LUDLC_MAX_PACKET_SIZE <= (ludlc_payload_size_t)-1,
	     "CONFIG_LUDLC_MAX_PAYLOAD_SIZE leaves no room for the header");
#else
/* The same without static_assert, where the enum name is the only clue the
 * compiler prints.
 */
enum __CONFIG_LUDLC_MAX_PAYLOAD_SIZE_leaves_no_room_for_the_header__ {
	__CONFIG_LUDLC_MAX_PAYLOAD_SIZE_leaves_no_room_for_the_header__ =
		1 / !!(LUDLC_MAX_PACKET_SIZE <= (ludlc_payload_size_t)-1)
};
#endif

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
 * @brief Flag set in the `tx_id` field to mark the packet as unnumbered.
 *
 * The `tx_id` of such a packet holds the sequence number the sender will
 * use next, not one it is using now, so the receiver can still detect a
 * gap while the packet consumes no number of its own. Two kinds of packet
 * carry this flag: a PING, which is a bare @ref ludlc_ping_packet_t used
 * for keep-alives and standalone ACKs/NAKs, and an unnumbered data packet,
 * which has a full header and a payload. They are distinguished by length.
 */
#define LUDLC_PING_FLAG		LUDLC_MAX_TX_ID

#endif /* __LUDLC_PACKET_H__ */

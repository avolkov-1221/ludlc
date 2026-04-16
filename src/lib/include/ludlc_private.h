// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file ludlc_private.h
 *
 * @brief Internal definitions for the LuDLC protocol implementation.
 *
 * This header file contains private data structures, enumerations, and
 * macro definitions used internally by the LuDLC core logic. These are
 * not intended for use by the public API.
 *
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * This file is licensed under either the Apache License, Version 2.0,
 * or the GNU General Public License, version 2 or (at your option)
 * any later version.
 */

#ifndef __LUDLC_PRIVATE_H__
#define __LUDLC_PRIVATE_H__

#include <ludlc_platform_api.h>
#include <ludlc_packet.h>
#include <ludlc_logger.h>

/**
 * @enum ludlc_conn_state
 * @brief Defines the states of the LuDLC connection finite state machine (FSM).
 */
enum ludlc_conn_state
{
	/**< Initial and final state, no connection. */
	LUDLC_STATE_DISCONNECTED = 0,
	/**< Sent first handshake packet, awaiting reply. */
	LUDLC_STATE_WAIT_CONN_1  = 1,
	/**< Received first reply, sent second, awaiting final. */
	LUDLC_STATE_WAIT_CONN_2  = 2,
	/**< Handshake complete, connection established. */
	LUDLC_STATE_CONNECTED    = 3,
	/**< State to signal termination (not part of handshake). */
	LUDLC_STATE_TERMINATE    = 0xff,
};

/**
 * @struct ludlc_stats
 * @brief A structure to hold connection statistics (if enabled).
 */
struct ludlc_stats {
	uint32_t rx_packet;  /**< Count of valid received packets. */
	uint32_t tx_packet;  /**< Count of transmitted packets. */
	uint32_t bad_csum;   /**< Count of packets received with bad csum. */
	uint32_t jabber;   /**< Count of excessively large/malformed packets. */
	uint32_t on_conn;    /**< Connection established counter. */
	uint32_t on_disconn; /**< Connection lost/disconnected counter. */
	uint32_t dropped;    /**< Count of dropped packets. */
};

#define CONFIG_LUDLC_STATS
#ifdef CONFIG_LUDLC_STATS
/**
 * @def LUDLC_DECLARE_STATS(var)
 * @brief Declares the statistics structure within a connection object.
 */
#define LUDLC_DECLARE_STATS(var)	struct ludlc_stats	var
/**
 * @def LUDLC_INC_STATS(state, var)
 * @brief Increments a specific statistic counter.
 */
#define LUDLC_INC_STATS(state, var)	((state)->stats.var++)
#else
#define LUDLC_DECLARE_STATS(var)
#define LUDLC_INC_STATS(state, var)
#endif

/**
 * @struct packet_queue_item_status
 * @brief Used in the confirmation queue/callback to report packet status.
 *
 * @note The size of this structure is designed to match @ref ludlc_packet_hdr_t
 * to fit within the `packet_queue_item` union.
 */
struct packet_queue_item_status {
	/**< 0 on success, or an error code (e.g., EPIPE, EREMOTEIO). */
	ludlc_id_t result;
	/**< Reserved for alignment, matches `ack_id`. */
	ludlc_id_t reserved;
	/**< The channel the packet was sent on. */
	ludlc_channel_t	 chan;
};

/**
 * @struct packet_queue_item
 * @brief Represents a packet in the transmit window (sliding window queue).
 */
struct packet_queue_item {
	/**< Pointer to the payload data buffer. */
	const void		*payload;
	/**< Size of the payload data. */
	ludlc_payload_size_t	size;
	union {
		/**< Header used for transmission. */
		ludlc_packet_hdr_t 	hdr;
		/**< Status used for confirmation. */
		struct packet_queue_item_status status;
	};
	/**< Time-To-Live (remaining retries). */
	uint8_t			ttl;
};

/**
 * @struct ludlc_connection
 * @brief Main structure representing a single LuDLC connection.
 *
 * This structure holds the complete state, callbacks, timers, and queues
 * required to manage one LuDLC link.
 */
struct ludlc_connection {
	/**< Platform transport callbacks. */
	const struct ludlc_proto_cb	*proto;
	/**< User event callbacks. */
	const struct ludlc_conn_cb	*cb;
	/**< User context pointer for callbacks. */
	void				*user_ctx;

	/**< Connection-wide mutex lock. */
	LUDLC_DECLARE_LOCK(lock);

	/**< Timer for sending keep-alive pings. */
	ludlc_platform_timer_t	ping_timer;
	/**< Watchdog timer for connection timeout. */
	ludlc_platform_timer_t	wd_timer;

/** @def LUDLC_CONN_SEND_NAK_F
 * @brief Atomic flag bit indicating a NAK (Negative Ack) should be sent. */
#define LUDLC_CONN_SEND_NAK_F	0
/** @def LUDLC_CONN_TIMEOUT_F
 * @brief Atomic flag bit indicating the watchdog timer has expired. */
#define LUDLC_CONN_TIMEOUT_F	1
/** @def LUDLC_CONN_INITED_F
 * @brief Atomic flag bit indicating the connection is inited. */
#define LUDLC_CONN_INITED_F	2
	/**< Atomic bitfield for connection flags. */
	ludlc_platform_atomic_t	flags;

	/**< Connection statistics. */
	LUDLC_DECLARE_STATS(stats);

	/**< Pre-allocated header for control packets (PING, Handshake). */
	ludlc_packet_hdr_t ctrl_packet;
	/** @brief The sliding window packet queue. */
	struct packet_queue_item packets_q[CONFIG_LUDLC_WINDOW];

	struct ludlc_platform_connection pconn;

	/**!< ID of the last packet enqueued by the user. */
	ludlc_id_t last_queued;
	/**!< ID of the last packet actually sent on the wire. */
	ludlc_id_t last_sent;
	/**!< ID of the last packet acknowledged by the peer. */
	ludlc_id_t last_ack;
	/**!< ID of the last valid, in-sequence packet received. */
	ludlc_id_t last_received;

	/**!< Current connection FSM state (see @ref ludlc_conn_state). */
	uint8_t conn_state;
};

/**
 * @brief Processes a received LuDLC packet from the transport layer.
 * @see ludlc_receive() implementation in ludlc.c
 */
int ludlc_receive(struct ludlc_connection *conn,
			ludlc_packet_t *packet, size_t pkt_sz,
			ludlc_timestamp_t tstamp_usec);

/**
 * @brief Retrieves the next packet to be sent by the transport layer.
 * @see ludlc_get_packet_to_send() implementation in ludlc.c
 */
int ludlc_get_packet_to_send(struct ludlc_connection *conn,
		const void **hdr, ludlc_payload_size_t *hdr_sz,
		const void **payload, ludlc_payload_size_t *pay_sz);

#endif /* __LUDLC_PRIVATE_H__ */

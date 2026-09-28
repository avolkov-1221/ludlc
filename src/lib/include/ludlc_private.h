// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only)
/**
 * @file ludlc_private.h
 *
 * @brief Internal definitions for the LuDLC protocol implementation.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 */

#ifndef __LUDLC_PRIVATE_H__
#define __LUDLC_PRIVATE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ludlc_platform_api.h>
#include <ludlc_packet.h>
#include <ludlc_logger.h>

/**
 * @enum ludlc_conn_state
 * @brief Defines the states of the LuDLC connection finite state machine (FSM).
 */
enum ludlc_conn_state
{
	/** Initial and final state, no connection. */
	LUDLC_STATE_DISCONNECTED = 0,
	/** Sent first handshake packet, awaiting reply. */
	LUDLC_STATE_WAIT_CONN_1  = 1,
	/** Received first reply, sent second, awaiting final. */
	LUDLC_STATE_WAIT_CONN_2  = 2,
	/** Handshake complete, connection established. */
	LUDLC_STATE_CONNECTED    = 3,
	/** State to signal termination (not part of handshake). */
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
	uint32_t overrun;    /**< Count of rx overruns. */
};

#ifndef CONFIG_LUDLC_STATS
#define CONFIG_LUDLC_STATS 1
#endif
#if CONFIG_LUDLC_STATS
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
	/** 0 on success, or an error code (e.g., EPIPE, EREMOTEIO). */
	ludlc_id_t result;
	/** Reserved for alignment, matches `ack_id`. */
	ludlc_id_t reserved;
	/** The channel the packet was sent on. */
	ludlc_channel_t	 chan;
};

/**
 * @struct packet_queue_item
 * @brief Represents a packet in the transmit window (sliding window queue).
 */
struct packet_queue_item {
	/** Pointer to the payload data buffer. */
	const void		*payload;
	/** Size of the payload data. */
	ludlc_payload_size_t	size;
	union {
		/** Header used for transmission. */
		ludlc_packet_hdr_t 	hdr;
		/** Status used for confirmation. */
		struct packet_queue_item_status status;
	};
	/** Time-To-Live (remaining retries). */
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
	/** User event callbacks. */
	const struct ludlc_conn_cb	*cb;
	/** User context pointer for callbacks. */
	void				*user_ctx;

	/** Connection-wide mutex lock. */
	LUDLC_DECLARE_LOCK(lock);

	/** Timer for sending keep-alive pings. */
	ludlc_platform_timer_t	ping_timer;
	/** Watchdog timer for connection timeout. */
	ludlc_platform_timer_t	wd_timer;

/** @def LUDLC_CONN_NO_NAK_F
 * @brief Clear when the peer is to be told a packet went missing.
 *
 * Active low, as @ref LUDLC_CONN_U_IDLE_F explains. */
#define LUDLC_CONN_NO_NAK_F		0
/** @def LUDLC_CONN_INITED_F
 * @brief Atomic flag bit indicating the connection is inited. */
#define LUDLC_CONN_INITED_F		1
/** @def LUDLC_CONN_DISC_AFTER_TX_F
 * @brief Request local disconnect once queued disconnect control packet drains. */
#define LUDLC_CONN_DISC_AFTER_TX_F	2
/** @def LUDLC_CONN_U_IDLE_F
 * @brief Clear while an unnumbered packet waits to be sent.
 *
 * Active low, so that taking it is one @ref ludlc_platform_test_and_set_bit.
 * Not every target has an instruction that tests and clears in one go, and a
 * test plus a clear is two operations. Starts set; see
 * @ref LUDLC_CONN_IDLE_MASK. */
#define LUDLC_CONN_U_IDLE_F		3
/** @def LUDLC_CONN_U_DONE_F
 * @brief Clear while the transport is still reading the unnumbered packet.
 *
 * The transport reads the caller's buffer after the lock is released, so
 * the slot stays in use until the next frame is requested. The previous
 * frame has been sent by then, and the buffer can be returned. */
#define LUDLC_CONN_U_DONE_F		4

	/** @brief The flags above, read and written under @c lock.
	 *
	 * The active-low ones are taken with a single
	 * @ref ludlc_platform_test_and_set_bit; the rest are plain set and
	 * clear, the lock being what keeps the word consistent. */
	ludlc_platform_atomic_t		flags;

	/**
	 * @brief The link's clock, its own copy.
	 *
	 * Filled at init from the caller's cfg, or from
	 * @ref ludlc_conn_cfg_default, never borrowed: the caller's cfg may be
	 * a compound literal that is out of scope by the time a packet is
	 * received.
	 */
	struct ludlc_conn_cfg		cfg;

	/** Connection statistics. */
	LUDLC_DECLARE_STATS(stats);

	/** Pre-allocated header for control packets (PING, Handshake). */
	ludlc_packet_hdr_t ctrl_packet;
	/** @brief The sliding window packet queue. */
	struct packet_queue_item packets_q[CONFIG_LUDLC_WINDOW];

	/** @brief The pending unnumbered packet, see the flags above.
	 *
	 * A single item rather than a queue, because it uses no window slot.
	 * Its payload is the caller's buffer, as for a queued packet. */
	struct packet_queue_item	u_packet;

	/** @brief The platform specific extension. */
	struct ludlc_platform_connection pconn;

	/** ID of the last packet enqueued by the user. */
	ludlc_id_t last_queued;
	/** ID of the last packet actually sent on the wire. */
	ludlc_id_t last_sent;
	/** ID of the last packet acknowledged by the peer. */
	ludlc_id_t last_ack;
	/** ID of the last valid, in-sequence packet received. */
	ludlc_id_t last_received;

	/** Current connection FSM state (see @ref ludlc_conn_state). */
	uint8_t conn_state;
};

/**
 * @brief Checks whether currently prepared TX frame is disconnect-after-tx one.
 *
 * This helper is shared by platform TX threads to detect the canonical
 * empty control-channel disconnect packet that should trigger local disconnect
 * once the TX path drains.
 *
 * @param conn Connection owning flags/state.
 * @param hdr Pointer to currently prepared frame header.
 * @param hdr_size Header size in bytes.
 * @param payload_size Payload size in bytes.
 * @return true if this frame matches disconnect-after-tx marker.
 */
static inline bool ludlc_is_disconnect_after_tx_packet(
		struct ludlc_connection *conn,
		const void *hdr,
		ludlc_payload_size_t hdr_size,
		ludlc_payload_size_t payload_size)
{
	if (!conn || !hdr) {
		return false;
	}

	if (!ludlc_platform_test_bit(LUDLC_CONN_DISC_AFTER_TX_F, &conn->flags)) {
		return false;
	}

	if (hdr_size != sizeof(ludlc_packet_hdr_t) || payload_size != 0) {
		return false;
	}

	return ((const ludlc_packet_hdr_t *)hdr)->chan ==
			CONFIG_LUDLC_CONTROL_CHANNEL;
}

/**
 * @brief Processes a received LuDLC packet from the transport layer.
 * @see ludlc_receive() implementation in ludlc.c
 */
int ludlc_receive(struct ludlc_connection *conn,
			ludlc_packet_t *packet, size_t pkt_sz,
			ludlc_timestamp_t tstamp);

/**
 * @brief Retrieves the next packet to be sent by the transport layer.
 * @see ludlc_get_packet_to_send() implementation in ludlc.c
 */
int ludlc_get_packet_to_send(struct ludlc_connection *conn,
		const void **hdr, ludlc_payload_size_t *hdr_sz,
		const void **payload, ludlc_payload_size_t *pay_sz);

/**
 * @brief Handles transport-side disconnect/timeout events in core.
 *
 * This API performs a state-safe disconnect transition, fails queued packets,
 * and notifies user callbacks once (outside lock).
 *
 * @param conn The connection to disconnect.
 *
 * @note This helper is idempotent and safe to call on repeated transport
 * error/timeout notifications.
 */
void ludlc_handle_disconnect(struct ludlc_connection *conn);

/** @brief The active-low flags, as they stand with nothing outstanding.
 *
 * Zeroed memory reads as all of them pending, so init and the disconnect
 * sweep set them. */
#define LUDLC_CONN_IDLE_MASK						\
	((ludlc_platform_atomic_t)((1U << LUDLC_CONN_NO_NAK_F) |	\
				   (1U << LUDLC_CONN_U_IDLE_F) |	\
				   (1U << LUDLC_CONN_U_DONE_F)))

/** @brief Flags the disconnect sweep keeps when it clears the runtime ones. */
#define LUDLC_CONN_SWEEP_KEEP_MASK	(1U << LUDLC_CONN_INITED_F)

/** @brief Flags the disconnect sweep clears: whatever it does not keep.
 *
 * Written as what is left over so that a flag added later is cleared without
 * anyone remembering to name it here. */
#define LUDLC_CONN_SWEEP_CLEAR_MASK					\
	((ludlc_platform_atomic_t)~(LUDLC_CONN_SWEEP_KEEP_MASK |	\
				    LUDLC_CONN_IDLE_MASK))

#endif /* __LUDLC_PRIVATE_H__ */

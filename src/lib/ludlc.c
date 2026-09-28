// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only)
/**
 * @file ludlc.c
 *
 * @brief LuDLC core logic implementation.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <ludlc.h>
#include <ludlc_platform.h>
#include <ludlc_private.h>

/**
 * @def HANDLE_RX_CONTINUE_F
 * @brief Flag indicating that packet handling should continue processing.
 *        Implies a successful state transition to CONNECTED.
 */
#define HANDLE_RX_CONTINUE_F	(1U<<0)

/**
 * @def HANDLE_RX_REQUEST_TX_F
 * @brief Flag indicating that a transmitter have to be called ASAP.
 */
#define HANDLE_RX_REQUEST_TX_F	(1U<<1)

/**
 * @def HANDLE_RX_ACCEPT_F
 * @brief Flag indicating that packet is ok and should be transferred
 *        to the upper layer.
 */
#define HANDLE_RX_ACCEPT_F	(1U<<2)

/**
 * @def HANDLE_RX_CONNECTED_F
 * @brief Flag indicating that the connection has been established and
 *        the upper level should be notified.
 */
#define HANDLE_RX_CONNECTED_F	(1U<<3)

/**
 * @def HANDLE_RX_DISCONNECTED_F
 * @brief Flag indicating that the connection has been lost and
 *        the upper level should be notified.
 */
#define HANDLE_RX_DISCONNECT_F	(1U<<4)

/**
 * @brief Checks if a given packet ID represents a PING packet.
 *
 * @param id The LuDLC packet ID to check.
 * @return true if the ID has the PING flag set, false otherwise.
 */
static inline bool is_ping(ludlc_id_t id)
{
	return (id & LUDLC_PING_FLAG) != 0;
}

/**
 * @brief Checks if the transmit (TX) queue is empty.
 *
 * @param conn Pointer to the connection structure.
 * @return true if no packets are queued for transmission, false otherwise.
 */
static inline bool ludlc_tx_queue_empty(struct ludlc_connection *conn)
{
	if( (conn->last_queued - conn->last_ack) & LUDLC_ID_MASK )
		return false;
	return true;
}

/**
 * @brief Checks if the transmit (TX) queue is full (window is saturated).
 *
 * @param conn Pointer to the connection structure.
 * @return true if the number of unacknowledged packets has reached the
 * window size, false otherwise.
 */
static inline bool ludlc_tx_queue_full(struct ludlc_connection *conn)
{
	if (((conn->last_queued - conn->last_ack) & LUDLC_ID_MASK) >=
			CONFIG_LUDLC_WINDOW)
		return true;
	return false;
}

static inline void ludlc_call_on_confirm(struct ludlc_connection *conn,
		const struct packet_queue_item *pkt)
{
	if (conn->cb && conn->cb->on_confirm) {
		conn->cb->on_confirm(conn,
				pkt->status.result,
				pkt->status.chan,
				pkt->payload,
				pkt->size,
				conn->user_ctx);
	}
}

static inline void ludlc_call_on_disconnect(struct ludlc_connection *conn)
{
	if (conn->cb && conn->cb->on_disconnect) {
		conn->cb->on_disconnect(conn, conn->user_ctx);
	}
}

#ifdef CONFIG_LUDLC_MT
/* One slot per window packet, plus one for the unnumbered packet. */
#define LUDLC_DECLARE_CONFIRMED_QUEUE(q_name) \
	struct packet_queue_item q_name[CONFIG_LUDLC_WINDOW + 1]
/**
 * @brief (MT) Adds a confirmed packet to a temporary queue for later processing.
 *
 * In multi-threaded builds, this function queues confirmed packets. The
 * actual user callback is invoked later, outside the connection lock,
 * by @ref ludlc_confirm_unlocked.
 *
 * @note This function must be called with the connection lock held.
 *
 * @param conn Pointer to the connection structure (unused in this function).
 * @param pkt The packet queue item that has been confirmed.
 * @param cq The temporary array to store confirmed packets.
 * @param confirmed_num Pointer to the counter of confirmed packets in `cq`.
 */
static inline void ludlc_on_confirm_lock(struct ludlc_connection *conn,
				struct packet_queue_item *pkt,
				struct packet_queue_item *cq,
				ludlc_id_t *confirmed_num)
{
	(void)conn;
	cq[*confirmed_num] = *pkt;
	(*confirmed_num)++;
}

/**
 * @brief (MT) Invokes user confirmation callbacks after releasing the lock.
 *
 * Iterates over the temporarily queued confirmed packets and calls the
 * user's `on_confirm` callback for each one.
 *
 * @note This function must be called *without* the connection lock held.
 *
 * @param conn Pointer to the connection structure.
 * @param cq The array of confirmed packets.
 * @param confirmed_num The number of packets in `cq`.
 */
static void ludlc_confirm_unlocked(struct ludlc_connection *conn,
		struct packet_queue_item *cq, ludlc_id_t confirmed_num)
{
	ludlc_id_t i;

	for (i = 0; i < confirmed_num; i++) {
		ludlc_call_on_confirm(conn, &cq[i]);
	}
}
#else
/* Confirmations are made on the spot in this build, so nothing is held. */
#define LUDLC_DECLARE_CONFIRMED_QUEUE(q_name) \
	struct packet_queue_item *q_name = NULL
/**
 * @brief (non-MT) Immediately invokes the user confirmation callback.
 *
 * In single-threaded builds, this function directly calls the user's
 * `on_confirm` callback.
 *
 * @note This function must be called with the connection lock held.
 *
 * @param conn Pointer to the connection structure.
 * @param pkt The packet queue item that has been confirmed.
 * @param cq Unused in non-MT builds.
 * @param confirmed_num Unused in non-MT builds.
 */
static void ludlc_on_confirm_lock(struct ludlc_connection *conn,
				struct packet_queue_item *pkt,
				struct packet_queue_item *cq,
				ludlc_id_t *confirmed_num)
{
	(void)cq;
	(void)confirmed_num;
	ludlc_call_on_confirm(conn, pkt);
}

/**
 * @brief (non-MT) Stub function.
 *
 * In single-threaded builds, confirmations are handled immediately in
 * @ref ludlc_on_confirm_lock, so this function does nothing.
 *
 * @param conn Unused.
 * @param cq Unused.
 * @param confirmed_num Unused.
 */
static inline void ludlc_confirm_unlocked(struct ludlc_connection *conn,
		struct packet_queue_item *cq, ludlc_id_t confirmed_num)
{
	(void)conn;
	(void)cq;
	(void)confirmed_num;
}

#endif

/**
 * @brief Internal handler for disconnect case.
 *
 * Stops all timers, sets the state to DISCONNECTED, and fails every
 * outstanding packet with EPIPE. That covers the transmit queue and the
 * unnumbered packet, if one is still waiting to be sent.
 *
 * @note This function must be called with the connection lock held.
 *
 * @param conn Pointer to the connection structure.
 * @param cq Temporary queue for confirmed (failed) packets (for MT build).
 * @param confirmed_num Pointer to counter for `cq` (for MT build).
 */
static void on_disconnect_lock(struct ludlc_connection *conn,
		struct packet_queue_item *cq,
		ludlc_id_t *confirmed_num)
{
	ludlc_id_t i, cnt;

	LUDLC_LOG_DEBUG("on_disconnect_lock");

	ludlc_platform_stop_timer(&conn->wd_timer);
	ludlc_platform_stop_timer(&conn->ping_timer);

	conn->conn_state = LUDLC_STATE_DISCONNECTED;

	/* The unnumbered packet is not part of the window, so it is not among
	 * those failed below, but its buffer must be returned all the same.
	 */
	if (!ludlc_platform_test_bit(LUDLC_CONN_U_IDLE_F, &conn->flags) ||
			!ludlc_platform_test_bit(LUDLC_CONN_U_DONE_F,
				&conn->flags)) {
		conn->u_packet.status.result = EPIPE;
		ludlc_on_confirm_lock(conn, &conn->u_packet, cq, confirmed_num);
	}

	/* Keep the lifecycle bit and clear the runtime protocol flags. That
	 * frees the unnumbered packet slot confirmed above, which means setting
	 * the active-low flags rather than clearing them. The idle ones go
	 * first, so a reader between the two sees the slot free.
	 */
	ludlc_platform_set_mask(LUDLC_CONN_IDLE_MASK, &conn->flags);
	ludlc_platform_clear_mask(LUDLC_CONN_SWEEP_CLEAR_MASK, &conn->flags);

	/* Packets (last_ack+1)..last_queued inclusive,
	 * use ID space, not window mask
	 */
	cnt = (conn->last_queued - conn->last_ack) & LUDLC_ID_MASK;

	/* Iterate and fail all pending packets */
	i = conn->last_ack;
	for (; cnt; cnt--) {
		i = (i + 1) & LUDLC_ID_MASK;
		conn->packets_q[i & LUDLC_WINDOW_MASK].status.result = EPIPE;
		ludlc_on_confirm_lock(
			conn,
			&conn->packets_q[i & LUDLC_WINDOW_MASK],
			cq, confirmed_num);
	}

	/* Reset queue pointers */
	conn->last_sent = 0;
	conn->last_queued = 0;
	conn->last_ack = 0;
	conn->last_received = 0;

	LUDLC_INC_STATS(conn, on_disconn);
}

/**
 * @brief Enqueues a data packet for transmission.
 *
 * Adds a data payload pointer to the transmit queue to be sent
 * to the remote peer. If the queue is full (transmit window is saturated),
 * it returns -EAGAIN. On success, it requests a transmission.
 *
 * @param conn Pointer to the connection structure.
 * @param dst_chan The destination channel for the packet.
 * @param buf Pointer to the payload data buffer.
 * @param size The size of the payload data in bytes.
 * @param ttl How many times the packet may be sent. The first send counts,
 * and each NAK that reports this packet missing costs one more. A value of
 * 0 is normalized to 1 (single send attempt). Values above
 * @ref LUDLC_MAX_TTL are clamped.
 * @return 0 on success.
 * @return -EINVAL if @p buf is NULL and @p size is not, or if @p size is
 * above @ref CONFIG_LUDLC_MAX_PAYLOAD_SIZE.
 * @return -EAGAIN if the transmit queue is full.
 */
int ludlc_enqueue_data(struct ludlc_connection *conn,
		ludlc_channel_t dst_chan,
		const void *buf, ludlc_payload_size_t size, uint8_t ttl)
{
	int ret;

	if (!buf && size) {
		return -EINVAL;
	}

	/* A larger payload would be sent and then dropped by the receiver,
	 * where it would look like a packet that never arrived.
	 */
	if (size > CONFIG_LUDLC_MAX_PAYLOAD_SIZE) {
		return -EINVAL;
	}

	if (ttl > LUDLC_MAX_TTL) {
		ttl = LUDLC_MAX_TTL;
	} else if(ttl == 0) {
		ttl = 1;
	}

	LUDLC_LOCK(&conn->lock);
	if (!ludlc_tx_queue_full(conn)) {
		/* Calculate next queue ID */
		ludlc_id_t id = (conn->last_queued + 1) & LUDLC_ID_MASK;
		ludlc_id_t q_idx = id & LUDLC_WINDOW_MASK;

		/* Populate the queue slot */
		conn->packets_q[q_idx].hdr.chan = dst_chan;
		conn->packets_q[q_idx].ttl = ttl;
		conn->packets_q[q_idx].payload = buf;
		conn->packets_q[q_idx].size = size;

		/* Commit the new packet */
		conn->last_queued = id;

		ret = 0;
	} else {
		ret = -EAGAIN;
	}
	LUDLC_UNLOCK(&conn->lock);

	if (!ret)
		ludlc_platform_request_tx(conn); /* Signal TX task */

	return ret;
}

/**
 * @brief Sends an unnumbered (U) packet, ahead of everything else.
 *
 * The packet takes no sequence number and no place in the window, so it goes
 * out in the next frame regardless of the window state: during a
 * re-transmission after a NAK, or while the window is full.
 *
 * It is not acknowledged and never re-transmitted, so the confirmation
 * means the packet was sent, not that the peer received it. The buffer
 * belongs to the caller until then, as for @ref ludlc_enqueue_data. Only
 * one packet is held at a time.
 *
 * @param conn Pointer to the connection structure.
 * @param dst_chan The destination channel for the packet.
 * @param buf Pointer to the payload data buffer, which must stay valid
 * until the packet is confirmed.
 * @param size The size of the payload data in bytes.
 * @return 0 on success.
 * @return -EINVAL if @p buf is NULL, or @p size is 0 or above
 * @ref CONFIG_LUDLC_MAX_PAYLOAD_SIZE.
 * @return -ENOTCONN if the connection is not established. An unnumbered
 * packet is sent immediately or not at all, so it does not wait for the link
 * to come up.
 * @return -EAGAIN if the previous unnumbered packet has not been sent yet.
 */
int ludlc_send_urgent_data(struct ludlc_connection *conn,
		ludlc_channel_t dst_chan,
		const void *buf, ludlc_payload_size_t size)
{
	int ret;

	if (!conn || !buf || !size ||
	    size > CONFIG_LUDLC_MAX_PAYLOAD_SIZE) {
		return -EINVAL;
	}

	LUDLC_LOCK(&conn->lock);
	if (conn->conn_state != LUDLC_STATE_CONNECTED) {
		ret = -ENOTCONN;
	} else if (!ludlc_platform_test_bit(LUDLC_CONN_U_IDLE_F,
				&conn->flags) ||
			!ludlc_platform_test_bit(LUDLC_CONN_U_DONE_F,
				&conn->flags)) {
		ret = -EAGAIN;
	} else {
		conn->u_packet.hdr.chan = dst_chan;
		conn->u_packet.payload = buf;
		conn->u_packet.size = size;

		ludlc_platform_clear_bit(LUDLC_CONN_U_IDLE_F, &conn->flags);

		ret = 0;
	}
	LUDLC_UNLOCK(&conn->lock);

	if (!ret)
		ludlc_platform_request_tx(conn); /* Signal TX task */

	return ret;
}

int ludlc_send_disconnect(struct ludlc_connection *conn)
{
	int ret;

	if (!conn) {
		return -EINVAL;
	}

	/*
	 * Request local disconnect after TX thread drains this control packet.
	 * Set the flag before enqueue to avoid missing the packet due to races.
	 */
	ludlc_platform_set_bit(LUDLC_CONN_DISC_AFTER_TX_F, &conn->flags);
	ret = ludlc_enqueue_data(conn, CONFIG_LUDLC_CONTROL_CHANNEL, NULL, 0, 1);
	if (ret) {
		ludlc_platform_clear_bit(LUDLC_CONN_DISC_AFTER_TX_F, &conn->flags);
	}

	return ret;
}

int ludlc_connect(struct ludlc_connection *conn)
{
	LUDLC_DECLARE_CONFIRMED_QUEUE(confirmed_q);
	ludlc_id_t confirmed_num = 0;
	int ret = 0;

	if (!conn ||
	    !ludlc_platform_test_bit(LUDLC_CONN_INITED_F, &conn->flags)) {
		return -EINVAL;
	}

	LUDLC_LOCK(&conn->lock);
	if (conn->conn_state == LUDLC_STATE_TERMINATE) {
		ret = -ESHUTDOWN;
	} else {
		/*
		 * Reuse disconnect cleanup path for reconnect:
		 * stop timers, clear runtime flags, reset FSM/queue state,
		 * and fail any pending packets through normal confirm path.
		 */
		on_disconnect_lock(conn, confirmed_q, &confirmed_num);
	}
	LUDLC_UNLOCK(&conn->lock);

	ludlc_confirm_unlocked(conn, confirmed_q, confirmed_num);
	if (!ret) {
		ludlc_platform_request_tx(conn);
	}

	return ret;
}

/**
 * @brief Handles an incoming packet when the LuDLC FSM is in an
 * unconnected or connecting state.
 *
 * This function processes control packets to manage handshake.
 * It returns a set of flags indicating the required next actions,
 * such as requesting a response transmission and/or continuing processing.
 *
 * @note **The packet's header TX id in handshake time is handled differently
 * than in established connection.**
 *
 * @param conn Pointer to the connection structure to update.
 * @param packet Pointer to the received LUDLC packet.
 * @param pkt_sz The size of the received packet.
 * @retval An integer containing bit flags:
 * - HANDLE_RX_REQUEST_TX_F if a response should be sent.
 * - HANDLE_RX_CONTINUE_F if processing should continue (e.g., connected).
 */
static int rx_handle_handshake(
		struct ludlc_connection *conn,
		ludlc_packet_t *packet,
		size_t pkt_sz)
{
	ludlc_id_t id = packet->hdr.id.tx_id;
	int ret = HANDLE_RX_REQUEST_TX_F; /* Default to sending a response */

	/* Check for unexpected ping or non-control packets. */
	if (is_ping(id) ||
		packet->hdr.chan != CONFIG_LUDLC_CONTROL_CHANNEL) {
		/* In WAIT_CONN_2 the peer has already moved to CONNECTED and
		 * is sending normal packets, so this side follows.
		 */
		if (conn->conn_state == LUDLC_STATE_WAIT_CONN_2) {
			conn->conn_state = LUDLC_STATE_CONNECTED;
			/* Carry on parsing the header as a normal packet. */
			return ret | HANDLE_RX_CONTINUE_F | HANDLE_RX_CONNECTED_F;
		}
		LUDLC_LOG_DEBUG(
			"unexpected packet arrived: (%x, %x) when state = %d",
				packet->hdr.id.tx_id,
				packet->hdr.id.ack_id,
				conn->conn_state);

		/* If unexpected packet arrived, then restart the handshake */
		conn->conn_state = LUDLC_STATE_DISCONNECTED;
	} else {
		/* Check if the received ID is within acceptable range
		 * (conn_state or conn_state + 1).
		 */
		if ((id - conn->conn_state) < 2U) {
			/* Check the optional caps data */
			if (id == LUDLC_STATE_WAIT_CONN_1 &&
					pkt_sz > sizeof(ludlc_packet_hdr_t)) {
				ret |= HANDLE_RX_ACCEPT_F;
			}

			/* This is the expected next handshake step */
			if (++conn->conn_state == LUDLC_STATE_CONNECTED) {
				ret |= HANDLE_RX_CONNECTED_F;
			}
		} else if ((id + 1) != conn->conn_state) {
			/* An ID too far off (not conn_state - 1), most likely
			 * because handshake packets were lost.
			 */
			LUDLC_LOG_DEBUG(
				"ID is too far off: (%x, %x) from state = %d",
					packet->hdr.id.tx_id,
					packet->hdr.id.ack_id,
					conn->conn_state);
			/* Restart handshake */
			conn->conn_state = LUDLC_STATE_DISCONNECTED;
		} else {
			/* This is a duplication of previous packet, drop it */
			/* (id == conn_state - 1) */
			ret &= ~HANDLE_RX_REQUEST_TX_F;
		}
	}

	return ret;
}

/**
 * @brief Handles a Negative Acknowledgment (NAK) condition.
 *
 * A NAK identifies one packet: it acknowledges the last packet received
 * in order, so the missing one is the next after it. Only that packet has
 * its TTL decremented. The packets behind it are re-transmitted because
 * the receiver discards everything after a gap, not because they were
 * lost, so their TTL is left alone.
 *
 * A packet left with no attempts is not dropped here but by
 * @ref ludlc_tx_drop_expired, on the next transmit. The transport reads the
 * queue once the lock is released, and this may run while it does so.
 *
 * Finally, it resets `last_sent` to `last_ack` to force a re-transmission
 * of all remaining unacknowledged packets.
 *
 * @param conn Pointer to the connection structure.
 * @param num_wait The number of packets waiting for acknowledgment.
 * @return @ref HANDLE_RX_REQUEST_TX_F to signal an immediate re-transmission.
 */
static int rx_handle_nak(struct ludlc_connection *conn, ludlc_id_t num_wait)
{
	ludlc_id_t id = (conn->last_ack + 1) & LUDLC_ID_MASK;
	ludlc_id_t q_idx = id & LUDLC_WINDOW_MASK;

	/* The missing packet is the first unacknowledged one */
	if (num_wait && conn->packets_q[q_idx].ttl) {
		conn->packets_q[q_idx].ttl--;
	}

	/* resend all unconfirmed packets */
	conn->last_sent = conn->last_ack;
	return HANDLE_RX_REQUEST_TX_F;
}

/**
 * @brief Core internal packet processing logic.
 *
 * This function is the core of the receive-side logic. It must be
 * called with the connection lock held. It performs the following steps:
 * 1. Validates packet size.
 * 2. If not connected, calls @ref rx_handle_handshake.
 * 3. Handles control channel packets (regular control payloads and explicit
 *    disconnect opcode).
 * 4. Resets the connection watchdog timer.
 * 5. Validates the packet's TX ID sequence (`tx_id`).
 * 6. Sets flags for NAK or acceptance.
 * 7. Processes the acknowledgment ID (`ack_id`) to confirm sent packets.
 * 8. Calls @ref rx_handle_nak if a NAK flag is present.
 *
 * @param conn Pointer to the connection structure.
 * @param packet Pointer to the received LUDLC packet.
 * @param pkt_sz The total size of the received packet.
 * @param confirmed_q Temporary queue for confirmed packets (for MT build).
 * @param confirmed_num Pointer to counter for `confirmed_q` (for MT build).
 * @return A bitmask of flags:
 * - @ref HANDLE_RX_REQUEST_TX_F to request a response (ACK/NAK/PING).
 * - @ref HANDLE_RX_ACCEPT_F to accept the packet's payload.
 */
static int rx_handle_packet(
		struct ludlc_connection *conn,
		ludlc_packet_t *packet,
		size_t pkt_sz,
		struct packet_queue_item *confirmed_q,
		ludlc_id_t *confirmed_num)
{
	bool ping = is_ping(packet->hdr.id.tx_id);
	ludlc_id_t id, next_rcvd_id;
	ludlc_id_t num_wait, num_acked;
	int ret;

	if (!ping && pkt_sz < sizeof(ludlc_packet_hdr_t)) {
		/* Too short to hold a header, yet the checksum is good, so
		 * the peer is misbehaving. Drop it.
		 */
		LUDLC_INC_STATS(conn, dropped);
		return 0;
	}

	if (conn->conn_state != LUDLC_STATE_CONNECTED) {
		ret = rx_handle_handshake(conn, packet, pkt_sz);
		if ((ret & HANDLE_RX_CONTINUE_F) == 0) {
			/* Handshake not complete, stop further processing */
			return ret;
		}
		/* Handshake just completed, continue processing this packet */
		ret &= ~HANDLE_RX_CONTINUE_F;
	} else {
		ret = 0;
	}

	id = packet->hdr.id.tx_id & LUDLC_ID_MASK;
	if (!ping && packet->hdr.chan == CONFIG_LUDLC_CONTROL_CHANNEL) {
		if (pkt_sz == sizeof(ludlc_packet_hdr_t)) {
			if (id != LUDLC_STATE_WAIT_CONN_2) {
				/* Any empty control packet other than
				 * last handshake step
				 */
				on_disconnect_lock(conn, confirmed_q,
						   confirmed_num);
				ret |= HANDLE_RX_DISCONNECT_F;
			}
			/* Respond to disconnect/handshake */
			return ret | HANDLE_RX_REQUEST_TX_F;
		}
	}

	/* Any valid packet (even ping) resets the connection watchdog */
	ludlc_platform_start_timer(&conn->wd_timer,
			3 * CONFIG_LUDLC_PING_TIME,
			0);

	/* 1. check the correctness of the incoming packet's sequence. */
	next_rcvd_id = (conn->last_received + 1) & LUDLC_ID_MASK;
	if (next_rcvd_id != id) {
		if (conn->last_received != id) {
			/* Out of order, so ask the peer to resend what was
			 * missed (NAK).
			 */
			ludlc_platform_clear_bit(LUDLC_CONN_NO_NAK_F,
					&conn->flags);
			ret |= HANDLE_RX_REQUEST_TX_F;
		}
		/* Otherwise last_received == id, a duplicate of the packet
		 * just received: the peer resent it because our ACK was lost.
		 * Drop the payload, but still process its ACK field.
		 */
	} else if (!ping) {
		conn->last_received = next_rcvd_id;
		ret |= HANDLE_RX_REQUEST_TX_F | HANDLE_RX_ACCEPT_F;
	}

	if (ludlc_is_unnumbered_data(packet->hdr.id.tx_id, pkt_sz)) {
		/* Outside the sequence, so accept the payload regardless of
		 * the ID check above and leave last_received unchanged.
		 * There is nothing to ACK.
		 */
		ret |= HANDLE_RX_ACCEPT_F;
	}

	/* 2. now process ack_id for our side */
	/* Get the number of packets are waiting for acknowledge */
	num_wait = (conn->last_sent - conn->last_ack) & LUDLC_ID_MASK;

	/* Number of packets this ACK confirms. ludlc_id_t is unsigned, so the
	 * subtraction wraps and the mask alone gives the right count.
	 */
	num_acked = (packet->hdr.id.ack_id - conn->last_ack) & LUDLC_ID_MASK;

	if (num_acked <= num_wait) {
		/* Valid ACK range */
		for (; num_acked; num_acked--) {
			uint8_t q_idx;
			conn->last_ack = (conn->last_ack + 1) & LUDLC_ID_MASK;
			q_idx = conn->last_ack & LUDLC_WINDOW_MASK;
			conn->packets_q[q_idx].status.result = 0;
			ludlc_on_confirm_lock(conn, &conn->packets_q[q_idx],
					confirmed_q, confirmed_num);
		}

		/* NAK handling needs the unacked count after the cumulative
		 * ACK above, not the stale num_wait from before the loop.
		 */
		if (packet->hdr.id.ack_id & LUDLC_NAK_FLAG) {
			num_wait = (conn->last_sent - conn->last_ack) &
					LUDLC_ID_MASK;
			ret |= rx_handle_nak(conn, num_wait);
		}
	} else {
		/* The peer ACKed packets we never sent, so the two sides are
		 * badly out of sync.
		 *
		 * TODO: force an immediate reconnect?
		 */
		LUDLC_LOG_DEBUG("ACK ID is too far: (%x) from last_ack = %x",
				packet->hdr.id.ack_id, conn->last_ack);
	}

	return ret;
}

/**
 * @brief Processes a received LuDLC packet from the transport layer.
 *
 * This is the main entry point for feeding data *into* the LuDLC state
 * machine. It acquires the connection lock, processes the packet, and
 * then (after releasing the lock) invokes any necessary user callbacks
 * (`on_confirm` for acknowledged packets, `on_recv` for new data).
 *
 * @param conn Pointer to the connection structure.
 * @param packet Pointer to the buffer containing the received packet.
 * @param pkt_sz The total size of the received packet in bytes.
 * @param tstamp Monotonic timestamp when the packet was received.
 * @return 0 on success.
 */
int ludlc_receive(struct ludlc_connection *conn,
			ludlc_packet_t *packet, size_t pkt_sz,
			ludlc_timestamp_t tstamp)
{
	int ret;
	LUDLC_DECLARE_CONFIRMED_QUEUE(confirmed_q);
	ludlc_id_t confirmed_num = 0;

	LUDLC_LOG_TRACE("packet (%x, %x) @%llu",
			packet->hdr.id.tx_id,
			packet->hdr.id.ack_id,
			tstamp);

	LUDLC_LOCK(&conn->lock);
		ret = rx_handle_packet(conn, packet, pkt_sz,
				confirmed_q, &confirmed_num);
	LUDLC_UNLOCK(&conn->lock);

	/* --- Actions outside the lock --- */

	if (ret & HANDLE_RX_CONNECTED_F) {
		LUDLC_INC_STATS(conn, on_conn);
		if (conn->cb && conn->cb->on_connect) {
			conn->cb->on_connect(conn, conn->user_ctx);
		}
	}

	if(ret & HANDLE_RX_REQUEST_TX_F) {
		ludlc_platform_request_tx(conn);  /* Signal TX task */
	}

	/* Call deferred on_confirm callbacks (for MT) */
	ludlc_confirm_unlocked(conn, confirmed_q, confirmed_num);

	if (ret & HANDLE_RX_DISCONNECT_F) {
		ludlc_call_on_disconnect(conn);
	}

	/* Call "on_recv" callback if packet is ok */
	if ((ret & HANDLE_RX_ACCEPT_F) &&
	    conn->cb && conn->cb->on_recv) {
		conn->cb->on_recv(conn,
				packet->hdr.chan,
				packet->payload,
				pkt_sz - sizeof(packet->hdr),
				tstamp,
				conn->user_ctx);
	}

	return 0;
}

/**
 * @brief Drops the leading packet if its retry budget has run out.
 *
 * A NAK decrements the TTL of the packet it names but leaves it in place, so
 * the queue is only rearranged here, on the transmit side. By this point the
 * previous frame has been sent and the transport has finished reading the
 * slot it came from; dropping the packet as the NAK arrived would move a slot
 * the transport is still encoding and free a payload it is still reading.
 *
 * Only the first unacknowledged packet is ever decremented, so at most one
 * can be waiting to be dropped. The packets behind it move up one place, each
 * taking the previous ID. A NAK also rewinds `last_sent` to `last_ack`, so
 * those packets are all unsent and keep their order.
 *
 * @note This function must be called with the connection lock held.
 *
 * @param conn Pointer to the connection structure.
 * @param[out] expired The dropped packet, to be confirmed once the lock has
 * been released.
 * @return true if a packet was dropped and @p expired filled in.
 */
static bool ludlc_tx_drop_expired(struct ludlc_connection *conn,
		struct packet_queue_item *expired)
{
	ludlc_id_t id = (conn->last_ack + 1) & LUDLC_ID_MASK;
	ludlc_id_t q_idx = id & LUDLC_WINDOW_MASK;
	ludlc_id_t num;

	/* Nothing outstanding, or the leading packet still has attempts left.
	 * A queued packet starts with at least one, so only a NAK can bring it
	 * to 0.
	 */
	if (conn->last_queued == conn->last_ack || conn->packets_q[q_idx].ttl)
		return false;

	conn->packets_q[q_idx].status.result = ETIMEDOUT;
	*expired = conn->packets_q[q_idx];

	num = (conn->last_queued - id) & LUDLC_ID_MASK;

	for (; num; num--) {
		ludlc_id_t next = (id + 1) & LUDLC_ID_MASK;

		conn->packets_q[id & LUDLC_WINDOW_MASK] =
			conn->packets_q[next & LUDLC_WINDOW_MASK];

		id = next;
	}

	conn->last_queued = (conn->last_queued - 1) & LUDLC_ID_MASK;

	return true;
}

/**
 * @brief Retrieves the next packet to be sent by the transport layer.
 *
 * This function is called by the transmit task to get the next packet
 * to send. It acquires the lock and determines what to send based on
 * the connection state:
 * - **Handshake:** Returns a control packet.
 * - **Connected (queue empty):** Returns a PING packet.
 * - **Connected (queue has data):** Returns the next data packet from the queue.
 *
 * It provides pointers to the header and payload segments separately.
 *
 * @param conn Pointer to the connection structure.
 * @param[out] hdr Pointer to a `const void*` that will be updated to
 * point to the packet's header.
 * @param[out] hdr_sz Pointer to a size variable to be updated with the
 * header's size.
 * @param[out] payload Pointer to a `const void*` that will be updated to
 * point to the packet's payload (or NULL).
 * @param[out] pay_sz Pointer to a size variable to be updated with the
 * payload's size (or 0).
 *
 * @return 0 if more packets are pending in the queue after this one.
 * @return 1 if this is the last packet in the queue (or if a PING/control
 * packet is sent).
 * @return -EINVAL if any output pointers are NULL.
 */
int ludlc_get_packet_to_send(struct ludlc_connection *conn,
		const void **hdr,
		ludlc_payload_size_t *hdr_sz,
		const void **payload,
		ludlc_payload_size_t *pay_sz)
{
	struct packet_queue_item u_sent, expired;
	bool u_done = false, exp_done;
	uint8_t ack, id, q_idx;
	int ret = 1; /* Assume this is the last packet by default */

	if (!hdr || !hdr_sz || !payload || !pay_sz)
		return -EINVAL;

	LUDLC_LOCK(&conn->lock);

	/* The previous frame has been sent by now, so the transport has
	 * finished with the unnumbered packet and its buffer can be returned
	 * to the caller. Copy the item first: the slot is free from here on,
	 * and the confirmation is made below, outside the lock.
	 */
	if (!ludlc_platform_test_and_set_bit(LUDLC_CONN_U_DONE_F,
			&conn->flags)) {
		conn->u_packet.status.result = 0;
		u_sent = conn->u_packet;
		u_done = true;
	}

	/* The same goes for a packet that has run out of attempts: it is taken
	 * off the queue here, where the transport is between frames.
	 */
	exp_done = ludlc_tx_drop_expired(conn, &expired);

	switch (conn->conn_state) {
	default:/* LUDLC_STATE_DISCONNECTED, LUDLC_STATE_WAIT_CONN_1, etc. */

		/* Send handshake control packet */
		conn->ctrl_packet.chan = CONFIG_LUDLC_CONTROL_CHANNEL;
		conn->ctrl_packet.id.tx_id = conn->conn_state;
		/* No ACK during handshake */
		conn->ctrl_packet.id.ack_id = 0;

		/* Full control header (id + channel) */
		*hdr = &conn->ctrl_packet;
		*hdr_sz = sizeof(conn->ctrl_packet);

		*payload = NULL;
		*pay_sz = 0;

		break;

	case LUDLC_STATE_CONNECTED:
		/* Prepare ACK ID */
		ack = conn->last_received;

		/* Check if a NAK needs to be sent, and take it if so */
		if (!ludlc_platform_test_and_set_bit(LUDLC_CONN_NO_NAK_F,
						&conn->flags)) {
			ack |= LUDLC_NAK_FLAG;
		}

		/* Next data packet sequence */
		id = (conn->last_sent + 1) & LUDLC_ID_MASK;

		if (!ludlc_platform_test_and_set_bit(LUDLC_CONN_U_IDLE_F,
				&conn->flags)) {
			/* An unnumbered packet is sent before anything else,
			 * including a re-transmission, as it uses none of
			 * the window's IDs. Like a PING, it carries the ID
			 * to be sent next, so the peer can still detect a
			 * gap.
			 */
			ludlc_platform_clear_bit(LUDLC_CONN_U_DONE_F,
					&conn->flags);

			conn->u_packet.hdr.id.tx_id = id | LUDLC_PING_FLAG;
			conn->u_packet.hdr.id.ack_id = ack;

			*hdr = &conn->u_packet.hdr;
			*hdr_sz = sizeof(conn->u_packet.hdr);

			*payload = conn->u_packet.payload;
			*pay_sz = conn->u_packet.size;

			if (conn->last_sent != conn->last_queued) {
				/* The window still has packets waiting */
				ret = 0;
			}

			break;
		}

		if (conn->last_sent == conn->last_queued) {
			/* Nothing to send, so send PING instead. */
			conn->ctrl_packet.id.tx_id = id | LUDLC_PING_FLAG;
			conn->ctrl_packet.id.ack_id = ack;
			*hdr = &conn->ctrl_packet.id;
			*hdr_sz = sizeof(conn->ctrl_packet.id);

			*payload = NULL;
			*pay_sz = 0;

			break;
		}

		/* send packet from the queue */
		q_idx = id & LUDLC_WINDOW_MASK;

		/* Fill in the dynamic header fields */
		conn->packets_q[q_idx].hdr.id.tx_id = id;
		conn->packets_q[q_idx].hdr.id.ack_id = ack;

		*hdr = &conn->packets_q[q_idx].hdr;
		*hdr_sz = sizeof(conn->packets_q[q_idx].hdr);

		*payload = conn->packets_q[q_idx].payload;
		*pay_sz = conn->packets_q[q_idx].size;

		/* Advance sent pointer */
		conn->last_sent = id;

		if (id != conn->last_queued) {
			/* More packets are waiting in the queue */
			ret = 0;
		}
		break;
	}

	LUDLC_UNLOCK(&conn->lock);

	if (u_done)
		ludlc_call_on_confirm(conn, &u_sent);

	if (exp_done)
		ludlc_call_on_confirm(conn, &expired);

	return ret;
}

/**
 * @brief Ping timer callback.
 *
 * This function is invoked when the ping timer expires. It simply
 * requests a TX cycle, which will send a PING packet if the queue is
 * empty.
 *
 * @param arg Platform timer callback argument.
 */
void ludlc_ping_timer(ludlc_platform_timer_arg_t arg)
{
	struct ludlc_connection *conn = ludlc_timer_arg_to_conn(arg);
	LUDLC_LOG_TRACE("Ping time");
	ludlc_platform_request_tx(conn);
}

/**
 * @brief Watchdog (WD) timer callback.
 *
 * This function is invoked when the connection watchdog timer expires,
 * indicating a loss of contact with the remote peer.
 *
 * @param arg Platform timer callback argument.
 */
static void ludlc_wd_timer(ludlc_platform_timer_arg_t arg)
{
	struct ludlc_connection *conn = ludlc_timer_arg_to_conn(arg);
	LUDLC_LOG_TRACE("Conn timeout");
	ludlc_platform_conn_timeout(conn);
}

/**
 * @brief Executes the common core disconnect transition and confirm flush.
 *
 * This helper performs the lock-protected disconnect state transition through
 * @ref on_disconnect_lock and then dispatches deferred confirmation callbacks
 * via @ref ludlc_confirm_unlocked.
 *
 * If the connection is already disconnected (or terminating), it is a no-op.
 *
 * @param conn Pointer to the connection structure.
 * @param confirmed_q Temporary confirmation queue (used in MT builds, ignored
 * in non-MT builds).
 * @param confirmed_num Pointer to the number of queued confirmations.
 *
 * @retval true Disconnect transition was executed and caller may notify
 * `on_disconnect`.
 * @retval false No transition was needed (invalid input or already disconnected).
 *
 * @note User-facing callbacks are dispatched outside `conn->lock`.
 */
static bool ludlc_disconnect_core(struct ludlc_connection *conn,
		struct packet_queue_item *confirmed_q,
		ludlc_id_t *confirmed_num)
{
	bool notify = false;

	if (!conn || !confirmed_num) {
		return false;
	}

	LUDLC_LOCK(&conn->lock);
	if (conn->conn_state != LUDLC_STATE_DISCONNECTED &&
	    conn->conn_state != LUDLC_STATE_TERMINATE) {
		on_disconnect_lock(conn, confirmed_q, confirmed_num);
		notify = true;
	}
	LUDLC_UNLOCK(&conn->lock);

	ludlc_confirm_unlocked(conn, confirmed_q, *confirmed_num);
	return notify;
}

/**
 * @brief Handles a transport-originated disconnect in core context.
 *
 * This function performs the full disconnect sequence in a state-safe way:
 * - transitions the FSM to disconnected state,
 * - fails pending queued TX packets through the regular confirm path,
 * - invokes the user `on_disconnect` callback once, outside the connection lock.
 *
 * Repeated calls are idempotent: if the connection is already disconnected
 * (or terminating), no additional callback notification is emitted.
 *
 * @param conn Pointer to the connection structure.
 *
 * @note The user callback is intentionally called outside `conn->lock` to
 * avoid deadlocks and re-entrancy issues.
 */
void ludlc_handle_disconnect(struct ludlc_connection *conn)
{
	LUDLC_DECLARE_CONFIRMED_QUEUE(confirmed_q);
	ludlc_id_t confirmed_num = 0;
	bool notify = ludlc_disconnect_core(conn, confirmed_q, &confirmed_num);

	if (notify && conn) {
		ludlc_call_on_disconnect(conn);
	}
}

/**
 * @brief Cleans up a connection structure.
 *
 * @param conn Pointer to the connection structure to be zeroed.
 */
void ludlc_connection_cleanup(struct ludlc_connection *conn)
{
	LUDLC_DECLARE_CONFIRMED_QUEUE(confirmed_q);
	ludlc_id_t confirmed_num = 0;

	if (!conn ||
		!ludlc_platform_test_bit(LUDLC_CONN_INITED_F, &conn->flags)) {
		return;
	}

	(void)ludlc_disconnect_core(conn, confirmed_q, &confirmed_num);

	ludlc_platform_destroy_timer(&conn->wd_timer);
	ludlc_platform_destroy_timer(&conn->ping_timer);

	ludlc_platform_conn_destroy(conn);
}

/**
 * @brief Initializes a LuDLC connection structure.
 *
 * Prepares a `ludlc_connection` structure for use. This includes
 * zeroing the memory, storing user contexts and callbacks, initializing
 * timers, and binding the protocol cfg.
 *
 * @param conn Pointer to the connection structure to initialize.
 * @param cfg What to open the link with: its clock, or NULL for
 * @ref ludlc_conn_cfg_default. Copied here, so a compound literal will do;
 * only the clock pointer is kept and must outlive the connection.
 * @param cb Pointer to the user-facing event callbacks (e.g., on_connect,
 * on_recv).
 * @param user_ctx A user-defined context pointer that will be passed back
 * in all callbacks.
 *
 * @return 0 on success.
 * @return -EINVAL if `conn` is NULL.
 */
int ludlc_connection_init(struct ludlc_connection *conn,
		const struct ludlc_conn_cfg *cfg,
		const struct ludlc_conn_cb *cb,
		void *user_ctx)
{
	int ret;

	if (!conn)
		return -EINVAL;

	if (!cfg)
		cfg = &ludlc_conn_cfg_default;

	memset(conn, 0, sizeof(*conn));

	/*
	 * A setting the caller left at zero takes the build default.
	 * Resolved here, once, rather than at every read afterwards.
	 */
	conn->cfg.get_timestamp =
		cfg->get_timestamp ? cfg->get_timestamp
			   : ludlc_default_get_timestamp;

	conn->cb = cb;
	conn->user_ctx = user_ctx;
	conn->ctrl_packet.chan = CONFIG_LUDLC_CONTROL_CHANNEL;
	conn->conn_state = LUDLC_STATE_DISCONNECTED;

	conn->flags = LUDLC_CONN_IDLE_MASK;

	ret = ludlc_platform_conn_init(conn);
	if (ret)
		return ret;

	/* Initialize platform-specific timers */
	ret = ludlc_platform_init_timer(conn, &conn->ping_timer,
			ludlc_ping_timer);
	if (!ret) {
		ret = ludlc_platform_init_timer(conn, &conn->wd_timer,
				ludlc_wd_timer);
		if (!ret) {
			ludlc_platform_set_bit(LUDLC_CONN_INITED_F,
					&conn->flags);
			return 0;
		}
		ludlc_platform_destroy_timer(&conn->ping_timer);
	}

	return ret;
}

const struct ludlc_conn_cfg ludlc_conn_cfg_default = LUDLC_CONN_CFG_DEFAULT;

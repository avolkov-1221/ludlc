// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file ludlc.c
 *
 * @brief LuDLC core logic implementation.
 *
 * This file contains the core state machine, packet handling (receive and
 * transmit), and connection management logic for the LuDLC protocol.
 *
 * Copyright (C) 2025 Andrey VOLKOV <andrey@volkov.fr> and LuDLC Contributors
 *
 * This file is licensed under either the Apache License, Version 2.0,
 * or the GNU General Public License, version 2 or (at your option)
 * any later version.
 */

#include <error.h>
#include <errno.h>

#include <ludlc.h>
#include <ludlc_private.h>
#include <ludlc_logger.h>

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

/**
 * @brief Internal handler for connection establishment.
 *
 * Increments connection statistics and invokes the user's `on_connect`
 * callback, if provided.
 *
 * @note This function must be called with the connection lock held.
 *
 * @param conn Pointer to the connection structure.
 */
static inline void on_connect_lock(struct ludlc_connection *conn)
{
	LUDLC_INC_STATS(conn, on_conn);

	if (conn->cb->on_connect)
		conn->cb->on_connect(conn, conn->user_ctx);
}

#ifdef CONFIG_LUDLC_MT
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
	if (conn->cb->on_confirm) {
		ludlc_id_t i;
		for (i = 0; i < confirmed_num; i++) {
			conn->cb->on_confirm(conn,
					cq[i].status.result,
					cq[i].status.chan,
					cq[i].payload,
					cq[i].size,
					conn->user_ctx);
		}
	}
}
#else
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
	if(conn->cb->on_confirm)
		conn->cb->on_confirm(conn,
			pkt->status.result,
			pkt->status.chan,
			pkt->payload,
			pkt->size,
			conn->user_ctx);
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
static void ludlc_confirm_unlocked(struct ludlc_connection *conn,
		struct packet_queue_item *cq, ludlc_id_t confirmed_num)
{
}

#endif


/**
 * @brief Internal handler for disconnect case.
 *
 * Stops all timers, sets the state to DISCONNECTED, and fails all
 * outstanding packets in the transmit queue by calling
 * @ref ludlc_on_confirm_lock with an EPIPE error.
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
	conn->flags = 0;

	/* Calculate number of packets waiting for ACK */
	cnt = (conn->last_queued - conn->last_ack) & LUDLC_WINDOW_MASK;

	/* Iterate and fail all pending packets */
	for (i = conn->last_ack; cnt; cnt--) {
		i = (i + 1) & LUDLC_WINDOW_MASK;
		conn->packets_q[i].status.result = EPIPE;
		ludlc_on_confirm_lock(
			conn,
			&conn->packets_q[i], cq, confirmed_num);
	}

	/* Reset queue pointers */
	conn->last_sent = 0;
	conn->last_queued = 0;
	conn->last_ack = 0;
	conn->last_received = 0;

	LUDLC_INC_STATS(conn, on_disconn);
}

/**
 * @brief Requests an immediate transmission.
 *
 * This function signals the LuDLC transmit task/thread (if any) to wake
 * up and attempt to send a packet. It sets a flag to force transmission
 * even if no new data has been queued (e.g., to send a PING with ACK).
 *
 * @param conn Pointer to the connection structure.
 */
void ludlc_request_tx(struct ludlc_connection *conn)
{
	LUDLC_WQ_WAKEUP(conn->tx_wq,
		ludlc_platform_set_bit(LUDLC_CONN_FORCE_TX_F, &conn->flags));
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
 * @param oneshot If true, the packet will only be tried once (TTL=1).
 * If false, it uses the default retry mechanism (LUDLC_MAX_TTL).
 * @return 0 on success.
 * @return -EAGAIN if the transmit queue is full.
 */
int ludlc_enqueue_data(struct ludlc_connection *conn,
		ludlc_channel_t dst_chan,
		const void *buf, ludlc_payload_size_t size, bool oneshot)
{
	int ret;

	LUDLC_LOCK(&conn->lock);
	if (!ludlc_tx_queue_full(conn)) {
		/* Calculate next queue ID */
		ludlc_id_t id = (conn->last_queued + 1) & LUDLC_ID_MASK;
		ludlc_id_t q_idx = id & LUDLC_WINDOW_MASK;

		/* Populate the queue slot */
		conn->packets_q[q_idx].hdr.chan = dst_chan;
		conn->packets_q[q_idx].ttl = oneshot ? 1 : LUDLC_MAX_TTL;
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
		ludlc_request_tx(conn); /* Signal TX task */

	return ret;
}

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
		ludlc_packet_t *packet)
{
	ludlc_id_t id = packet->hdr.id.tx_id;
	int ret = HANDLE_RX_REQUEST_TX_F; /* Default to sending a response */

	/* Check for unexpected ping or non-control packets. */
	if (is_ping(id) ||
		packet->hdr.chan != CONFIG_LUDLC_CONTROL_CHANNEL) {
		/* If our LuDLC FSM in WAIT_CONN_2 state of the handshake, then
		 * other side correctly transferred to the CONNECTED one and
		 * start sending us normal packets, so we can do the same */
		if (conn->conn_state == LUDLC_STATE_WAIT_CONN_2) {
			conn->conn_state = LUDLC_STATE_CONNECTED;
			on_connect_lock(conn);
			/* and demands to continue handle the packet's header
			 * as a normal one */
			return ret | HANDLE_RX_CONTINUE_F;
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
			/* This is the expected next handshake step */
			if (++conn->conn_state == LUDLC_STATE_CONNECTED)
				on_connect_lock(conn);
		} else if ((id + 1) != conn->conn_state) {
			/* Check for an ID that is too far off
			 * (not equal to conn_state - 1). Probably due to
			 * lost of some handshake packets.
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
 * This function is called when a NAK is received. It iterates
 * through the unacknowledged packets in the queue:
 * 1. Decrements the TTL of each packet.
 * 2. If TTL reaches 0, the packet is marked as failed (EREMOTEIO) and
 *    moved to the confirmed queue.
 * 3. Other packets are compacted to the front of the un-ACK'd window.
 *
 * Finally, it resets `last_sent` to `last_ack` to force a re-transmission
 * of all remaining unacknowledged packets.
 *
 * @param conn Pointer to the connection structure.
 * @param packet The received packet that triggered the NAK (unused).
 * @param num_wait The number of packets waiting for acknowledgment.
 * @param cq Temporary queue for confirmed (failed) packets (for MT build).
 * @param confirmed_num Pointer to counter for `cq` (for MT build).
 * @return @ref HANDLE_RX_REQUEST_TX_F to signal an immediate re-transmission.
 */
static int rx_handle_nak(
		struct ludlc_connection *conn,
		ludlc_packet_t *packet,
		ludlc_id_t num_wait,
		struct packet_queue_item *cq,
		ludlc_id_t *confirmed_num)
{
	ludlc_id_t wr_idx = (conn->last_ack + 1) & LUDLC_ID_MASK;
	ludlc_id_t id = wr_idx;

	/* Remove expired packages from the queue */
	for (; num_wait; num_wait--) {
		ludlc_id_t q_idx = id & LUDLC_WINDOW_MASK;

		if (--conn->packets_q[q_idx].ttl) {
			/* Put the item to the new place, */
			/* and fill the gap */
			if (wr_idx != id) {
				ludlc_id_t q_wr_idx =
					wr_idx & LUDLC_WINDOW_MASK;
				conn->packets_q[q_wr_idx] =
						conn->packets_q[q_idx];
			}
			wr_idx = (wr_idx + 1) & LUDLC_ID_MASK;
		} else {
			/* Packet expired (TTL=0) */
			conn->packets_q[q_idx].status.result = EREMOTEIO;
			ludlc_on_confirm_lock(conn,
					&conn->packets_q[q_idx],
					cq, confirmed_num);
		}

		id = (id + 1) & LUDLC_ID_MASK;
	}

	if (wr_idx != id) {
		/* Move the rest of the packets in the queue to fill the gaps */
		num_wait = (conn->last_queued - wr_idx) & LUDLC_ID_MASK;

		for (; num_wait; num_wait--) {
			ludlc_id_t q_idx = id & LUDLC_WINDOW_MASK;

			/* Put the item to the new place */
			ludlc_id_t q_wr_idx = wr_idx & LUDLC_WINDOW_MASK;
			conn->packets_q[q_wr_idx] = conn->packets_q[q_idx];

			wr_idx = (wr_idx + 1) & LUDLC_ID_MASK;
			id = (id + 1) & LUDLC_ID_MASK;
		}

		conn->last_queued = wr_idx;
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
 * 3. Handles control channel packets (e.g., disconnect).
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
		/* Something strange was happened on other side:
		 * the packet is too short, but with good crc,
		 * drop it for a while */
		LUDLC_INC_STATS(conn, dropped);
		return 0;
	}

	/* OK, the packet's integrity looks good,
	 * so start handling the packet. */
	if (conn->conn_state != LUDLC_STATE_CONNECTED) {
		ret = rx_handle_handshake(conn, packet);
		if ((ret & HANDLE_RX_CONTINUE_F) == 0) {
			/* Handshake not complete, stop further processing */
			return ret;
		}
		/* Handshake just completed, continue processing this packet */
		ret &= ~HANDLE_RX_CONTINUE_F;
	} else {
		ret = 0;
	}

	id = packet->hdr.id.tx_id & ~LUDLC_PING_FLAG;

	/* Handle "tx_id" of the packet */
	if (!ping && packet->hdr.chan == CONFIG_LUDLC_CONTROL_CHANNEL) {
		/* Received a control packet while connected */
		if (id != LUDLC_STATE_WAIT_CONN_2) {
			/* Any control packet other than last handshake step */
			on_disconnect_lock(conn, confirmed_q, confirmed_num);
		}
		/* Respond to disconnect/handshake */
		return HANDLE_RX_REQUEST_TX_F;
	}

	/* Any valid packet (even ping) resets the connection watchdog */
	ludlc_platform_start_timer(&conn->wd_timer,
			3 * CONFIG_LUDLC_PING_TIME,
			0);

	/* 1. check the correctness of the incoming packet's sequence. */
	next_rcvd_id = (conn->last_received + 1) & LUDLC_ID_MASK;
	if (next_rcvd_id != id) {
		if (conn->last_received != id) {
			/* The packet with unpredicted ID arrived (out of order)
			 * So, set the flag to resend unndelivered
			 * request(s) (NAK)
			 */
			ludlc_platform_set_bit(LUDLC_CONN_SEND_NAK_F,
					&conn->flags);
			ret |= HANDLE_RX_REQUEST_TX_F;
		}
		/* However, if conn->last_received == id, it's mean
		 * that this packet has been sent and received already by
		 * ourselves, but the sender didn't received ack yet
		 * (very rare case), so do nothing for it.
		 * (this is a duplicate). Drop it, but still process
		 * its ACK field.
		 */
	} else if(!ping) {
		/* The normal packet arrived with correct id's sequence,
		 * so update "last_received" counter, request to send ack and
		 * accept the packet
		 */
		conn->last_received = next_rcvd_id;
		ret |= HANDLE_RX_REQUEST_TX_F | HANDLE_RX_ACCEPT_F;
	}

	/* 2. now process ack_id for our side */
	/* Get the number of packets are waiting for acknowledge */
	num_wait = (conn->last_sent - conn->last_ack) & LUDLC_ID_MASK;

	/* Get the number of confirmed packets.
	 *
	 * For the UNSIGNED integers like ludlc_id_t already is, it's useless
	 * to clear the highest bit in separate operation, the result of
	 * subtraction will be corrected by masking.
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

		/* Check if the NAK flag is set in the ACK ID, and handle it */
		if (packet->hdr.id.ack_id & LUDLC_NAK_FLAG)
			ret = rx_handle_nak(conn, packet, num_wait, confirmed_q,
					confirmed_num);

	} else {
		// TODO:
		/* Wow, this is strange case, peer ACK'd packets we haven't sent.
		 * This implies a severe state desynchronization.
		 */
		/* reconnect immediately ? */
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
 * @param tstamp A timestamp of when the packet was received.
 * @return 0 on success.
 */
int ludlc_receive(struct ludlc_connection *conn,
			ludlc_packet_t *packet, size_t pkt_sz,
			ludlc_timestamp_t tstamp)
{
	int ret;
#ifdef CONFIG_LUDLC_MT
	/* On-stack queue for MT builds to defer callbacks */
	struct packet_queue_item confirmed_q[CONFIG_LUDLC_WINDOW];
#else
	/* Not used in non-MT */
	struct packet_queue_item *confirmed_q = NULL;
#endif
	ludlc_id_t confirmed_num = 0;

	LUDLC_LOG_DEBUG("packet (%x, %x) @%llu",
			packet->hdr.id.tx_id,
			packet->hdr.id.ack_id,
			tstamp);

	LUDLC_LOCK(&conn->lock);
		ret = rx_handle_packet(conn, packet, pkt_sz,
				confirmed_q, &confirmed_num);
	LUDLC_UNLOCK(&conn->lock);

	/* --- Actions outside the lock --- */

	if(ret & HANDLE_RX_REQUEST_TX_F)
		ludlc_request_tx(conn);  /* Signal TX task */

	/* Call deferred on_confirm callbacks (for MT) */
	ludlc_confirm_unlocked(conn, confirmed_q, confirmed_num);

	/* Call "on_recv" callback if packet is ok */
	if (ret & HANDLE_RX_ACCEPT_F) {
		if(conn->cb->on_recv)
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
	uint8_t ack, id, q_idx;
	int ret = 1; /* Assume this is the last packet by default */

	if (!hdr || !hdr_sz || !payload || !pay_sz)
		return -EINVAL;

	LUDLC_LOCK(&conn->lock);

	switch (conn->conn_state) {
	default:/* LUDLC_STATE_DISCONNECTED, LUDLC_STATE_WAIT_CONN_1, etc. */

		/* Send handshake control packet */
		conn->ctrl_packet.chan = CONFIG_LUDLC_CONTROL_CHANNEL;
		conn->ctrl_packet.id.tx_id = conn->conn_state;
		/* No ACK during handshake */
		conn->ctrl_packet.id.ack_id = 0;

		*hdr = &conn->ctrl_packet.id;
		*hdr_sz = sizeof(conn->ctrl_packet);

		*payload = NULL;
		*pay_sz = 0;

		break;

	case LUDLC_STATE_CONNECTED:
		/* Prepare ACK ID */
		ack = conn->last_received;

		/* Check if a NAK needs to be sent */
		if (ludlc_platform_test_and_clear_bit(LUDLC_CONN_SEND_NAK_F,
				&conn->flags)) {
			ack |= LUDLC_NAK_FLAG;
		}

		/* Get next TX ID */
		id = (conn->last_sent + 1) & LUDLC_ID_MASK;

		if (conn->last_sent == conn->last_queued) {
			/* if there are no pending packets, */
			/* then send a ping frame */
			conn->ctrl_packet.id.tx_id = id | LUDLC_PING_FLAG;
			conn->ctrl_packet.id.ack_id = ack;
			*hdr = &conn->ctrl_packet.id;
			/* Only ID fields of header */
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

	return ret;
}

/**
 * @brief Ping timer callback.
 *
 * This function is invoked when the ping timer expires. It simply
 * requests a TX cycle, which will send a PING packet if the queue is
 * empty.
 *
 * @param conn Pointer to the connection structure (passed from timer context).
 */
static void ludlc_ping_timer(struct ludlc_connection *conn)
{
	LUDLC_LOG_DEBUG("Ping time");
	ludlc_request_tx(conn);
}

/**
 * @brief Watchdog (WD) timer callback.
 *
 * This function is invoked when the connection watchdog timer expires,
 * indicating a loss of contact with the remote peer. It sets the
 * timeout flag, which will be handled by the protocol's main loop
 * or polling function.
 *
 * @param conn Pointer to the connection structure (passed from timer context).
 */
static void ludlc_wd_timer(struct ludlc_connection *conn)
{
	ludlc_platform_set_bit(LUDLC_CONN_TIMEOUT_F, &conn->flags);
}

/**
 * @brief Cleans up and zeroes a connection structure.
 *
 * @param conn Pointer to the connection structure to be zeroed.
 */
void ludlc_connection_cleanup(struct ludlc_connection *conn)
{
#ifdef CONFIG_LUDLC_MT
	/* On-stack queue for MT builds to defer callbacks */
	struct packet_queue_item confirmed_q[CONFIG_LUDLC_WINDOW];
#else
	/* Not used in non-MT */
	struct packet_queue_item *confirmed_q = NULL;
#endif
	ludlc_id_t confirmed_num = 0;

	if (ludlc_platform_test_bit(LUDLC_CONN_INITED_F, &conn->flags)) {
		LUDLC_LOCK(&conn->lock);
		on_disconnect_lock(conn, confirmed_q, &confirmed_num);
		LUDLC_UNLOCK(&conn->lock);

		/* Call deferred on_confirm callbacks (for MT) */
		ludlc_confirm_unlocked(conn, confirmed_q, confirmed_num);

		ludlc_platform_destroy_timer(&conn->wd_timer);
		ludlc_platform_destroy_timer(&conn->ping_timer);

	}

	memset(conn, 0, sizeof(*conn));
}

/**
 * @brief Initializes a LuDLC connection structure.
 *
 * Prepares a `ludlc_connection` structure for use. This includes
 * zeroing the memory, storing user contexts and callbacks, initializing
 * timers, and setting up the TX FIFO.
 *
 * @param conn Pointer to the connection structure to initialize.
 * @param proto_cb Pointer to the platform/transport callbacks (e.g.,
 * tx_write, rx_read).
 * @param cb Pointer to the user-facing event callbacks (e.g., on_connect,
 * on_recv).
 * @param user_ctx A user-defined context pointer that will be passed back
 * in all callbacks.
 *
 * @return 0 on success.
 * @return -EINVAL if `conn` or `proto_cb` are NULL, or if mandatory
 * `proto_cb` functions are missing.
 */
int ludlc_connection_init(struct ludlc_connection *conn,
		const struct ludlc_proto_cb *proto_cb,
		const struct ludlc_conn_cb *cb,
		void *user_ctx)
{
	int ret;

	if (!conn || !proto_cb)
		return -EINVAL;

	/* The "write", "read" and "get_timestamp" callbacks are obligatory */
	if(!proto_cb->tx_write ||
		!proto_cb->rx_read ||
		!proto_cb->get_timestamp)
		return -EINVAL;

	memset(conn, 0, sizeof(*conn));

	conn->proto = proto_cb;
	conn->cb = cb;
	conn->user_ctx = user_ctx;

	conn->ctrl_packet.chan = CONFIG_LUDLC_CONTROL_CHANNEL;
	conn->conn_state = LUDLC_STATE_DISCONNECTED;

	LUDLC_KFIFO_INIT(&conn->tx_fifo, conn->tx_buf, sizeof(conn->tx_buf));

	/* Initialize platform-specific timers */
	ret = ludlc_platform_init_timer(conn, &conn->ping_timer,
			ludlc_ping_timer);

	if (!ret) {
		ret = ludlc_platform_init_timer(conn, &conn->wd_timer,
				ludlc_wd_timer);
		if(!ret) {
			ludlc_platform_set_bit(LUDLC_CONN_INITED_F,
					&conn->flags);
			return 0;
		}

		ludlc_platform_destroy_timer(&conn->ping_timer);
	}

	return ret;
}


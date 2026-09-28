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
 * @brief Tells the peer's last handshake step from a peer that has restarted.
 *
 * Both arrive as an empty control packet. A restarted peer begins again from
 * the first step; the last step is one this side has already handled, and
 * arrives again only when the reply to it was lost. Ending the session over
 * it would undo a handshake that has just completed. A deliberate disconnect
 * carries @ref LUDLC_CTRL_DISCONNECT on an unnumbered packet, so the step ID
 * separates the two remaining cases exactly.
 *
 * @param id Sequence number of the received packet.
 * @return true if the packet is the peer's last handshake step.
 */
static inline bool is_last_handshake_step(ludlc_id_t id)
{
	return id == LUDLC_STATE_WAIT_CONN_2;
}

/**
 * @brief Allocates the connection's sliding window.
 *
 * Only a multi-connection build allocates, the window size being a runtime
 * value there; a single-connection build has the array as a member.
 *
 * @param conn The connection, with its cfg already resolved.
 * @return 0, or -ENOMEM.
 */
static int ludlc_conn_queue_alloc(struct ludlc_connection *conn)
{
#if CONFIG_LUDLC_MULTI_CONN
	ludlc_id_t wnd = ludlc_conn_window(conn);

	conn->packets_q =
		ludlc_platform_alloc(wnd * sizeof(*conn->packets_q));
	if (!conn->packets_q) {
		return -ENOMEM;
	}

#else /* one link: the window is a member, sized by the build's constant */
	(void)conn;
#endif
	return 0;
}

/** @brief Frees what @ref ludlc_conn_queue_alloc allocated. */
static void ludlc_conn_queue_free(struct ludlc_connection *conn)
{
#if CONFIG_LUDLC_MULTI_CONN
	ludlc_platform_free(conn->packets_q);
	conn->packets_q = NULL;
#else
	(void)conn;
#endif
}

/**
 * @brief Checks if the transmit (TX) queue is empty.
 *
 * @param conn Pointer to the connection structure.
 * @return true if no packets are queued for transmission, false otherwise.
 */
static inline bool ludlc_tx_queue_empty(struct ludlc_connection *conn)
{
	if ((conn->last_queued - conn->last_ack) & LUDLC_ID_MASK) {
		return false;
	}
	return true;
}

/**
 * @brief Checks if the transmit (TX) queue is full (window is saturated).
 *
 * A slot holds its packet until the confirmation has been reported, not
 * merely decided, so the count runs from @ref ludlc_conn_last_reported rather
 * than @c last_ack and the window cannot advance onto a slot whose callback
 * has yet to run. A build that reports inline makes the two the same, leaving
 * the plain window check.
 *
 * @param conn Pointer to the connection structure.
 * @return true if the unacknowledged packets, together with those resolved
 * but not yet reported, have reached the window size, false otherwise.
 */
static inline bool ludlc_tx_queue_full(struct ludlc_connection *conn)
{
	if (((conn->last_queued -
		ludlc_conn_last_reported(conn)) & LUDLC_ID_MASK) >=
				ludlc_conn_window(conn)) {
		return true;
	}

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

/**
 * @brief Resets the sequence once nothing is left to report.
 *
 * The disconnect sweep cannot clear the counters itself, because a resolved
 * packet is reported from the slot it was sent from and the counters are what
 * identify those slots. The sweep therefore sets @ref LUDLC_CONN_BUS_OUT_F
 * and calls this, as does every pass of @ref ludlc_confirm_unlocked; the call
 * that finds nothing pending does the reset and reopens the queue. In a build
 * that reports inline that is always the sweep.
 *
 * @note This function must be called with the connection lock held.
 *
 * @param conn Pointer to the connection structure.
 */
static void ludlc_seq_reset_if_drained(struct ludlc_connection *conn)
{
	if (!ludlc_platform_test_bit(LUDLC_CONN_BUS_OUT_F, &conn->flags)) {
		return;
	}

#ifdef CONFIG_LUDLC_MT
	if (conn->last_reported != conn->last_ack ||
	    ludlc_platform_test_bit(LUDLC_CONN_U_CONFIRM_F, &conn->flags)) {
		return;
	}

	conn->last_reported = 0;
#endif

	conn->last_sent = 0;
	conn->last_queued = 0;
	conn->last_ack = 0;
	conn->last_received = 0;

	ludlc_platform_clear_bit(LUDLC_CONN_BUS_OUT_F, &conn->flags);
}

#ifdef CONFIG_LUDLC_MT
/**
 * @brief (MT) Leaves a resolved packet for @ref ludlc_confirm_unlocked.
 *
 * Nothing is copied or queued: the item stays in its slot, and @c last_ack
 * having moved past it is what marks the callback as pending. The window
 * cannot reach that slot first; see @ref ludlc_tx_queue_full.
 *
 * @note This function must be called with the connection lock held, and the
 * caller must have written @c status.result into the slot.
 *
 * @param conn Pointer to the connection structure.
 * @param pkt The slot that has been resolved.
 */
static inline void ludlc_confirm_queued_lock(struct ludlc_connection *conn,
				const struct packet_queue_item *pkt)
{
	(void)conn;
	(void)pkt;
}

/**
 * @brief (MT) Saves the unnumbered packet's confirmation to be reported.
 *
 * It has no window slot, so the range cannot describe it. Copied rather than
 * referenced, because @ref ludlc_send_disconnect reuses @c u_packet for the
 * next packet in the very next statement.
 *
 * @note This function must be called with the connection lock held, and the
 * caller must have written @c status.result into @c u_packet.
 *
 * @param conn Pointer to the connection structure.
 */
static inline void ludlc_confirm_u_lock(struct ludlc_connection *conn)
{
	conn->u_confirm = conn->u_packet;
	ludlc_platform_set_bit(LUDLC_CONN_U_CONFIRM_F, &conn->flags);
}

/**
 * @brief (MT) Runs the callbacks the lock decided on, with the lock released.
 *
 * Each pass copies up to @ref CONFIG_LUDLC_CONFIRM_BATCH of the pending range
 * <tt>(last_reported, last_ack]</tt> onto the stack, releases the lock and
 * runs their callbacks, so the stack is bounded by the batch and not by the
 * window. The batch needs no locking, being on the reporting thread's stack
 * and nowhere else; @ref LUDLC_REPORT_BUSY_F only fixes the order the
 * callbacks run in. A caller that finds it set returns, leaving its items to
 * the thread holding it.
 *
 * @note This function must be called *without* the connection lock held.
 *
 * @param conn Pointer to the connection structure.
 */
static void ludlc_confirm_unlocked(struct ludlc_connection *conn)
{
	if (ludlc_platform_test_and_set_bit(LUDLC_REPORT_BUSY_F,
					    &conn->report_busy)) {
		return;
	}

	for (;;) {
		struct packet_queue_item batch[CONFIG_LUDLC_CONFIRM_BATCH];
		struct packet_queue_item u_pkt;
		bool u_done = false;
		unsigned int n = 0;
		unsigned int i;

		LUDLC_LOCK(&conn->lock);

		if (ludlc_platform_test_bit(LUDLC_CONN_U_CONFIRM_F,
					    &conn->flags)) {
			u_pkt = conn->u_confirm;
			ludlc_platform_clear_bit(LUDLC_CONN_U_CONFIRM_F,
						 &conn->flags);
			u_done = true;
		}

		while (n < CONFIG_LUDLC_CONFIRM_BATCH &&
		       conn->last_reported != conn->last_ack) {
			conn->last_reported = (conn->last_reported + 1) &
					      LUDLC_ID_MASK;
			batch[n++] = conn->packets_q[
				conn->last_reported &
				ludlc_conn_window_mask(conn)];
		}

		ludlc_seq_reset_if_drained(conn);

		if (!n && !u_done) {
			/*
			 * Nothing left, so the flag is cleared here rather
			 * than after the unlock. A packet is resolved under
			 * this same lock and its thread tests the flag
			 * afterwards, so it either appears in a batch above
			 * or finds the flag clear and becomes the reporter.
			 * Cleared outside the lock, the two can miss each
			 * other and the confirmation is never reported.
			 */
			ludlc_platform_clear_bit(LUDLC_REPORT_BUSY_F,
						 &conn->report_busy);
			LUDLC_UNLOCK(&conn->lock);

			return;
		}

		LUDLC_UNLOCK(&conn->lock);

		if (u_done) {
			ludlc_call_on_confirm(conn, &u_pkt);
		}

		for (i = 0; i < n; i++) {
			ludlc_call_on_confirm(conn, &batch[i]);
		}
	}
}
#else
/**
 * @brief (non-MT) Immediately invokes the user confirmation callback.
 *
 * With no other thread to order against, the callback runs where the
 * confirmation is decided.
 *
 * @note This function must be called with the connection lock held.
 *
 * @param conn Pointer to the connection structure.
 * @param pkt The packet queue item that has been confirmed.
 */
static void ludlc_confirm_queued_lock(struct ludlc_connection *conn,
				const struct packet_queue_item *pkt)
{
	ludlc_call_on_confirm(conn, pkt);
}

/** @brief (non-MT) Immediately confirms the unnumbered packet. */
static inline void ludlc_confirm_u_lock(struct ludlc_connection *conn)
{
	ludlc_call_on_confirm(conn, &conn->u_packet);
}

/**
 * @brief (non-MT) Stub function.
 *
 * Confirmations are reported in @ref ludlc_confirm_queued_lock as they are
 * decided, so nothing is ever left to report here.
 *
 * @param conn Unused.
 */
static inline void ludlc_confirm_unlocked(struct ludlc_connection *conn)
{
	(void)conn;
}

#endif

/**
 * @brief Internal handler for disconnect case.
 *
 * Stops all timers, sets the state to DISCONNECTED, and fails every
 * outstanding packet with EPIPE. That covers the transmit queue and the
 * unnumbered packet, if one is still waiting to be sent.
 *
 * The sequence is reset once all of that has been reported, which is not
 * necessarily here; the queue stays closed until it is. See
 * @ref LUDLC_CONN_BUS_OUT_F and @ref ludlc_seq_reset_if_drained.
 *
 * @note This function must be called with the connection lock held.
 *
 * @param conn Pointer to the connection structure.
 */
static void on_disconnect_lock(struct ludlc_connection *conn)
{
	ludlc_id_t i, cnt;

	LUDLC_LOG_DEBUG("on_disconnect_lock");

	ludlc_platform_stop_timer(&conn->wd_timer);
	ludlc_platform_stop_timer(&conn->ping_timer);

	conn->conn_state = LUDLC_STATE_DISCONNECTED;

	/* Everything below is failed to the user, and the sequence resets
	 * once it has all been reported. A packet accepted in between would
	 * be lost to that reset, so the queue is closed until then.
	 */
	ludlc_platform_set_bit(LUDLC_CONN_BUS_OUT_F, &conn->flags);

	/* The unnumbered packet is not part of the window, so it is not among
	 * those failed below, but its buffer must be returned all the same.
	 * A DISCONNECT is the exception: it belongs to the core, and no
	 * caller is waiting for it.
	 */
	if ((!ludlc_platform_test_bit(LUDLC_CONN_U_IDLE_F, &conn->flags) ||
			!ludlc_platform_test_bit(LUDLC_CONN_U_DONE_F,
				&conn->flags)) &&
			!ludlc_platform_test_bit(LUDLC_CONN_DISC_AFTER_TX_F,
				&conn->flags)) {
		conn->u_packet.status.result = EPIPE;
		ludlc_confirm_u_lock(conn);
	}

	/* Keep the lifecycle bit, this sweep and any confirmation it just
	 * saved, and clear the runtime protocol flags. That frees the
	 * unnumbered packet slot confirmed above, which means setting the
	 * active-low flags rather than clearing them. The idle ones go first,
	 * so a reader between the two sees the slot free rather than a
	 * disconnect still owing a confirmation.
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
		struct packet_queue_item *it;

		i = (i + 1) & LUDLC_ID_MASK;
		it = &conn->packets_q[i & ludlc_conn_window_mask(conn)];
		it->status.result = EPIPE;

		ludlc_confirm_queued_lock(conn, it);
	}

	/* Everything the window held is resolved now and pending a callback:
	 * the range the drain walks runs up to last_ack.
	 */
	conn->last_ack = conn->last_queued;

	ludlc_seq_reset_if_drained(conn);

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
 * above what this link carries.
 * @return -EAGAIN if the transmit queue is full, or if a disconnect is
 * failing what the queue held and has yet to reset the sequence.
 */
int ludlc_enqueue_data(struct ludlc_connection *conn,
		ludlc_channel_t dst_chan,
		const void *buf, ludlc_payload_size_t size, uint8_t ttl)
{
	int ret;

	if (!conn || (!buf && size)) {
		return -EINVAL;
	}

	/* The control channel belongs to the core, which distinguishes a
	 * handshake step from a disconnect by the layout of the packet. A
	 * caller sending on it could imitate either one.
	 */
	if (dst_chan == CONFIG_LUDLC_CONTROL_CHANNEL) {
		return -EINVAL;
	}

	/* A larger payload would be sent and then dropped by the receiver,
	 * where it would look like a packet that never arrived.
	 */
	if (size > ludlc_conn_payload_lim(conn)) {
		return -EINVAL;
	}

	if (ttl > LUDLC_MAX_TTL) {
		ttl = LUDLC_MAX_TTL;
	} else if(ttl == 0) {
		ttl = 1;
	}

	LUDLC_LOCK(&conn->lock);
	if (ludlc_platform_test_bit(LUDLC_CONN_BUS_OUT_F, &conn->flags)) {
		/* A disconnect is failing the queue and is about to reset the
		 * sequence, so there is nowhere to put this yet.
		 */
		ret = -EAGAIN;
	} else if (!ludlc_tx_queue_full(conn)) {
		/* Calculate next queue ID */
		ludlc_id_t id = (conn->last_queued + 1) & LUDLC_ID_MASK;
		ludlc_id_t q_idx = id & ludlc_conn_window_mask(conn);

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
 * @return -EINVAL if @p buf is NULL, or @p size is 0 or above what this
 * link carries.
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
	    size > ludlc_conn_payload_lim(conn)) {
		return -EINVAL;
	}

	/* Reserved for the core, as for @ref ludlc_enqueue_data. */
	if (dst_chan == CONFIG_LUDLC_CONTROL_CHANNEL) {
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
	static const uint8_t opcode = LUDLC_CTRL_DISCONNECT;
	int ret;

	if (!conn) {
		return -EINVAL;
	}

	LUDLC_LOCK(&conn->lock);
	if (conn->conn_state != LUDLC_STATE_CONNECTED) {
		ret = -ENOTCONN;
	} else if (!ludlc_platform_test_bit(LUDLC_CONN_U_DONE_F,
				&conn->flags)) {
		/* The transport is reading that buffer as this runs, so the
		 * slot cannot be taken from it.
		 */
		ret = -EAGAIN;
	} else {
		/* A DISCONNECT replaces an unnumbered packet still waiting,
		 * since it is the last frame of the session in any case. The
		 * replaced buffer is returned to its owner through the usual
		 * path, unless it is a DISCONNECT already waiting, which
		 * belongs to the core and has no caller to report to.
		 */
		if (!ludlc_platform_test_bit(LUDLC_CONN_U_IDLE_F,
					     &conn->flags) &&
		    !ludlc_platform_test_bit(LUDLC_CONN_DISC_AFTER_TX_F,
					     &conn->flags)) {
			conn->u_packet.status.result = ECANCELED;
			ludlc_confirm_u_lock(conn);
		}

		conn->u_packet.hdr.chan = CONFIG_LUDLC_CONTROL_CHANNEL;
		conn->u_packet.payload = &opcode;
		conn->u_packet.size = sizeof(opcode);

		/* The core owns this packet: the flag below marks it as such,
		 * so its confirmation is not reported to the user but ends the
		 * session instead.
		 */
		ludlc_platform_set_bit(LUDLC_CONN_DISC_AFTER_TX_F,
				&conn->flags);
		ludlc_platform_clear_bit(LUDLC_CONN_U_IDLE_F, &conn->flags);

		ret = 0;
	}
	LUDLC_UNLOCK(&conn->lock);

	ludlc_confirm_unlocked(conn);

	if (!ret)
		ludlc_platform_request_tx(conn); /* Signal TX task */

	return ret;
}

int ludlc_connect(struct ludlc_connection *conn)
{
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
		on_disconnect_lock(conn);
	}
	LUDLC_UNLOCK(&conn->lock);

	ludlc_confirm_unlocked(conn);
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
	ludlc_id_t q_idx = id & ludlc_conn_window_mask(conn);

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
 * 3. Handles control channel packets: an announced disconnect, and an empty
 *    one, which is a handshake step from a peer that has restarted.
 * 4. Resets the connection watchdog timer.
 * 5. Validates the packet's TX ID sequence (`tx_id`).
 * 6. Sets flags for NAK or acceptance.
 * 7. Processes the acknowledgment ID (`ack_id`) to confirm sent packets.
 * 8. Calls @ref rx_handle_nak if a NAK flag is present.
 *
 * @param conn Pointer to the connection structure.
 * @param packet Pointer to the received LUDLC packet.
 * @param pkt_sz The total size of the received packet.
 * @return A bitmask of flags:
 * - @ref HANDLE_RX_REQUEST_TX_F to request a response (ACK/NAK/PING).
 * - @ref HANDLE_RX_ACCEPT_F to accept the packet's payload.
 */
static int rx_handle_packet(
		struct ludlc_connection *conn,
		ludlc_packet_t *packet,
		size_t pkt_sz)
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

	/* An unnumbered packet on the control channel carries an opcode. The
	 * layout is checked before the channel is read, because a PING ends
	 * after the two IDs and has no channel field.
	 */
	if (ludlc_is_unnumbered_data(packet->hdr.id.tx_id, pkt_sz) &&
	    packet->hdr.chan == CONFIG_LUDLC_CONTROL_CHANNEL &&
	    packet->payload[0] == LUDLC_CTRL_DISCONNECT) {
		/* The peer is closing the session deliberately, so it ends now
		 * rather than on the watchdog. Nothing is sent back: a
		 * handshake step would only reopen it, and a peer that comes
		 * back starts the handshake itself.
		 */
		on_disconnect_lock(conn);
		return ret | HANDLE_RX_DISCONNECT_F;
	}

	if (!ping && packet->hdr.chan == CONFIG_LUDLC_CONTROL_CHANNEL) {
		if (pkt_sz == sizeof(ludlc_packet_hdr_t)) {
			/* An empty control packet is a handshake step, so its
			 * sender has restarted and does not know this session
			 * is over.
			 */
			if (!is_last_handshake_step(id)) {
				on_disconnect_lock(conn);
				ret |= HANDLE_RX_DISCONNECT_F;
			}
			/* Respond to disconnect/handshake */
			return ret | HANDLE_RX_REQUEST_TX_F;
		}
	}

	/* Any valid packet (even ping) resets the connection watchdog */
	ludlc_watchdog_rearm(conn);

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
			q_idx = conn->last_ack & ludlc_conn_window_mask(conn);
			conn->packets_q[q_idx].status.result = 0;
			ludlc_confirm_queued_lock(conn,
						  &conn->packets_q[q_idx]);
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

	LUDLC_LOG_TRACE("packet (%x, %x) @%llu",
			packet->hdr.id.tx_id,
			packet->hdr.id.ack_id,
			tstamp);

	LUDLC_LOCK(&conn->lock);
		ret = rx_handle_packet(conn, packet, pkt_sz);
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
	ludlc_confirm_unlocked(conn);

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
	ludlc_id_t q_idx = id & ludlc_conn_window_mask(conn);
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

		conn->packets_q[id & ludlc_conn_window_mask(conn)] =
			conn->packets_q[next & ludlc_conn_window_mask(conn)];

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
	bool u_done = false, exp_done, disc_done = false;
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
		/* A DISCONNECT is the core's own packet: there is no buffer
		 * to return, and the session it closed ends here.
		 */
		disc_done = ludlc_platform_test_bit(LUDLC_CONN_DISC_AFTER_TX_F,
				&conn->flags);
		if (!disc_done) {
			conn->u_packet.status.result = 0;
			u_sent = conn->u_packet;
			u_done = true;
		}
	}

	if (disc_done) {
		LUDLC_UNLOCK(&conn->lock);

		/* Nothing is prepared for the transport: a session that has
		 * just sent its DISCONNECT has no frame after that one, and
		 * the queue it would come from is about to be failed.
		 */
		ludlc_handle_disconnect(conn);

		return -ENOTCONN;
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

			/* A DISCONNECT is the last frame of the session, so
			 * nothing else asks for a turn once it is out. The
			 * core counts a frame as sent on the call which asks
			 * for the next one, so without a turn asked for here
			 * that call comes from the ping timer and the session
			 * ends a ping period late.
			 */
			if (ludlc_platform_test_bit(LUDLC_CONN_DISC_AFTER_TX_F,
						    &conn->flags))
				ludlc_platform_request_tx(conn);

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
		q_idx = id & ludlc_conn_window_mask(conn);

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
 *
 * @retval true Disconnect transition was executed and caller may notify
 * `on_disconnect`.
 * @retval false No transition was needed (invalid input or already disconnected).
 *
 * @note User-facing callbacks are dispatched outside `conn->lock`.
 */
static bool ludlc_disconnect_core(struct ludlc_connection *conn)
{
	bool notify = false;

	if (!conn)
		return false;

	LUDLC_LOCK(&conn->lock);
	if (conn->conn_state != LUDLC_STATE_DISCONNECTED &&
	    conn->conn_state != LUDLC_STATE_TERMINATE) {
		on_disconnect_lock(conn);
		notify = true;
	}
	LUDLC_UNLOCK(&conn->lock);

	ludlc_confirm_unlocked(conn);
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
	if (ludlc_disconnect_core(conn))
		ludlc_call_on_disconnect(conn);
}

/**
 * @brief Restarts the watchdog for another period.
 *
 * An arriving packet normally re-arms the watchdog, which is enough for a
 * running link. It is not enough for one whose timers were stopped, such as
 * across a suspend: the peer may be gone by the time this end resumes, and
 * with nothing arriving there would be nothing to notice it.
 *
 * @param conn The connection, or NULL.
 */
void ludlc_watchdog_rearm(struct ludlc_connection *conn)
{
	if (conn)
		ludlc_platform_start_timer(&conn->wd_timer,
					   ludlc_conn_wd_time(conn), 0);
}

/**
 * @brief Cleans up a connection structure.
 *
 * @param conn Pointer to the connection structure to be zeroed.
 */
void ludlc_connection_cleanup(struct ludlc_connection *conn)
{
	if (!conn ||
		!ludlc_platform_test_bit(LUDLC_CONN_INITED_F, &conn->flags)) {
		return;
	}

	(void)ludlc_disconnect_core(conn);

	ludlc_platform_destroy_timer(&conn->wd_timer);
	ludlc_platform_destroy_timer(&conn->ping_timer);

	ludlc_platform_conn_destroy(conn);

	ludlc_conn_queue_free(conn);
}

/**
 * @brief Initializes a LuDLC connection structure.
 *
 * Prepares a `ludlc_connection` structure for use. This includes
 * zeroing the memory, storing user contexts and callbacks, initializing
 * timers, and binding the protocol cfg.
 *
 * @param conn Pointer to the connection structure to initialize.
 * @param cfg What to open the link with: its clock and its protocol settings,
 * or NULL for @ref ludlc_conn_cfg_default. Copied here, so a compound literal
 * will do; only the clock pointer is kept and must outlive the connection.
 * The settings are read only in a @ref CONFIG_LUDLC_MULTI_CONN build, being
 * the CONFIG_* constants otherwise.
 * @param cb Pointer to the user-facing event callbacks (e.g., on_connect,
 * on_recv).
 * @param user_ctx A user-defined context pointer that will be passed back
 * in all callbacks.
 *
 * @return 0 on success.
 * @return -EINVAL if `conn` is NULL, or if `cfg` names a payload, a window or
 * a TTL this build cannot hold.
 * @return -ENOMEM if the window could not be allocated.
 */
int ludlc_connection_init(struct ludlc_connection *conn,
		const struct ludlc_conn_cfg *cfg,
		const struct ludlc_conn_cb *cb,
		void *user_ctx)
{
	int ret;

	if (!conn)
		return -EINVAL;

	LUDLC_LOG_DEBUG("Start connection initialization");

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

#if CONFIG_LUDLC_MULTI_CONN

	conn->cfg.max_payload = cfg->max_payload ? cfg->max_payload
			   : (ludlc_payload_size_t)
			     CONFIG_LUDLC_MAX_PAYLOAD_SIZE;

	conn->cfg.window = cfg->window ? cfg->window
			   : (ludlc_id_t)CONFIG_LUDLC_WINDOW;

	conn->cfg.ping_time = cfg->ping_time ? cfg->ping_time
			   : (unsigned int)CONFIG_LUDLC_PING_TIME;

	conn->cfg.default_ttl = cfg->default_ttl ? cfg->default_ttl
			   : (uint8_t)LUDLC_DEFAULT_TTL;

	/*
	 * The transport derives this from the frame its link carries. A frame
	 * the build cannot hold gives a payload it cannot hold either.
	 */
	if (conn->cfg.max_payload > CONFIG_LUDLC_MAX_PAYLOAD_SIZE)
		return -EINVAL;

	/* A window the IDs cannot tell apart, or cannot be indexed by. */
	if (conn->cfg.window > LUDLC_MAX_TX_ID / 2 ||
	    (conn->cfg.window & (conn->cfg.window - 1U)) != 0)
		return -EINVAL;

	if (conn->cfg.default_ttl >= LUDLC_MAX_TTL)
		return -EINVAL;

#endif
	ret = ludlc_conn_queue_alloc(conn);
	if (ret) {
		LUDLC_LOG_ERROR("ludlc_conn_queue_alloc has failed (%d)", -ret);
		return ret;
	}

	conn->cb = cb;
	conn->user_ctx = user_ctx;
	conn->ctrl_packet.chan = CONFIG_LUDLC_CONTROL_CHANNEL;
	conn->conn_state = LUDLC_STATE_DISCONNECTED;

	conn->flags = LUDLC_CONN_IDLE_MASK;

	ret = ludlc_platform_conn_init(conn);
	if (ret)
		goto fail_platform_init;

	/* Initialize platform-specific timers */
	ret = ludlc_platform_init_timer(conn, &conn->ping_timer,
			ludlc_ping_timer);
	if (!ret) {
		ret = ludlc_platform_init_timer(conn, &conn->wd_timer,
						ludlc_wd_timer);
		if (!ret) {
			ludlc_platform_set_bit(LUDLC_CONN_INITED_F,
				&conn->flags);
			LUDLC_LOG_DEBUG("The connection initialized");
			return 0;
		}
		ludlc_platform_destroy_timer(&conn->ping_timer);
	}

fail_platform_init:

	ludlc_conn_queue_free(conn);

	LUDLC_LOG_ERROR("Connection initialization has failed (%d)", -ret);

	return ret;
}

const struct ludlc_conn_cfg ludlc_conn_cfg_default = LUDLC_CONN_CFG_DEFAULT;

ludlc_payload_size_t ludlc_conn_max_payload(const struct ludlc_connection *conn)
{
	if (!conn) {
		return 0;
	}

	return ludlc_conn_payload_lim(conn);
}

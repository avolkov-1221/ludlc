// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only)
/**
 * @file ludlc.c
 *
 * @brief LuDLC core logic implementation.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 */

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#endif

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
 * @def HANDLE_RX_CAPS_F
 * @brief Flag indicating that the packet carries the peer's capabilities,
 *        to be handed to the upper level.
 */
#define HANDLE_RX_CAPS_F	(1U<<5)

/**
 * @def HANDLE_RX_EXPIRED_F
 * @brief Flag indicating that a NAK has given up a packet, to be confirmed
 *        outside the lock.
 */
#define HANDLE_RX_EXPIRED_F	(1U<<6)

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
 * @brief Checks if a channel field selects the control channel, whatever its
 * tag.
 *
 * Control packets can't be tagged: the core sends them untagged and tells
 * them by the whole field. A field selecting the control channel with a tag
 * belongs to no channel, so it is refused on send and dropped on receipt.
 *
 * @param conn The connection, whose tag is left out of the comparison.
 * @param chan The channel field of a packet.
 * @return true if the bits below the tag hold @ref CONFIG_LUDLC_CONTROL_CHANNEL.
 */
static inline bool is_ctrl_chan(const struct ludlc_connection *conn,
				ludlc_channel_t chan)
{
	return (chan & ludlc_conn_chan_id_mask(conn)) ==
		CONFIG_LUDLC_CONTROL_CHANNEL;
}

/**
 * @brief Allocates the connection's sliding window, and the copy of the
 * payload being sent.
 *
 * Only a multi-connection build allocates, the window size and the payload
 * being runtime values there; a single-connection build has both arrays as
 * members.
 *
 * @param conn The connection, with its cfg already resolved.
 * @return 0, or -ENOMEM.
 */
static int ludlc_conn_queue_alloc(struct ludlc_connection *conn)
{
#if CONFIG_LUDLC_MULTI_CONN
	ludlc_id_t wnd = ludlc_conn_window(conn);

	/* One allocation, the payload after the window. */
	conn->packets_q =
		ludlc_platform_alloc(wnd * sizeof(*conn->packets_q) +
				     ludlc_conn_payload_lim(conn));
	if (!conn->packets_q) {
		return -ENOMEM;
	}

	conn->tx_payload = (uint8_t *)(conn->packets_q + wnd);
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
	conn->tx_payload = NULL;
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
				-(int)pkt->result,
				pkt->chan,
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
	conn->last_seen = 0;

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
 * caller must have written @c result into the slot.
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
 * caller must have written @c result into @c u_packet.
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
		conn->u_packet.result = EPIPE;
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
		it->result = EPIPE;

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
 * @param ttl How many times the packet may be sent and lost. Each copy the
 * peer reports missing costs one, and once none is left the packet is given
 * up and confirmed with -ETIMEDOUT. A value of 0 is normalized to 1 (single
 * send attempt). Values above @ref LUDLC_MAX_TTL are clamped.
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
	 * caller sending on it could imitate either one, and a tag would not
	 * get it through: the peer drops a tagged control packet.
	 */
	if (is_ctrl_chan(conn, dst_chan)) {
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
		conn->packets_q[q_idx].chan = dst_chan;
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
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(ludlc_enqueue_data);
#endif

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
	if (is_ctrl_chan(conn, dst_chan)) {
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
		conn->u_packet.chan = dst_chan;
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
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(ludlc_send_urgent_data);
#endif

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
			conn->u_packet.result = ECANCELED;
			ludlc_confirm_u_lock(conn);
		}

		conn->u_packet.chan = CONFIG_LUDLC_CONTROL_CHANNEL;
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
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(ludlc_send_disconnect);
#endif

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
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(ludlc_connect);
#endif

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
 * @param step Whether the packet is a handshake step, a numbered control
 * packet, rather than a PING or data.
 * @retval An integer containing bit flags:
 * - HANDLE_RX_REQUEST_TX_F if a response should be sent.
 * - HANDLE_RX_CONTINUE_F if processing should continue (e.g., connected).
 * - HANDLE_RX_CAPS_F if the packet says what the peer announces.
 */
static int rx_handle_handshake(
		struct ludlc_connection *conn,
		ludlc_packet_t *packet,
		bool step)
{
	ludlc_id_t id = packet->hdr.id.tx_id;
	int ret = HANDLE_RX_REQUEST_TX_F; /* Default to sending a response */

	/* Anything but a handshake step: a PING or data. */
	if (!step) {
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
		if ((unsigned int)(id - conn->conn_state) < 2U) {
			/* Only the step that takes this end out of State 1
			 * hands on what the peer announces, its capabilities
			 * or nothing. Every handshake leaves State 1 on the
			 * peer's id=1 or id=2, which both carry it, whereas a
			 * PING or data from a peer already connected may end
			 * State 2. Either step may be the one: the peer passes
			 * through State 1 without sending id=1 when its
			 * transmitter gets no turn before it moves on. So this
			 * happens once per handshake, before the link is up.
			 */
			if (conn->conn_state == LUDLC_STATE_WAIT_CONN_1) {
				ret |= HANDLE_RX_CAPS_F;
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
 * @brief Notes that the peer has answered our NAK, or needs none.
 *
 * From here on a gap the peer shows is news, and is reported.
 *
 * @note This function must be called with the connection lock held.
 *
 * @param conn Pointer to the connection structure.
 */
static inline void rx_nak_answered(struct ludlc_connection *conn)
{
	ludlc_platform_clear_mask((ludlc_platform_atomic_t)
				  ((1U << LUDLC_CONN_NAK_OUT_F) |
				   (1U << LUDLC_CONN_NAK_TRACKED_F)),
				  &conn->flags);
}

/**
 * @brief Checks if a NAK stands and no numbered packet has gone out since.
 *
 * @param conn Pointer to the connection structure.
 * @return true if nothing yet shows whether the peer has had the NAK.
 */
static inline bool rx_nak_untracked(struct ludlc_connection *conn)
{
	return ludlc_platform_test_bit(LUDLC_CONN_NAK_OUT_F, &conn->flags) &&
	       !ludlc_platform_test_bit(LUDLC_CONN_NAK_TRACKED_F,
					&conn->flags);
}

/**
 * @brief Asks the peer to send again from the first packet missing, unless
 * it has yet to answer the last time it was asked.
 *
 * Everything the peer sent before it saw a NAK shows the same gap. A NAK for
 * each would send it back once per frame, and charge the missing packet's
 * TTL each time. A NAK therefore stands until the peer is seen answering it:
 * the gap fills, a PING shows nothing is missing any more, the peer goes back
 * to send its packets again (@p new_pass), or it acknowledges the first
 * numbered packet this end sent after the NAK, by which time it has had the
 * NAK or lost it. Meanwhile a PING carries the NAK again; see
 * @ref ludlc_get_packet_to_send.
 *
 * @note This function must be called with the connection lock held.
 *
 * @param conn Pointer to the connection structure.
 * @param new_pass The peer has gone back since the last NAK.
 * @return @ref HANDLE_RX_REQUEST_TX_F to send a NAK, or 0.
 */
static int rx_gap(struct ludlc_connection *conn, bool new_pass)
{
	if (new_pass)
		rx_nak_answered(conn);

	if (ludlc_platform_test_bit(LUDLC_CONN_NAK_OUT_F, &conn->flags))
		return 0;

	/* The next frame carries it. */
	ludlc_platform_clear_bit(LUDLC_CONN_NO_NAK_F, &conn->flags);
	return HANDLE_RX_REQUEST_TX_F;
}

/**
 * @brief Handles a Negative Acknowledgment (NAK) condition.
 *
 * A NAK identifies one packet: it acknowledges the last packet received
 * in order, so the missing one is the next after it. Only that packet has
 * its TTL decremented, and once for each copy sent: a NAK arriving before
 * the copy sent in answer to the previous one has gone out cannot be about
 * it. The packets behind it are re-transmitted because the receiver discards
 * everything after a gap, not because they were lost, so their TTL is left
 * alone.
 *
 * A packet left with no attempts is given up there and then, and the ones
 * behind it move up one place, each taking the previous ID. The peer neither
 * holds it nor can still get it: a NAK is sent when a packet arrives ahead of
 * it, and on a link that keeps its order every copy sent before that packet
 * is then lost. The keep-alive that repeats a NAK follows no arrival, so the
 * ping period must exceed the time an answer to a NAK takes to arrive. The
 * transport reads only the copy it was handed, so the slots can move and the
 * buffer can be returned at once.
 *
 * Once none is left behind it, the NAK is about packets this end no longer
 * has, yet the peer may repeat it until it sees as much. The next packet sent
 * is therefore not charged for its first copy, which may still be on its way.
 *
 * Finally, it resets `last_sent` to `last_ack` to force a re-transmission
 * of all remaining unacknowledged packets.
 *
 * @note This function must be called with the connection lock held.
 *
 * @param conn Pointer to the connection structure.
 * @param[out] expired The packet given up, to be confirmed once the lock has
 * been released.
 * @return @ref HANDLE_RX_REQUEST_TX_F to signal an immediate re-transmission,
 * with @ref HANDLE_RX_EXPIRED_F if @p expired has been filled in.
 */
static int rx_handle_nak(struct ludlc_connection *conn,
			 struct packet_queue_item *expired)
{
	ludlc_id_t id = (conn->last_ack + 1) & LUDLC_ID_MASK;
	struct packet_queue_item *it =
		&conn->packets_q[id & ludlc_conn_window_mask(conn)];
	int ret = HANDLE_RX_REQUEST_TX_F;

	/* The missing packet is the first unacknowledged one. It pays for the
	 * copy last sent, once, and only if one has gone out since a NAK last
	 * sent last_sent back to it.
	 */
	if (conn->last_sent != conn->last_ack &&
	    !ludlc_platform_test_and_set_bit(LUDLC_CONN_TTL_CHARGED_F,
					     &conn->flags)) {
		if (--it->ttl == 0) {
			unsigned int mask = ludlc_conn_window_mask(conn);
			ludlc_id_t n = (conn->last_queued - id) & LUDLC_ID_MASK;

			it->result = ETIMEDOUT;
			*expired = *it;
			ret |= HANDLE_RX_EXPIRED_F;

			/* Those behind it move up one place. */
			for (; n; n--, id = (id + 1) & LUDLC_ID_MASK)
				conn->packets_q[id & mask] =
					conn->packets_q[(id + 1) & mask];

			conn->last_queued = (conn->last_queued - 1) &
					    LUDLC_ID_MASK;
			if (conn->last_queued == conn->last_ack)
				ludlc_platform_set_bit(LUDLC_CONN_TTL_HOLD_F,
						       &conn->flags);
		}
	}

	/* resend all unconfirmed packets */
	conn->last_sent = conn->last_ack;
	return ret;
}

/**
 * @brief Processes the ACK field the peer sent: which of our packets it
 * holds, and whether it misses one.
 *
 * An ACK may run past `last_sent`, which a NAK sends back to `last_ack`
 * while the peer may hold the packets after it. So the ACK is checked against
 * what has been queued, and `last_sent` moves up with it rather than send
 * those packets again.
 *
 * @note This function must be called with the connection lock held.
 *
 * @param conn Pointer to the connection structure.
 * @param ack_id The ACK field, with its NAK flag.
 * @param[out] expired See @ref rx_handle_nak.
 * @return What @ref rx_handle_nak returns for a NAK, 0 otherwise.
 */
static int rx_handle_ack(struct ludlc_connection *conn, ludlc_id_t ack_id,
			 struct packet_queue_item *expired)
{
	/* ludlc_id_t is unsigned, so the subtractions wrap and the mask alone
	 * gives the right count, dropping the NAK flag as well.
	 */
	ludlc_id_t num_sent = (conn->last_sent - conn->last_ack) &
			      LUDLC_ID_MASK;
	ludlc_id_t num_acked = (ack_id - conn->last_ack) & LUDLC_ID_MASK;
	ludlc_id_t i;

	if (num_acked > ((conn->last_queued - conn->last_ack) &
			 LUDLC_ID_MASK)) {
		/* The peer ACKed packets we never queued, so the two sides
		 * are badly out of sync.
		 *
		 * TODO: force an immediate reconnect?
		 */
		LUDLC_LOG_DEBUG("ACK ID is too far: (%x) from last_ack = %x",
				ack_id, conn->last_ack);
		return 0;
	}

	for (i = num_acked; i; i--) {
		struct packet_queue_item *it;

		conn->last_ack = (conn->last_ack + 1) & LUDLC_ID_MASK;
		it = &conn->packets_q[conn->last_ack &
				      ludlc_conn_window_mask(conn)];
		it->result = 0;
		ludlc_confirm_queued_lock(conn, it);

		/* The peer holds our first numbered packet after our NAK, so
		 * by now it has had the NAK, or lost it.
		 */
		if (conn->last_ack == conn->nak_track &&
		    ludlc_platform_test_bit(LUDLC_CONN_NAK_TRACKED_F,
					    &conn->flags))
			rx_nak_answered(conn);
	}

	if (num_acked) {
		/* A new first packet, which has paid for no copy yet. */
		ludlc_platform_clear_bit(LUDLC_CONN_TTL_CHARGED_F,
					 &conn->flags);

		/* A NAK sent last_sent back below what the peer holds. */
		if (num_acked > num_sent)
			conn->last_sent = conn->last_ack;
	}

	if (ack_id & LUDLC_NAK_FLAG)
		return rx_handle_nak(conn, expired);

	return 0;
}

/**
 * @brief Core internal packet processing logic.
 *
 * This function is the core of the receive-side logic. It must be
 * called with the connection lock held. It performs the following steps:
 * 1. Validates packet size, and drops a tagged control packet.
 * 2. If not connected, calls @ref rx_handle_handshake, and starts the
 *    watchdog if that brings the link up.
 * 3. Handles control channel packets: an announced disconnect, and a
 *    numbered one, which is a handshake step from a peer that has restarted.
 * 4. Resets the connection watchdog timer.
 * 5. Processes the acknowledgment ID (`ack_id`) to confirm sent packets,
 *    through @ref rx_handle_ack.
 * 6. Validates the packet's TX ID sequence (`tx_id`): accepts the next one,
 *    acknowledges again a copy of one already received, and calls
 *    @ref rx_gap for one sent after packets that went missing.
 *
 * @param conn Pointer to the connection structure.
 * @param packet Pointer to the received LUDLC packet.
 * @param pkt_sz The total size of the received packet.
 * @param[out] expired A packet a NAK has given up; see @ref rx_handle_nak.
 * @return A bitmask of flags:
 * - @ref HANDLE_RX_REQUEST_TX_F to request a response (ACK/NAK/PING).
 * - @ref HANDLE_RX_ACCEPT_F to accept the packet's payload.
 * - @ref HANDLE_RX_EXPIRED_F to confirm @p expired.
 */
static int rx_handle_packet(
		struct ludlc_connection *conn,
		ludlc_packet_t *packet,
		size_t pkt_sz,
		struct packet_queue_item *expired)
{
	ludlc_id_t id, ahead;
	bool ping = is_ping(packet->hdr.id.tx_id);
	bool u_packet = (ping && pkt_sz > sizeof(ludlc_packet_hdr_t)) ? true
								: false;
	bool is_hshake;
	int ret;

	if (!ping && pkt_sz < sizeof(ludlc_packet_hdr_t)) {
		/* Too short to hold a header, yet the checksum is good, so
		 * the peer is misbehaving. Drop it.
		 */
		LUDLC_INC_STATS(conn, dropped);
		return 0;
	}

	/* The core never tags the control channel, so a peer that does is
	 * misbehaving as well, or splits the channel field another way. The
	 * packet belongs to no channel here. A PING has no channel field.
	 */
	if ((u_packet || !ping) &&
			packet->hdr.chan != CONFIG_LUDLC_CONTROL_CHANNEL &&
			is_ctrl_chan(conn, packet->hdr.chan)) {
		LUDLC_INC_STATS(conn, dropped);
		return 0;
	}

	/* A numbered control packet is a handshake step, whatever it
	 * carries.
	 */
	is_hshake = !ping && packet->hdr.chan == CONFIG_LUDLC_CONTROL_CHANNEL;

	if (conn->conn_state != LUDLC_STATE_CONNECTED) {
		ret = rx_handle_handshake(conn, packet, is_hshake);

		/* The watchdog starts with the link, whichever packet brings
		 * it up: the last handshake step returns just below, before
		 * anything else would start it.
		 */
		if (ret & HANDLE_RX_CONNECTED_F)
			ludlc_watchdog_restart(conn);

		if ((ret & HANDLE_RX_CONTINUE_F) == 0) {
			/* Handshake not complete, stop further processing */
			return ret;
		}
		/* Handshake just completed, continue processing this packet */
	} else {
		ret = 0;
	}

	id = packet->hdr.id.tx_id & LUDLC_ID_MASK;

	/* An unnumbered packet on the control channel carries an opcode. The
	 * layout is checked before the channel is read, because a PING ends
	 * after the two IDs and has no channel field.
	 */
	if (u_packet &&
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

	if (is_hshake) {
		/* Unless it is the last step arriving again, its sender has
		 * restarted and does not know this session is over.
		 */
		if (id != LUDLC_STATE_LAST_STEP) {
			on_disconnect_lock(conn);
			ret |= HANDLE_RX_DISCONNECT_F;
		}
		/* Respond to disconnect/handshake */
		return ret | HANDLE_RX_REQUEST_TX_F;
	}

	/* Any valid packet (even ping) resets the connection watchdog */
	ludlc_watchdog_restart(conn);

	/* 1. Our own packets first: those the peer holds, and one it misses.
	 * The sequence below needs to know whether the peer has had our NAK.
	 */
	ret |= rx_handle_ack(conn, packet->hdr.id.ack_id, expired);

	/* 2. Then the peer's: how far past the next one expected this one
	 * is. Up to a window ahead, it was sent after packets that never
	 * arrived; further, it is behind, a copy of one received already.
	 */
	ahead = (id - conn->last_received - 1) & LUDLC_ID_MASK;

	if (ping) {
		/* A PING or an unnumbered packet holds the ID the peer sends
		 * next, so everything before that has gone out: up to a
		 * window of it lost, or behind, the peer having gone back.
		 * The tie, a whole window either way, counts as lost: a NAK
		 * too many costs a re-transmission, one too few stalls the
		 * link. The very ID expected shows nothing missing, which
		 * answers a NAK whose packets the peer has given up.
		 */
		if (!ahead || ahead > ludlc_conn_window(conn))
			rx_nak_answered(conn);
		else
			ret |= rx_gap(conn, false);

		/* Outside the sequence, so accept the payload regardless of
		 * the ID check above and leave last_received unchanged.
		 * There is nothing to ACK.
		 */
		if (u_packet)
			ret |= HANDLE_RX_ACCEPT_F;
	} else if (!ahead) {
		/* The next one. A NAK still waiting would report the one
		 * after it, which the peer is about to send in any case.
		 */
		conn->last_received = id;
		ludlc_platform_set_bit(LUDLC_CONN_NO_NAK_F, &conn->flags);
		rx_nak_answered(conn);
		ret |= HANDLE_RX_REQUEST_TX_F | HANDLE_RX_ACCEPT_F;
	} else if (ahead < ludlc_conn_window(conn)) {
		/* Packets went missing. The peer sends in rising order, so
		 * one no further ahead than the last it sent comes again:
		 * the peer has gone back, answering a NAK.
		 */
		ludlc_id_t last = (conn->last_seen - conn->last_received - 1) &
				  LUDLC_ID_MASK;

		ret |= rx_gap(conn, last < ludlc_conn_window(conn) &&
				    ahead <= last);
	} else {
		/* A copy of one received already: the peer missed our ACK,
		 * or went back. Drop the payload, and ACK again.
		 */
		rx_nak_answered(conn);
		ret |= HANDLE_RX_REQUEST_TX_F;
	}

	/* How far the peer has sent, a PING holding the ID it sends next. */
	conn->last_seen = ping ? (id - 1) & LUDLC_ID_MASK : id;

	return ret;
}

/**
 * @brief Processes a received LuDLC packet from the transport layer.
 *
 * This is the main entry point for feeding data *into* the LuDLC state
 * machine. It acquires the connection lock, processes the packet, and
 * then (after releasing the lock) invokes any necessary user callbacks
 * (`on_caps` for the peer's capabilities, `on_confirm` for acknowledged
 * packets and one a NAK has given up, `on_recv` for new data).
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
	struct packet_queue_item expired = { 0 };
	int ret;

	LUDLC_LOG_TRACE("packet (%x, %x) @%llu",
			packet->hdr.id.tx_id,
			packet->hdr.id.ack_id,
			tstamp);

	LUDLC_LOCK(&conn->lock);
		ret = rx_handle_packet(conn, packet, pkt_sz, &expired);
	LUDLC_UNLOCK(&conn->lock);

	/* --- Actions outside the lock --- */

	/* Never on the packet that completes the handshake, which comes later,
	 * so the application has the peer's capabilities by on_connect.
	 */
	if ((ret & HANDLE_RX_CAPS_F) && conn->cb && conn->cb->on_caps) {
		ludlc_payload_size_t size = pkt_sz - sizeof(packet->hdr);

		conn->cb->on_caps(conn, size ? packet->payload : NULL, size,
				conn->user_ctx);
	}

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

	/* After those, the packet given up being the one that follows them. */
	if (ret & HANDLE_RX_EXPIRED_F)
		ludlc_call_on_confirm(conn, &expired);

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
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(ludlc_receive);
#endif

/**
 * @brief Retrieves the next packet to be sent by the transport layer.
 *
 * This function is called by the transmit task to get the next packet
 * to send. It acquires the lock and determines what to send based on
 * the connection state:
 * - **Handshake:** Returns a control packet.
 * - **Connected (queue empty):** Returns a PING packet.
 * - **Connected (queue has data):** Returns the next data packet from the
 *   queue.
 *
 * It provides pointers to the header and payload segments separately. Both
 * are the core's and stay as they are until the next call, whatever happens
 * to the packet meanwhile: the header is built for the frame, and the
 * payload is a copy of the caller's buffer, which an ACK for an earlier copy
 * or the link going down may return before the transport has finished. The
 * capabilities of a handshake step go out as they are, being constant.
 *
 * Returning a frame also restarts the ping timer, which the transport does
 * not have to do anything about. On a connected link that postpones the
 * keep-alive by one period, as the frame carries everything a PING would.
 * During the handshake no PING is sent, and the timer only re-sends the
 * current step a period after the last frame.
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
	const struct packet_queue_item *it = NULL;
	struct packet_queue_item u_sent;
	bool u_done = false, disc_done = false;
	bool ping = false, numbered = false;
	ludlc_id_t ack, id;
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
			conn->u_packet.result = 0;
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

	*hdr = &conn->tx_hdr;
	*payload = NULL;
	*pay_sz = 0;

	switch (conn->conn_state) {
	default:/* LUDLC_STATE_DISCONNECTED, LUDLC_STATE_WAIT_CONN_1, etc. */

		/* Send handshake control packet */
		conn->tx_hdr.chan = CONFIG_LUDLC_CONTROL_CHANNEL;
		conn->tx_hdr.id.tx_id = conn->conn_state;
		/* No ACK during handshake */
		conn->tx_hdr.id.ack_id = 0;

		/* Full control header (id + channel) */
		*hdr_sz = sizeof(conn->tx_hdr);

		/* The steps of States 1 and 2 both carry this end's
		 * capabilities: the peer leaves State 1 on either, and this
		 * end may pass through State 1 without sending its id=1. The
		 * first step stays empty: it is what tells a peer still in a
		 * session that this end has restarted.
		 */
		if ((conn->conn_state == LUDLC_STATE_WAIT_CONN_1 ||
		     conn->conn_state == LUDLC_STATE_WAIT_CONN_2) &&
		    conn->cb && conn->cb->caps_size) {
			*payload = conn->cb->caps;
			*pay_sz = conn->cb->caps_size;
		}

		break;

	case LUDLC_STATE_CONNECTED:
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

			conn->tx_hdr.id.tx_id = id | LUDLC_PING_FLAG;
			conn->tx_hdr.chan = conn->u_packet.chan;
			*hdr_sz = sizeof(conn->tx_hdr);
			it = &conn->u_packet;

			if (conn->last_sent != conn->last_queued) {
				/* The window still has packets waiting */
				ret = 0;
			}

			/* The core counts a frame as sent on the call which
			 * asks for the next one, and that call is what frees
			 * the slot and confirms the packet. Without a turn
			 * asked for here it comes from the ping timer: on an
			 * idle window a stream of unnumbered packets would run
			 * at one per ping period, and a DISCONNECT would end
			 * the session a ping period late. The turn sends a
			 * PING when nothing else is waiting; that PING is not
			 * the keep-alive and repeats no NAK, see below.
			 */
			ludlc_platform_request_tx(conn);
		} else if (conn->last_sent == conn->last_queued) {
			/* Nothing to send, so send PING instead. The PING of
			 * the turn an unnumbered packet asked for follows
			 * that packet at once, not a ping period after the
			 * last frame, so it is not the keep-alive.
			 */
			conn->tx_hdr.id.tx_id = id | LUDLC_PING_FLAG;
			*hdr_sz = sizeof(conn->tx_hdr.id);
			ping = !u_done;
		} else {
			/* send packet from the queue */
			it = &conn->packets_q[
				id & ludlc_conn_window_mask(conn)];

			conn->tx_hdr.id.tx_id = id;
			conn->tx_hdr.chan = it->chan;
			*hdr_sz = sizeof(conn->tx_hdr);

			/* A new copy of the first packet unacknowledged,
			 * which a NAK may charge for again, unless a give-up
			 * has just emptied the window, see rx_handle_nak().
			 */
			if (conn->last_sent == conn->last_ack &&
			    !ludlc_platform_test_and_clear_bit(
					LUDLC_CONN_TTL_HOLD_F, &conn->flags))
				ludlc_platform_clear_bit(
					LUDLC_CONN_TTL_CHARGED_F,
					&conn->flags);

			/* Advance sent pointer */
			conn->last_sent = id;
			numbered = true;

			if (id != conn->last_queued) {
				/* More packets are waiting in the queue */
				ret = 0;
			}
		}

		/* Prepare ACK ID, with a NAK if one is waiting. A NAK stands
		 * until the peer answers it, see rx_gap(), and a PING carries
		 * it again meanwhile. Without that, a NAK lost each way would
		 * leave each end waiting for an ACK the other cannot send
		 * until it has the NAK. While it stands the receive path asks
		 * for no frame, so that PING is the keep-alive, a ping period
		 * after the last frame. The period must cover the round trip
		 * and a full window sent ahead of the answer, or the repeat
		 * may charge a copy still on its way, see rx_handle_nak().
		 * Either way the NAK goes out anew, and only a numbered packet
		 * sent after it can show it arrived.
		 */
		ack = conn->last_received;
		if (!ludlc_platform_test_and_set_bit(LUDLC_CONN_NO_NAK_F,
						&conn->flags) ||
		    (ping && ludlc_platform_test_bit(LUDLC_CONN_NAK_OUT_F,
						     &conn->flags))) {
			ack |= LUDLC_NAK_FLAG;
			ludlc_platform_set_bit(LUDLC_CONN_NAK_OUT_F,
					       &conn->flags);
			ludlc_platform_clear_bit(LUDLC_CONN_NAK_TRACKED_F,
						 &conn->flags);
		}

		/* The peer's ACK for the first numbered packet after the NAK
		 * shows that it has had the NAK, or lost it.
		 */
		if (numbered && rx_nak_untracked(conn)) {
			conn->nak_track = id;
			ludlc_platform_set_bit(LUDLC_CONN_NAK_TRACKED_F,
					       &conn->flags);
		}

		conn->tx_hdr.id.ack_id = ack;
		break;
	}

	if (it) {
		/* The transport reads the frame until it asks for the next
		 * one, while the packet may be confirmed and its buffer
		 * returned meanwhile. So the frame carries a copy.
		 */
		if (it->size)
			memcpy(conn->tx_payload, it->payload, it->size);
		*payload = conn->tx_payload;
		*pay_sz = it->size;
	}

	/*
	 * On a connected link every frame carries the next sequence ID and the
	 * current ACK, which is what a PING would say, so it serves as a
	 * keep-alive and the ping timer can be postponed. An unnumbered packet
	 * is no exception. During the handshake the same timer re-sends the
	 * current step a ping period after the last frame.
	 */
	ludlc_platform_start_timer(&conn->ping_timer,
				   ludlc_conn_ping_time(conn),
				   ludlc_conn_ping_time(conn));

	LUDLC_UNLOCK(&conn->lock);

	if (u_done)
		ludlc_call_on_confirm(conn, &u_sent);

	return ret;
}
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(ludlc_get_packet_to_send);
#endif

/**
 * @brief Ping timer callback.
 *
 * This function is invoked when the ping timer expires, a ping period after
 * the last frame sent. It only requests a TX cycle, and what that sends
 * depends on the state: on a connected link a PING if the queue is empty,
 * during the handshake the current handshake step again. No PING is sent
 * before the link is up.
 *
 * @param arg Platform timer callback argument.
 */
static void ludlc_ping_timer(ludlc_platform_timer_arg_t arg)
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
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(ludlc_handle_disconnect);
#endif

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
void ludlc_watchdog_restart(struct ludlc_connection *conn)
{
	if (conn)
		ludlc_platform_start_timer(&conn->wd_timer,
					   ludlc_conn_wd_time(conn), 0);
}
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(ludlc_watchdog_restart);
#endif

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
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(ludlc_connection_cleanup);
#endif

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
 * @return -EINVAL if `conn` is NULL, if `cfg` names a payload, a window, a TTL
 * or a tag this build cannot hold, or if the capabilities in `cb` do not fit
 * in one packet of the link.
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

	LUDLC_LOCK_INIT(&conn->lock);

	/*
	 * A setting the caller left at zero takes the build default.
	 * Resolved here, once, rather than at every read afterwards.
	 */
	conn->cfg.get_timestamp =
		cfg->get_timestamp ? cfg->get_timestamp
			   : ludlc_default_get_timestamp;

#if LUDLC_CHAN_TAG_PER_LINK
	/*
	 * The tag has to leave a channel, and the control channel has to be
	 * one of those left: a caller could otherwise reach it with a tag.
	 * A tag the whole build runs is checked when it is compiled instead.
	 */
	if (!ludlc_chan_tag_bits_ok(cfg->chan_tag_bits))
		return -EINVAL;

	conn->cfg.chan_tag_bits = cfg->chan_tag_bits;
#endif

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
	/* The capabilities go out whole with a handshake step, so they must
	 * fit in one packet of the link.
	 */
	if (cb && cb->caps_size &&
	    (!cb->caps || cb->caps_size > ludlc_conn_payload_lim(conn)))
		return -EINVAL;

	ret = ludlc_conn_queue_alloc(conn);
	if (ret) {
		LUDLC_LOG_ERROR("ludlc_conn_queue_alloc has failed (%d)", -ret);
		return ret;
	}

	conn->cb = cb;
	conn->user_ctx = user_ctx;
	conn->tx_hdr.chan = CONFIG_LUDLC_CONTROL_CHANNEL;
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
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(ludlc_connection_init);
#endif

const struct ludlc_conn_cfg ludlc_conn_cfg_default = LUDLC_CONN_CFG_DEFAULT;
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(ludlc_conn_cfg_default);
#endif

ludlc_payload_size_t ludlc_conn_max_payload(const struct ludlc_connection *conn)
{
	if (!conn) {
		return 0;
	}

	return ludlc_conn_payload_lim(conn);
}
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(ludlc_conn_max_payload);
#endif

/** @brief The tag width of @p conn, none for a NULL one. */
static inline unsigned int chan_tag_bits(const struct ludlc_connection *conn)
{
	return conn ? ludlc_conn_chan_tag_bits(conn) : 0;
}

ludlc_channel_t ludlc_chan_make(const struct ludlc_connection *conn,
		ludlc_channel_t id, ludlc_channel_t tag)
{
	unsigned int bits = chan_tag_bits(conn);

	/* No tag, and shifting by the whole width of the field is undefined. */
	if (!bits) {
		return id;
	}

	return (ludlc_channel_t)((id & LUDLC_CHAN_ID_MASK(bits)) |
			(tag << (CONFIG_LUDLC_CHANNEL_BITS - bits)));
}
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(ludlc_chan_make);
#endif

ludlc_channel_t ludlc_chan_id(const struct ludlc_connection *conn,
		ludlc_channel_t chan)
{
	return (ludlc_channel_t)(chan & LUDLC_CHAN_ID_MASK(chan_tag_bits(conn)));
}
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(ludlc_chan_id);
#endif

ludlc_channel_t ludlc_chan_tag(const struct ludlc_connection *conn,
		ludlc_channel_t chan)
{
	unsigned int bits = chan_tag_bits(conn);

	/* As in ludlc_chan_make(). */
	if (!bits) {
		return 0;
	}

	return (ludlc_channel_t)(chan >> (CONFIG_LUDLC_CHANNEL_BITS - bits));
}
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(ludlc_chan_tag);
#endif

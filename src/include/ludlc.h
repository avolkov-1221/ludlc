// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only)
/**
 * @file ludlc.h
 *
 * @brief LuDLC public API definitions.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 */

#ifndef __LUDLC_H__
#define __LUDLC_H__

#include <ludlc_types.h>
#include <ludlc_proto.h>
#include <ludlc_platform_args.h>

/**
 * @struct ludlc_conn_cb
 * @brief User-facing event callbacks for a LuDLC connection.
 *
 * This structure defines the set of callback functions that the LuDLC
 * core will invoke to notify the user application of various events, such
 * as connection status changes, received data, and packet confirmations.
 */
struct ludlc_conn_cb {
	/**
	 * @brief Called when the connection is lost or terminated.
	 *
	 * @param conn The connection that was disconnected.
	 * @param user_arg The user-defined context pointer.
	 */
	void (*on_disconnect)(struct ludlc_connection *conn, void *user_arg);

	/**
	 * @brief Called when the connection handshake successfully completes.
	 *
	 * @param conn The connection that has been established.
	 * @param user_arg The user-defined context pointer.
	 */
	void (*on_connect)(struct ludlc_connection *conn, void *user_arg);

	/**
	 * @brief Called when a new data packet is received.
	 *
	 * @param conn The connection that received the data.
	 * @param chan The channel on which the data was received.
	 * @param data Pointer to the received payload data buffer.
	 * @param data_size The size of the received payload in bytes.
	 * @param tstamp Monotonic timestamp when the transport received the
	 * packet. Unit is whatever @ref ludlc_get_timestamp_t returns.
	 * @param user_arg The user-defined context pointer.
	 */
	void (*on_recv)(struct ludlc_connection *conn,
			ludlc_channel_t chan,
			const void *data,
			ludlc_payload_size_t data_size,
			ludlc_timestamp_t tstamp,
			void *user_arg);

	/**
	 * @brief Called to confirm the transmission status of a sent packet.
	 *
	 * This callback is invoked when a packet sent via
	 * `ludlc_enqueue_data` is either successfully acknowledged by the
	 * peer or has failed (e.g., due to retries being exhausted or
	 * disconnection).
	 *
	 * @param conn The connection handling the packet.
	 * @param error 0 on successful acknowledgment, or a negative
	 * error code (e.g., -EPIPE, -EREMOTEIO) on failure.
	 * @param chan The channel on which the packet was sent.
	 * @param data Pointer to the payload data that was sent.
	 * @param data_size The size of the payload data.
	 * @param user_arg The user-defined context pointer.
	 */
	void (*on_confirm)(struct ludlc_connection *conn,
			int error,
			ludlc_channel_t chan,
			const void *data,
			ludlc_payload_size_t data_size,
			void *user_arg);
};

/**
 * @brief Initializes a pre-allocated LuDLC connection structure.
 *
 * This is primarily used as a part of specialized connection
 * allocation/deallocation schemes (e.g., static or pool allocation).
 *
 * @param conn Pointer to the pre-allocated connection structure to
 * initialize.
 * @param cfg The link's clock, or NULL for @ref ludlc_conn_cfg_default.
 * Copied at init, so it need not outlive the call, but the clock it names
 * must, being kept.
 * @param cb Pointer to the user event callbacks.
 * @param user_ctx A user-defined context pointer passed to all callbacks.
 * @return 0 on success, or a negative error code on failure.
 */
int ludlc_connection_init(
		struct ludlc_connection *conn,
		const struct ludlc_conn_cfg *cfg,
		const struct ludlc_conn_cb *cb,
		void *user_ctx);
/**
 * @brief Cleans up a LuDLC connection structure.
 *
 * Resets the connection structure to a clean state. Does not free
 * the memory for the structure itself.
 *
 * @param conn The connection to clean up.
 */
void ludlc_connection_cleanup(struct ludlc_connection *conn);

/**
 * @brief Enqueues a data packet for transmission.
 *
 * Adds a data payload to the transmit queue. The core logic will
 * automatically handle packetization, acknowledgment, and retries.
 *
 * @param conn The connection to send data on.
 * @param dst_chan The destination channel for the packet.
 * @param buf Pointer to the payload data buffer.
 * @param size The size of the payload, up to
 * @ref CONFIG_LUDLC_MAX_PAYLOAD_SIZE.
 * @param ttl How many times the packet may be sent. The first send counts,
 * and each NAK that reports this packet missing costs one more. A packet
 * re-transmitted after another packet's gap is not charged for it. A value
 * of 0 means a single attempt. Values above @ref LUDLC_MAX_TTL are clamped.
 * @return 0 on success.
 * @return -EINVAL if @p buf is NULL and @p size is not, or if @p size is too
 * large.
 * @return -EAGAIN if the transmit queue (window) is full.
 */
int ludlc_enqueue_data(struct ludlc_connection *conn,
		ludlc_channel_t dst_chan,
		const void *buf, ludlc_payload_size_t size, uint8_t ttl);

/**
 * @brief Sends an unnumbered (U) packet, ahead of everything else.
 *
 * For events that cannot wait behind ordinary traffic, such as a power
 * failure. The packet carries no sequence number and takes no place in the
 * window, so it goes out in the next frame whatever the window is doing:
 * during a re-transmission after a NAK, or while the window is full and
 * @ref ludlc_enqueue_data is returning -EAGAIN.
 *
 * Skipping the window also loses its guarantees: the packet is not
 * acknowledged and never re-transmitted, so `on_confirm` reports it once it
 * has been sent, not once the peer has received it. Use
 * @ref ludlc_enqueue_data for data that must arrive.
 *
 * @param conn The connection to send data on.
 * @param dst_chan The destination channel for the packet.
 * @param buf Pointer to the payload data buffer. As for
 * @ref ludlc_enqueue_data, the buffer is referenced rather than copied and
 * must stay valid until the packet is confirmed.
 * @param size The size of the payload, up to
 * @ref CONFIG_LUDLC_MAX_PAYLOAD_SIZE.
 * @return 0 on success.
 * @return -EINVAL if @p buf is NULL, or if @p size is 0 or too large.
 * @return -ENOTCONN if the connection is not established. Such a packet is
 * sent immediately or not at all, so it does not wait for a link.
 * @return -EAGAIN if the previous unnumbered packet has not been sent yet.
 * Only one is held at a time.
 */
int ludlc_send_urgent_data(struct ludlc_connection *conn,
		ludlc_channel_t dst_chan,
		const void *buf, ludlc_payload_size_t size);

/**
 * @brief Sends a graceful disconnect control request to the peer.
 *
 * This helper sends an empty header as disconnect notification
 * on @ref CONFIG_LUDLC_CONTROL_CHANNEL.
 * The peer will process it through the explicit control disconnect path.
 *
 * @param conn The connection to send the disconnect request on.
 * @return 0 on success.
 * @return -EAGAIN if the transmit queue (window) is full.
 */
int ludlc_send_disconnect(struct ludlc_connection *conn);

/**
 * @brief Starts or restarts LuDLC handshake to establish a link.
 *
 * May be called after connection creation and after a transport drop to
 * trigger (re)connection. If the protocol state is mid-session, the core is
 * reset to a clean disconnected state first, pending TX packets are failed
 * via the regular confirm callbacks, and the handshake starts again.
 *
 * Dropping a session this way does not call `on_disconnect`, as the caller
 * asked for it. A caller that pairs the two callbacks should account for the
 * `on_connect` that follows.
 *
 * @param conn The connection to (re)connect.
 * @return 0 on success.
 * @return -EINVAL if @p conn is NULL or not initialized.
 * @return -ESHUTDOWN if the connection is terminating.
 */
int ludlc_connect(struct ludlc_connection *conn);

#endif /* __LUDLC_H__ */

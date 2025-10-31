// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file ludlc.h
 *
 * @brief LuDLC public API definitions.
 *
 * This file defines the main public API for creating, managing, and
 * interacting with a LuDLC connection. It includes the user-facing
 * callback structures and the primary functions for initializing,
 * destroying, and sending data over a connection.
 *
 * Copyright (C) 2025 Andrey VOLKOV <andrey@volkov.fr> and LuDLC Contributors
 *
 * This file is licensed under either the Apache License, Version 2.0,
 * or the GNU General Public License, version 2 or (at your option)
 * any later version.
 */

#ifndef __LUDLC_H__
#define __LUDLC_H__

#include <ludlc_types.h>
#include <ludlc_proto.h>
#include <ludlc_platform_config.h>

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
	 * @param tstamp The timestamp (in microseconds) when the packet
	 * was received by the transport.
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

#include <ludlc_serial.h>

/**
 * @brief Allocates and initializes a new LuDLC connection state.
 *
 * This function dynamically allocates memory for a `ludlc_connection`
 * structure and then calls `ludlc_connection_init` on it.
 *
 * @param proto_cb Pointer to the platform/transport callbacks.
 * @param cb Pointer to the user event callbacks.
 * @param user_ctx A user-defined context pointer passed to all callbacks.
 * @return A pointer to the newly created connection, or NULL on failure.
 */
struct ludlc_connection *ludlc_create_connection(
		const struct ludlc_proto_cb *proto_cb,
		const struct ludlc_conn_cb *cb,
		void *user_ctx);
/**
 * @brief De-initializes and frees a LuDLC connection.
 *
 * This function calls `ludlc_connection_cleanup` and then frees the
 * memory associated with the connection.
 *
 * @param conn The connection to destroy.
 */
void ludlc_destroy_connection(struct ludlc_connection *conn);

/**
 * @brief Initializes a pre-allocated LuDLC connection structure.
 *
 * This is primarily used as a part of specialized connection
 * allocation/deallocation schemes (e.g., static or pool allocation).
 *
 * @param conn Pointer to the pre-allocated connection structure to
 * initialize.
 * @param proto_cb Pointer to the platform/transport callbacks.
 * @param cb Pointer to the user event callbacks.
 * @param user_ctx A user-defined context pointer passed to all callbacks.
 * @return 0 on success, or a negative error code on failure.
 */
 int ludlc_connection_init(
		struct ludlc_connection *conn,
		const struct ludlc_proto_cb *proto_cb,
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
 * @param size The size of the payload data in bytes.
 * @param oneshot If true, the packet will only be tried once (TTL=1).
 * If false, it uses the default retry mechanism.
 * @return 0 on success.
 * @return -EAGAIN if the transmit queue (window) is full.
 */
int ludlc_enqueue_data(struct ludlc_connection *conn,
		ludlc_channel_t dst_chan,
		const void *buf, ludlc_payload_size_t size, bool oneshot);

#endif /* __LUDLC_H__ */

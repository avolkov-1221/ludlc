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
 * @brief User-facing event callbacks for a LuDLC connection, and the
 * capabilities it announces.
 *
 * This structure defines the set of callback functions that the LuDLC
 * core will invoke to notify the user application of various events, such
 * as connection status changes, received data, and packet confirmations.
 * It also holds what this end tells its peer about itself in the handshake,
 * which is constant and so costs the connection nothing. Must outlive the
 * connection.
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
	 * @param chan The channel field of the packet: the channel on which
	 * 	       the data was received, with its tag on a link that has
	 *             one. Check @ref ludlc_chan_id() and @ref ludlc_chan_tag()
	 * @param data Pointer to the received payload data buffer.
	 * @param data_size The size of the received payload in bytes.
	 * @param tstamp Monotonic timestamp when the transport received the
	 *               packet. Unit is whatever @ref ludlc_get_timestamp_t
	 *               returns.
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
	 *              error code on failure: -ETIMEDOUT for a packet given up
	 *              once its TTL ran out, -EPIPE for one the link went
	 *              down under, -ECANCELED* for an unnumbered packet a
	 *              DISCONNECT replaced.
	 * @param chan The channel field the packet was sent with,
	 *             its tag included.
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

	/**
	 * @brief Called with what the peer announced in the handshake: its
	 * capabilities, or nothing for a peer that announces none.
	 *
	 * Taken from the handshake step that moves this end out of State 1,
	 * so this runs once before every @c on_connect, and a link never
	 * comes up without it.
	 *
	 * @param conn The connection being established.
	 * @param data The peer's capabilities, valid during the call only,
	 *             or NULL when it announced none.
	 * @param data_size Their size in bytes, zero when it announced none.
	 * @param user_arg The user-defined context pointer.
	 */
	void (*on_caps)(struct ludlc_connection *conn,
			const void *data,
			ludlc_payload_size_t data_size,
			void *user_arg);

	/**
	 * @brief The capabilities this end announces to the peer, or NULL.
	 *
	 * Sent whole with the handshake steps of States 1 and 2, so they must
	 * fit in one packet of the link, and must not change: the peer takes
	 * them from whichever of the two moves it out of State 1, which may
	 * be either. Their layout belongs to the application. Borrowed, and
	 * must outlive the connection.
	 */
	const void *caps;

	/** @brief Octets at @c caps, at most the payload the link carries. */
	ludlc_payload_size_t caps_size;
};

/**
 * @brief Initializes a pre-allocated LuDLC connection structure.
 *
 * This is primarily used as a part of specialized connection
 * allocation/deallocation schemes (e.g., static or pool allocation).
 *
 * @param conn Pointer to the pre-allocated connection structure to
 * initialize.
 * @param cfg The link's clock and protocol settings, or NULL for
 * @ref ludlc_conn_cfg_default. Copied at init, so it need not outlive the
 * call, but the clock it names must, being kept. The settings below the clock
 * are read only where the build has @ref CONFIG_LUDLC_MULTI_CONN; with a
 * single connection they are the CONFIG_* constants and the link keeps none
 * of them.
 * @param cb Pointer to the user event callbacks.
 * @param user_ctx A user-defined context pointer passed to all callbacks.
 * @return 0 on success.
 * @return -EINVAL if @p conn is NULL, if @p cfg names a checksum, a frame, a
 * window, a TTL or a tag this build cannot hold, or if the capabilities in
 * @p cb do not fit in one packet of the link.
 * @return -ENOMEM if the queue the window needs could not be allocated. A
 * build with a single connection embeds that queue in the connection and
 * never returns this.
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
 * @brief The largest payload @p conn carries, in octets.
 *
 * The frame size the link was opened with, less the header and this link's
 * checksum, so a link on a narrow checksum - or on none - carries more
 * payload. @ref ludlc_enqueue_data enforces this limit, so a caller with more
 * to send has to split it.
 */
ludlc_payload_size_t ludlc_conn_max_payload(
		const struct ludlc_connection *conn);

/**
 * @name The tag in the channel field
 *
 * A link with a tag splits the channel field of every packet in two: the bits
 * below the tag select the channel, and the tag belongs to the application.
 * Each link sets the width of its tag in its @ref ludlc_conn_cfg, unless the
 * build runs @ref CONFIG_LUDLC_CHAN_TAG_BITS on every link, as it does with
 * one connection or with @ref CONFIG_LUDLC_COMMON_CHAN_TAG.
 *
 * @ref ludlc_enqueue_data and @ref ludlc_send_urgent_data take the whole
 * field, and @c on_recv and @c on_confirm pass it back as it was sent, so a
 * tag reaches the peer and comes back with the confirmation. On a link with
 * no tag, and for a NULL @p conn, the helpers below return the channel
 * unchanged and a zero tag.
 *
 * The control channel carries no tag. A caller cannot send on it with one,
 * and a packet that arrives with one is dropped.
 * @{
 */

/**
 * @brief The channel field for channel @p id with @p tag on @p conn.
 *
 * Bits of @p id or @p tag that do not fit their part are dropped.
 */
ludlc_channel_t ludlc_chan_make(const struct ludlc_connection *conn,
		ludlc_channel_t id, ludlc_channel_t tag);

/** @brief The channel @p chan selects on @p conn, without its tag. */
ludlc_channel_t ludlc_chan_id(const struct ludlc_connection *conn,
		ludlc_channel_t chan);

/** @brief The tag @p chan carries on @p conn. */
ludlc_channel_t ludlc_chan_tag(const struct ludlc_connection *conn,
		ludlc_channel_t chan);

/** @} */

/**
 * @brief Enqueues a data packet for transmission.
 *
 * Adds a data payload to the transmit queue. The core logic will
 * automatically handle packetization, acknowledgment, and retries.
 *
 * @param conn The connection to send data on.
 * @param dst_chan The destination channel for the packet, with its tag on a
 * link that has one. See @ref ludlc_chan_make.
 * @param buf Pointer to the payload data buffer.
 * @param size The size of the payload, up to what this link carries. See
 * @ref ludlc_conn_max_payload.
 * @param ttl How many times the packet may be sent and lost. Each copy the
 * peer reports missing costs one, and once none is left the packet is given
 * up and confirmed with -ETIMEDOUT, while those queued behind it go on. A
 * packet re-transmitted after another packet's gap is not charged for it. A
 * value of 0 means a single attempt. Values above @ref LUDLC_MAX_TTL are
 * clamped.
 * @return 0 on success.
 * @return -EINVAL if @p buf is NULL and @p size is not, if @p size is too
 * large, or if @p dst_chan selects @ref CONFIG_LUDLC_CONTROL_CHANNEL, with a
 * tag or without, that channel being reserved for the core.
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
 * @param dst_chan The destination channel for the packet, with its tag on a
 * link that has one. See @ref ludlc_chan_make.
 * @param buf Pointer to the payload data buffer. As for
 * @ref ludlc_enqueue_data, the buffer is referenced rather than copied and
 * must stay valid until the packet is confirmed.
 * @param size The size of the payload, up to what this link carries. See
 * @ref ludlc_conn_max_payload.
 * @return 0 on success.
 * @return -EINVAL if @p buf is NULL, if @p size is 0 or too large, or if
 * @p dst_chan selects @ref CONFIG_LUDLC_CONTROL_CHANNEL, with a tag or
 * without, that channel being reserved for the core.
 * @return -ENOTCONN if the connection is not established. Such a packet is
 * sent immediately or not at all, so it does not wait for a link.
 * @return -EAGAIN if the previous unnumbered packet has not been sent yet.
 * Only one is held at a time.
 */
int ludlc_send_urgent_data(struct ludlc_connection *conn,
		ludlc_channel_t dst_chan,
		const void *buf, ludlc_payload_size_t size);

/**
 * @brief Tells the peer that this side is leaving.
 *
 * Sends @ref LUDLC_CTRL_DISCONNECT as an unnumbered packet on
 * @ref CONFIG_LUDLC_CONTROL_CHANNEL, so it goes out ahead of anything queued
 * and takes no room in the window. The peer ends the session on it rather
 * than waiting out its watchdog.
 *
 * This side goes down once the frame has been sent, and @c on_disconnect
 * reports it as it would any other disconnect. Packets still queued are
 * failed with @c -EPIPE, so a caller with data that must delivered should wait
 * until it confirmed first. An unnumbered packet still waiting to be sent is
 * dropped and confirmed with @c -ECANCELED, the DISCONNECT being the
 * last frame of the session.
 *
 * @param conn The connection to send the disconnect on.
 * @return 0 on success.
 * @return -EINVAL if @p conn is NULL.
 * @return -ENOTCONN if there is no session to end.
 * @return -EAGAIN if an unnumbered packet is on its way out as this runs, its
 * buffer being held by the transport.
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

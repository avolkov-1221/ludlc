// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file ludlc_proto.h
 *
 * @brief LuDLC protocol parameters, packet-related types, and core service hooks.
 *
 * This header holds compile-time protocol limits (window size, ping period,
 * payload bounds, channel IDs), typedefs for IDs and sizes, and the small
 * @ref ludlc_proto_cb table the engine uses for checksum and time—**not**
 * for serial framing or byte I/O (those live in the transport layer).
 *
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * This file is licensed under either the Apache License, Version 2.0,
 * or the GNU General Public License, version 2 or (at your option)
 * any later version.
 */

#ifndef __LUDLC_PROTO_H__
#define __LUDLC_PROTO_H__

/**
 * @def CONFIG_LUDLC_WINDOW
 * @brief Defines the maximal size of the packets waiting queue (sliding window).
 *
 * This value **must** be a power of 2.
 * Defaults to 4 if not specified.
 */
#ifndef CONFIG_LUDLC_WINDOW
#define CONFIG_LUDLC_WINDOW	4
#elif (CONFIG_LUDLC_WINDOW) == 0 || \
	(((CONFIG_LUDLC_WINDOW) & ((CONFIG_LUDLC_WINDOW) - 1U)) != 0)
#error Please define the CONFIG_LUDLC_WINDOW as power of 2
#endif

/**
 * @def LUDLC_WINDOW_MASK
 * @brief A bitmask derived from @ref CONFIG_LUDLC_WINDOW.
 *
 * Used to wrap around packet ID numbers to get an index into the
 * `packets_q` array.
 */
#define LUDLC_WINDOW_MASK	((CONFIG_LUDLC_WINDOW) - 1U)

/**
 * @def CONFIG_LUDLC_ID_TYPE
 * @brief Defines the underlying C type for packet IDs (sequence numbers).
 *
 * Defaults to `uint8_t`. This type must be unsigned.
 */
#ifndef CONFIG_LUDLC_ID_TYPE
#define CONFIG_LUDLC_ID_TYPE	uint8_t
#endif
/**
 * @typedef ludlc_id_t
 * @brief Type definition for LuDLC packet sequence/acknowledgment IDs.
 *
 * The size of this type determines the maximum sequence number before
 * wrap-around. The highest bit is reserved for PING/NAK flags.
 */
typedef CONFIG_LUDLC_ID_TYPE	ludlc_id_t;

/**
 * @brief Compile-time check to ensure `ludlc_id_t` is an unsigned type.
 *
 * This check is essential because the protocol's sequence number
 * arithmetic relies on unsigned wrap-around behavior.
 */
#if defined(BUILD_ASSERT)
BUILD_ASSERT(((ludlc_id_t)0 < (ludlc_id_t)-1));
#elif defined(BUILD_BUG_ON)
BUILD_BUG_ON(!((ludlc_id_t)0 < (ludlc_id_t)-1));
#else
/* C11-style build-time check without static_assert (borrowed from Zephyr). */
enum __build_assert_enum_ludlc_id_t__ {
	__build_assert_enum_ludlc_id_t__ = 1 / !!((ludlc_id_t)0 < (ludlc_id_t)-1)
};
#endif

/**
 * @def CONFIG_LUDLC_PING_TIME
 * @brief The time between heartbeat PING (idle) packets, in microseconds.
 *
 * Defaults to 1,000,000 us (1 second).
 */
#ifndef CONFIG_LUDLC_PING_TIME
#define CONFIG_LUDLC_PING_TIME	1000000UL
#endif

/**
 * @def CONFIG_LUDLC_PAYLOAD_TYPE
 * @brief Defines the C type used to store payload sizes.
 *
 * The type size should be enough to handle the maximum packet size.
 * Defaults to `uint8_t`.
 */
#ifndef CONFIG_LUDLC_PAYLOAD_TYPE
#define CONFIG_LUDLC_PAYLOAD_TYPE	uint8_t
#endif
/**
 * @typedef ludlc_payload_size_t
 * @brief Type definition for storing the size of a packet's payload.
 */
typedef CONFIG_LUDLC_PAYLOAD_TYPE	ludlc_payload_size_t;

/**
 * @def CONFIG_LUDLC_MAX_PAYLOAD_SIZE
 * @brief Defines the maximum packet's payload size in bytes.
 *
 * Defaults to 32 bytes.
 */
#ifndef CONFIG_LUDLC_MAX_PAYLOAD_SIZE
#define CONFIG_LUDLC_MAX_PAYLOAD_SIZE	32U
#endif
/**
 * @def LUDLC_MAX_PACKET_SIZE
 * @brief The total maximum size of a LuDLC packet in memory.
 *
 * This includes the maximum payload size plus the size of the
 * `ludlc_packet_t` structure (which includes the header).
 */
#define LUDLC_MAX_PACKET_SIZE	\
	(CONFIG_LUDLC_MAX_PAYLOAD_SIZE + sizeof(ludlc_packet_t))

/**
 * @def CONFIG_LUDLC_CHANNEL_TYPE
 * @brief Defines the C type used for channel identifiers.
 *
 * Defaults to `uint8_t`.
 */
#ifndef CONFIG_LUDLC_CHANNEL_TYPE
#define CONFIG_LUDLC_CHANNEL_TYPE	uint8_t
#endif
/**
 * @typedef ludlc_channel_t
 * @brief Type definition for packet channel identifiers.
 */
typedef CONFIG_LUDLC_CHANNEL_TYPE	ludlc_channel_t;

/**
 * @def CONFIG_LUDLC_CONTROL_CHANNEL
 * @brief The reserved channel ID used for handshake and control packets.
 *
 * Defaults to channel 0.
 */
#ifndef CONFIG_LUDLC_CONTROL_CHANNEL
#define CONFIG_LUDLC_CONTROL_CHANNEL	0
#endif

#define LUDLC_MAX_TTL		127

/**
 * @def CONFIG_LUDLC_DEFAULT_TTL
 * @brief Transmit retry budget (TTL) for queued packets before failing them.
 *
 * If defined, must lie in @c [1, 126]. When not defined, @ref LUDLC_DEFAULT_TTL
 * falls back to 4.
 */
#ifdef CONFIG_LUDLC_DEFAULT_TTL
#if CONFIG_LUDLC_DEFAULT_TTL >= LUDLC_MAX_TTL || CONFIG_LUDLC_DEFAULT_TTL < 1
#error The CONFIG_LUDLC_DEFAULT_TTL value is out of range [1; 126], please fix it!
#endif
#define LUDLC_DEFAULT_TTL	CONFIG_LUDLC_MAX_TTL
#else
#define LUDLC_DEFAULT_TTL	4
#endif

/**
 * @def CONFIG_LUDLC_CONN_CSUM_CONFIG
 * @brief Enables per-connection checksum init/verify/host->wire settings
 * in @ref ludlc_proto_cb.
 *
 * Disabled by default to keep @ref ludlc_proto_cb minimal and preserve legacy
 * macro-only checksum behavior.
 */
#ifndef CONFIG_LUDLC_CONN_CSUM_CONFIG
#define CONFIG_LUDLC_CONN_CSUM_CONFIG 0
#endif

/**
 * @struct ludlc_proto_cb
 * @brief Services the LuDLC engine needs from below: running CRC and a monotonic clock.
 *
 * Pass a filled-in instance to @c ludlc_connection_init(). The core uses it for
 * packet integrity and protocol timing (microsecond timestamps).
 * **Framing and encoding are not part of this structure** - they are implemented by
 * the serial (or other) transport.
 *
 */
struct ludlc_proto_cb {
	/**
	 * @brief (Optional/Mandatory) Calculates the checksum of a single byte.
	 *
	 * This is a running checksum function. It's a
	 * mandatory one if hardware has not integrated packet
	 * validation IP (like typical UARTs)
	 *
	 * @param csum The current checksum value.
	 * @param data The new byte to add to the checksum.
	 * @return The updated checksum value.
	 */
	ludlc_csum_t (*csum_byte)(ludlc_csum_t csum, uint8_t data);
#if CONFIG_LUDLC_CONN_CSUM_CONFIG
	/**
	 * @brief Initial checksum seed for this connection.
	 *
	 * Overrides @ref LUDLC_CSUM_INIT_VALUE for this connection.
	 */
	ludlc_csum_t csum_init_value;
	/**
	 * @brief Expected residual checksum after validating [payload][csum].
	 *
	 * Overrides @ref LUDLC_CSUM_VERIFY_VALUE for this connection.
	 */
	ludlc_csum_t csum_verify_value;
	/**
	 * @brief Optional host->wire checksum conversion for this connection.
	 *
	 * If NULL, @ref LUDLC_CSUM_HTON is used.
	 */
	ludlc_csum_t (*csum_to_wire)(ludlc_csum_t csum);
#endif
	/**
	 * @brief Monotonic time in microseconds (e.g. @c CLOCK_MONOTONIC on POSIX).
	 *
	 * **Mandatory.** Used for RTT-oriented logic and timers; must not jump
	 * backward when the wall clock is adjusted.
	 *
	 * @param[out] ts Filled with the current time on success; left
	 * unchanged on failure.
	 * @return 0 on success, or a negative value (e.g. \c -EINVAL
	 * if \a ts is NULL, or \c -errno from the platform clock API).
	 */
	int (*get_timestamp)(ludlc_timestamp_t *ts);
};

/**
 * @brief Default @c get_timestamp for POSIX builds (monotonic, microseconds).
 * @see ludlc_proto_cb::get_timestamp
 */
int ludlc_default_get_timestamp(ludlc_timestamp_t *out_ts);

#endif /* __LUDLC_PROTO_H__ */

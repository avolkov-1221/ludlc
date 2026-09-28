// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only)
/**
 * @file ludlc_proto.h
 *
 * @brief LuDLC protocol parameters, packet-related types, and core service
 * hooks.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * Compile-time protocol limits (window size, ping period, payload bounds,
 * channel IDs), ID/size typedefs, and the monotonic timestamp callback the
 * core keeps on each connection. Serial framing, checksums and byte I/O live
 * in the transport layer.
 */

#ifndef __LUDLC_PROTO_H__
#define __LUDLC_PROTO_H__

#include <ludlc_types.h>

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
 * @def CONFIG_LUDLC_ID_BITS
 * @brief Defines the bit-width for packet IDs (sequence numbers).
 *
 * Defaults to 8 bits.
 */
#ifndef CONFIG_LUDLC_ID_BITS
#define CONFIG_LUDLC_ID_BITS 8
#endif
/**
 * @typedef ludlc_id_t
 * @brief Type definition for LuDLC packet sequence/acknowledgment IDs.
 *
 * The size of this type determines the maximum sequence number before
 * wrap-around. The highest bit is reserved for PING/NAK flags.
 */
#if CONFIG_LUDLC_ID_BITS == 8
typedef uint8_t ludlc_id_t;
#elif CONFIG_LUDLC_ID_BITS == 16
typedef uint16_t ludlc_id_t;
#elif CONFIG_LUDLC_ID_BITS == 32
typedef uint32_t ludlc_id_t;
#else
#error Unsupported CONFIG_LUDLC_ID_BITS value (supported: 8, 16, 32)
#endif

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
 * @def CONFIG_LUDLC_PAYLOAD_BITS
 * @brief Defines the bit-width used to store payload sizes.
 *
 * Defaults to 8 bits.
 */
#ifndef CONFIG_LUDLC_PAYLOAD_BITS
#define CONFIG_LUDLC_PAYLOAD_BITS 8
#endif
/**
 * @typedef ludlc_payload_size_t
 * @brief Type definition for storing the size of a packet's payload.
 */
#if CONFIG_LUDLC_PAYLOAD_BITS == 8
typedef uint8_t ludlc_payload_size_t;
#if CONFIG_LUDLC_MAX_PAYLOAD_SIZE > UINT8_MAX
#error CONFIG_LUDLC_MAX_PAYLOAD_SIZE exceeds 8-bit payload capacity
#endif
#elif CONFIG_LUDLC_PAYLOAD_BITS == 16
typedef uint16_t ludlc_payload_size_t;
#if CONFIG_LUDLC_MAX_PAYLOAD_SIZE > UINT16_MAX
#error CONFIG_LUDLC_MAX_PAYLOAD_SIZE exceeds 16-bit payload capacity
#endif
#elif CONFIG_LUDLC_PAYLOAD_BITS == 32
typedef uint32_t ludlc_payload_size_t;
#if CONFIG_LUDLC_MAX_PAYLOAD_SIZE > UINT32_MAX
#error CONFIG_LUDLC_MAX_PAYLOAD_SIZE exceeds 32-bit payload capacity
#endif
#else
#error Unsupported CONFIG_LUDLC_PAYLOAD_BITS value (supported: 8, 16, 32)
#endif

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
#define LUDLC_MAX_PACKET_SIZE                                                  \
	(CONFIG_LUDLC_MAX_PAYLOAD_SIZE + sizeof(ludlc_packet_t))

/**
 * @def CONFIG_LUDLC_CHANNEL_BITS
 * @brief Defines the bit-width used for channel identifiers.
 *
 * Defaults to 8 bits.
 */
#ifndef CONFIG_LUDLC_CHANNEL_BITS
#define CONFIG_LUDLC_CHANNEL_BITS 8
#endif
/**
 * @typedef ludlc_channel_t
 * @brief Type definition for packet channel identifiers.
 */
#if CONFIG_LUDLC_CHANNEL_BITS == 8
typedef uint8_t ludlc_channel_t;
#elif CONFIG_LUDLC_CHANNEL_BITS == 16
typedef uint16_t ludlc_channel_t;
#elif CONFIG_LUDLC_CHANNEL_BITS == 32
typedef uint32_t ludlc_channel_t;
#else
#error Unsupported CONFIG_LUDLC_CHANNEL_BITS value (supported: 8, 16, 32)
#endif

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
#define LUDLC_DEFAULT_TTL CONFIG_LUDLC_DEFAULT_TTL
#else
#define LUDLC_DEFAULT_TTL	4
#endif

/**
 * @typedef ludlc_get_timestamp_t
 * @brief Monotonic timestamp callback kept on each connection.
 *
 * Any time unit is fine so long as the clock is monotonic and its resolution
 * can tell two sequential packets apart. Must not jump backward when the wall
 * clock is adjusted. Kept for the life of the connection.
 *
 * @param[out] ts Filled with the current time on success; left unchanged on
 * failure.
 * @return 0 on success, or a negative value (e.g. \c -EINVAL if \a ts is NULL,
 * or \c -errno from the platform clock API).
 */
typedef int (*ludlc_get_timestamp_t)(ludlc_timestamp_t *ts);

/**
 * @brief Default monotonic timestamp.
 *
 * Returns timestamp from a monotonic clock. Any units are equally valid
 * for a custom callback; see @ref ludlc_get_timestamp_t.
 */
int ludlc_default_get_timestamp(ludlc_timestamp_t *out_ts);

/**
 * @struct ludlc_csum
 * @brief Checksum a link runs.
 *
 * The transport computes it, but both ends of a link must run the same one,
 * so it is a property of the network and is named in the platform arguments
 * the link is opened with. A transport that validates frames in hardware
 * needs none.
 *
 * All the core reads from it is @c octets, the width being what a frame gives
 * up to the checksum and therefore what separates a frame size from the
 * payload that fits in it. Must outlive the connection opened on it.
 */
struct ludlc_csum {
	const char *name;
	/**
	 * @brief Running checksum over one octet. **Mandatory.**
	 *
	 * Called for each frame octet, before escaping on TX and after
	 * unescaping on RX.
	 *
	 * @param csum Checksum so far.
	 * @param data Octet to add.
	 * @return Checksum including @p data.
	 */
	ludlc_csum_t (*csum)(ludlc_csum_t csum, uint8_t data);
	/**
	 * @brief Optional last transform before the checksum goes on the wire.
	 *
	 * A value transform, not an octet order: the framing always sends the
	 * checksum least significant octet first. Holds a final xor (HDLC
	 * FCS-16), or a swap for an algorithm that sends it the other way
	 * (CRC-16/XMODEM against CRC-16/KERMIT).
	 */
	ludlc_csum_t (*to_wire)(ludlc_csum_t csum);
	/** Seed each frame starts at. */
	ludlc_csum_t init;
	/**
	 * @brief Residual expected over [payload][checksum].
	 *
	 * RX runs the checksum across the checksum octets too; an intact frame
	 * always leaves this constant. Zero for a zero-seeded checksum sent as
	 * computed, 0xf0b8 for HDLC FCS-16.
	 */
	ludlc_csum_t verify;
	/**
	 * @brief How many octets are sent on the wire, i.e. the
	 * algorithm's width. **Mandatory.**
	 *
	 * @ref ludlc_csum_t is only the register to compute in and may be
	 * wider: one build may carry 16 bits on one link and 32 on the next.
	 */
	uint8_t octets;
};

/**
 * @struct ludlc_conn_cfg
 * @brief Everything a link is opened with.
 *
 * Read at @ref ludlc_connection_init and copied, never borrowed, so a
 * compound literal will do. A field left at zero takes the build's default;
 * @ref LUDLC_CONN_CFG_DEFAULT spells those defaults out.
 *
 * The checksum is not here. It belongs to the transport.
 */
struct ludlc_conn_cfg {
	/**
	 * @brief The clock this link timestamps packets with.
	 *
	 * NULL for @ref ludlc_default_get_timestamp, the platform's own
	 * monotonic clock. A callback of one's own must outlive the
	 * connection, which keeps it.
	 */
	ludlc_get_timestamp_t get_timestamp;
};

/**
 * @def LUDLC_CONN_CFG_DEFAULT
 * @brief Initializer from the CONFIG_* defaults.
 *
 * A zero clock asks for the platform's own.
 */
#define LUDLC_CONN_CFG_DEFAULT {					\
	.get_timestamp = NULL,						\
}

/** @brief Shared instance of @ref LUDLC_CONN_CFG_DEFAULT. */
extern const struct ludlc_conn_cfg ludlc_conn_cfg_default;

#endif /* __LUDLC_PROTO_H__ */

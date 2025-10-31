// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file ludlc_proto.h
 *
 * @brief LuDLC protocol interface callbacks and type definitions.
 *
 * This file defines the transport-layer callbacks, protocol-wide types
 * (like packet IDs and sizes), and configurable parameters (like
 * window size and ping times) required for a LuDLC implementation.
 *
 * Copyright (C) 2025 Andrey VOLKOV <andrey@volkov.fr> and LuDLC Contributors
 *
 * This file is licensed under either the Apache License, Version 2.0,
 * or the GNU General Public License, version 2 or (at your option)
 * any later version.
 */

#ifndef __LUDLC_PROTO_H__
#define __LUDLC_PROTO_H__

#include <stdint.h>
#include <ludlc_platform_config.h>

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
BUILD_ASSERT(((ludlc_id_t)0 < (ludlc_id_t)-1))
#elif defined(BUILD_BUG_ON)
BUILD_BUG_ON(!((ludlc_id_t)0 < (ludlc_id_t)-1))
#else
/* Simulate static_assert() from C11 (borrowed from Zephyr */
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

/**
 * @struct ludlc_proto_cb
 * @brief Transport-layer callback structure.
 *
 * This structure defines the set of functions that the platform/transport
 * must provide to the LuDLC core. These functions handle the actual
 * reading and writing of bytes to the physical medium, as well as
 * checksumming and timestamping.
 */
struct ludlc_proto_cb {
	/**
	 * @brief (Optional) Called to initialize the transmitter hardware.
	 * @param user_arg The user context pointer from the connection.
	 * @return 0 on success, negative error code on failure.
	 */
	int (*tx_start)(void *user_arg);
	/**
	 * @brief (Optional) Called to de-initialize the transmitter hardware.
	 * @param user_arg The user context pointer from the connection.
	 * @return 0 on success, negative error code on failure.
	 */
	int (*tx_stop)(void *user_arg);
	/**
	 * @brief (Mandatory) Writes a block of data to the transmitter hardware.
	 *
	 * This function should be non-blocking or have its own timeout.
	 *
	 * @param user_arg The user context pointer from the connection.
	 * @param buf Pointer to the data buffer to write.
	 * @param sz The number of bytes to write.
	 * @return The number of bytes written, or a negative error code.
	 */
	ssize_t (*tx_write)(void *user_arg, const void *buf, size_t sz);

	/**
	 * @brief (Optional) Called to initialize the receiver hardware.
	 * @param user_arg The user context pointer from the connection.
	 * @return 0 on success, negative error code on failure.
	 */
	int (*rx_start)(void *user_arg);
	/**
	 * @brief (Optional) Called to de-initialize the receiver hardware.
	 * @param user_arg The user context pointer from the connection.
	 * @return 0 on success, negative error code on failure.
	 */
	int (*rx_stop)(void *user_arg);
	/**
	 * @brief (Mandatory) Reads a block of data from the hardware.
	 *
	 * @param user_arg The user context pointer from the connection.
	 * @param buf Pointer to the buffer to store read data.
	 * @param sz The maximum number of bytes to read.
	 * @param tout_ms Timeout in milliseconds. A negative value
	 * typically means wait indefinitely.
	 * @return The number of bytes read, or a negative error code
	 * (e.g., -EAGAIN on timeout).
	 */
	ssize_t (*rx_read)(void *user_arg, void *buf, size_t sz, int tout_ms);
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
	/**
	 * @brief (Mandatory) Gets the current high-resolution timestamp.
	 *
	 * @return The current time in microseconds.
	 */
	ludlc_timestamp_t (*get_timestamp)(void);
};

#endif /* __LUDLC_PROTO_H__ */


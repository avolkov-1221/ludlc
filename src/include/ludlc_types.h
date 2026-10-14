/* SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only) */
/**
 * @file ludlc_types.h
 *
 * @brief LuDLC common base types.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 */

#ifndef __LUDLC_TYPES_H__
#define __LUDLC_TYPES_H__

#if defined(__linux__) && defined(__KERNEL__)
#include <linux/types.h>
#ifndef UINT8_MAX
#define UINT8_MAX 0xffU
#endif
#ifndef UINT16_MAX
#define UINT16_MAX 0xffffU
#endif
#ifndef UINT32_MAX
#define UINT32_MAX 0xffffffffU
#endif
#define LUDLC_LINUX_KERNEL	1
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#endif

/**
 * @struct ludlc_connection
 * @brief Opaque structure representing a single LuDLC connection.
 *
 * This structure holds the complete internal state of a connection.
 * It is initialized by `ludlc_connection_init`, or by a transport helper
 * such as `ludlc_serial_connection_create`, and passed to all API functions.
 */
struct ludlc_connection;

/*
 * The checksum belongs to the transport: algorithm, seed, width and wire
 * transform all come from the struct ludlc_csum a serial link was opened
 * with, and two links in one system may differ. The type below is only the
 * register they are computed in, so its width is that of the widest algorithm
 * a build allows, not of any one link. A small target has reason to keep that
 * register narrow, hence the build-wide setting; the kernel does not, so it
 * always uses u64.
 */
#if defined(LUDLC_LINUX_KERNEL)
#if defined(CONFIG_LUDLC_CSUM_BITS)
#error The LuDLC checksum register is always 64-bit in the Linux kernel; a link chooses its own width.
#endif
typedef u64 ludlc_csum_t;
#else
#ifndef CONFIG_LUDLC_CSUM_BITS
#	define CONFIG_LUDLC_CSUM_BITS 16
#endif
#if CONFIG_LUDLC_CSUM_BITS == 8
typedef uint8_t ludlc_csum_t;
#elif CONFIG_LUDLC_CSUM_BITS == 16
typedef uint16_t ludlc_csum_t;
#elif CONFIG_LUDLC_CSUM_BITS == 32
typedef uint32_t ludlc_csum_t;
#elif CONFIG_LUDLC_CSUM_BITS == 64
typedef uint64_t ludlc_csum_t;
#else
#error Unsupported CONFIG_LUDLC_CSUM_BITS value (supported: 8, 16, 32, 64)
#endif
#endif

/* --- Timestamps --- */
/*
 * Packet timestamps are monotonic values from ludlc_get_timestamp_t: any unit
 * is fine so long as consecutive packets can be told apart. The width is
 * fixed by the platform - ktime_t on Linux - rather than chosen by Kconfig,
 * because an out-of-tree build never parses ours. The default below is 64
 * bits, so a build that says nothing still works.
 */
#if defined(LUDLC_LINUX_KERNEL)
#if defined(CONFIG_LUDLC_TIMESTAMP_BITS)
#error The LuDLC timestamp is always ktime_t in the Linux kernel.
#endif
typedef ktime_t ludlc_timestamp_t;
#else
#ifndef CONFIG_LUDLC_TIMESTAMP_BITS
#	define CONFIG_LUDLC_TIMESTAMP_BITS 64
#endif

#if CONFIG_LUDLC_TIMESTAMP_BITS == 8
typedef uint8_t ludlc_timestamp_t;
#elif CONFIG_LUDLC_TIMESTAMP_BITS == 16
typedef uint16_t ludlc_timestamp_t;
#elif CONFIG_LUDLC_TIMESTAMP_BITS == 32
typedef uint32_t ludlc_timestamp_t;
#elif CONFIG_LUDLC_TIMESTAMP_BITS == 64
typedef uint64_t ludlc_timestamp_t;
#else
#error Unsupported CONFIG_LUDLC_TIMESTAMP_BITS value (supported: 8, 16, 32, 64)
#endif
#endif

#endif /* __LUDLC_TYPES_H__ */

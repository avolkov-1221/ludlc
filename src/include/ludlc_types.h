// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only)
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

#include <stdint.h>

/**
 * @struct ludlc_connection
 * @brief Opaque structure representing a single LuDLC connection.
 *
 * This structure holds the complete internal state of a connection.
 * It is initialized by `ludlc_create_connection` or `ludlc_connection_init`
 * and passed to all API functions.
 */
struct ludlc_connection;

/*
 * The checksum belongs to the transport: algorithm, seed, width and wire
 * transform all come from the struct ludlc_csum a serial link was opened
 * with, and two links in one system may differ. The type below is only the
 * register they are computed in, so its width is that of the widest algorithm
 * a build allows, not of any one link. A small target has reason to keep that
 * register narrow, hence the build-wide setting.
 */
#ifndef CONFIG_LUDLC_CSUM_BITS
#define CONFIG_LUDLC_CSUM_BITS 16
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

/* --- Timestamps --- */
#ifndef CONFIG_LUDLC_TIMESTAMP_BITS
#define CONFIG_LUDLC_TIMESTAMP_BITS 64
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

#endif /* __LUDLC_TYPES_H__ */

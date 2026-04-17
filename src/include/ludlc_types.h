// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file ludlc_types.h
 *
 * @brief LuDLC common base types.
 *
 * This file defines fundamental types and forward declarations used
 * throughout the LuDLC library.
 *
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * This file is licensed under either the Apache License, Version 2.0,
 * or the GNU General Public License, version 2 or (at your option)
 * any later version.
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

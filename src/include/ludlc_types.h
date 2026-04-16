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

#ifndef CONFIG_LUDLC_CSUM_TYPE
#define CONFIG_LUDLC_CSUM_TYPE uint16_t
#endif
typedef CONFIG_LUDLC_CSUM_TYPE ludlc_csum_t;

/* --- Timestamps --- */
#ifndef CONFIG_LUDLC_TIMESTAMP_TYPE
#define CONFIG_LUDLC_TIMESTAMP_TYPE uint64_t
#endif
typedef CONFIG_LUDLC_TIMESTAMP_TYPE ludlc_timestamp_t;

#endif /* __LUDLC_TYPES_H__ */

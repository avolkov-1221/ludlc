// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file ludlc_types.h
 *
 * @brief LuDLC common base types.
 *
 * This file defines fundamental types and forward declarations used
 * throughout the LuDLC library.
 *
 * Copyright (C) 2025 Andrey VOLKOV <andrey@volkov.fr> and LuDLC Contributors
 *
 * This file is licensed under either the Apache License, Version 2.0,
 * or the GNU General Public License, version 2 or (at your option)
 * any later version.
 */

#ifndef __LUDLC_TYPES_H__
#define __LUDLC_TYPES_H__

/**
 * @struct ludlc_connection
 * @brief Opaque structure representing a single LuDLC connection.
 *
 * This structure holds the complete internal state of a connection.
 * It is initialized by `ludlc_create_connection` or `ludlc_connection_init`
 * and passed to all API functions.
 */
struct ludlc_connection;

/**
 * @typedef ludlc_timer_cb_t
 * @brief Callback function type for platform timers.
 *
 * @param conn Pointer to the LuDLC connection associated with the timer.
 * This allows the callback to access the connection's state.
 */
typedef void (*ludlc_timer_cb_t)(struct ludlc_connection *conn);

#endif /* __LUDLC_TYPES_H__ */


// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file ludlc_platform_api.h
 *
 * @brief LuDLC platform abstraction layer API.
 *
 * @details
 * Declarations for functions the **core** (`ludlc.c`) calls into the platform
 * port. Implementations live in the platform directory (e.g. POSIX, Zephyr).
 * Types, macros, and optional **inline** platform hooks are in
 * @c ludlc_platform.h (included here).
 *
 * Implementations may instead provide inlines by defining the corresponding
 * @c LUDLC_PLATFORM_*_INLINED macros and omitting some of these symbols from
 * the link.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * This file is licensed under either the Apache License, Version 2.0,
 * or the GNU General Public License, version 2 or (at your option)
 * any later version.
 */

#ifndef __LUDLC_PLATFORM_API_H__
#define __LUDLC_PLATFORM_API_H__

#include <ludlc_types.h>
#include <ludlc_platform.h>

/**
 * @defgroup ludlc_platform_api LuDLC platform API (functions)
 * @{
 */

/**
 * @brief Bind a platform timer to a connection and a LuDLC expiry callback.
 *
 * @param[in] conn   Connection passed to @a cb when the timer fires (platform
 *                   stores this; must remain valid until the timer is destroyed).
 * @param[in,out] timer Opaque timer object (storage provided by @c ludlc_connection).
 * @param[in] cb     Callback invoked on expiration; signature is
 *                   @ref ludlc_timer_cb_t. Context may be ISR or timer thread —
 *                   see platform notes.
 *
 * @retval 0        Success.
 * @retval -EINVAL  @a timer or @a cb invalid (platform-defined).
 * @retval <0       Other platform error (e.g. @c -errno).
 */
int ludlc_platform_init_timer(struct ludlc_connection *conn,
		ludlc_platform_timer_t *timer,
		ludlc_timer_cb_t cb);

/**
 * @brief Arm or re-arm a timer created with @ref ludlc_platform_init_timer.
 *
 * @param[in,out] timer  Initialized timer handle.
 * @param[in] delay      Time until first expiry, in **microseconds** (relative).
 * @param[in] period     Interval between subsequent expiries, in microseconds.
 *                       If @c 0, the timer fires once only.
 *
 * @retval 0       Success.
 * @retval -EINVAL @a timer is NULL or parameters invalid.
 * @retval <0      Platform error (e.g. @c -errno).
 *
 * @note Stopping or restarting the timer from within the expiry callback is
 *       platform-specific; the core uses separate ping and watchdog timers.
 */
int ludlc_platform_start_timer(ludlc_platform_timer_t *timer,
				ludlc_timestamp_t delay,
				ludlc_timestamp_t period);

/**
 * @brief Disarm a timer so it will not fire until started again.
 *
 * @param[in,out] timer Timer to stop.
 *
 * @retval 0  Success (including no-op if already stopped).
 * @retval <0 Platform error on failure to disarm.
 *
 * @note The implementation should synchronize with any in-flight expiry so that
 *       after this returns, the LuDLC callback is not running unless the
 *       platform guarantees that separately.
 */
int ludlc_platform_stop_timer(ludlc_platform_timer_t *timer);

/**
 * @brief Release OS resources held by @a timer.
 *
 * @param[in,out] timer Timer to tear down. Safe to call on an uninitialized
 *                      or already-destroyed timer if the platform defines that.
 *
 * @post The timer must not be passed to @ref ludlc_platform_start_timer until
 *       @ref ludlc_platform_init_timer is called again.
 */
void ludlc_platform_destroy_timer(ludlc_platform_timer_t *timer);

/**
 * @brief Recover @ref ludlc_connection from the timer argument passed to
 *        @ref ludlc_timer_cb_t.
 *
 * @param[in] arg Value supplied to the timer callback (platform-specific;
 *                often the timer handle or user pointer set at init).
 * @return Connection associated with this timer at @ref ludlc_platform_init_timer.
 *
 * @note Must be consistent with how @ref ludlc_platform_init_timer stores @a conn.
 */
struct ludlc_connection *ludlc_timer_arg_to_conn(ludlc_platform_timer_arg_t arg);

/**
 * @brief Wake the transmit path so the next frame can be sent soon.
 *
 * Sets an internal “force TX” indication and, on threaded transports, unblocks
 * the TX worker (e.g. pipe/event write). Used when the core has something to
 * send (handshake, data, ACK/PING) or must run the transmitter without new data
 * (e.g. keep-alive).
 *
 * @param[in,out] conn Active connection.
 *
 * @note Callable from timer callbacks and other threads; must be async-signal-safe
 *       only if your port invokes it from signal handlers (usually avoid that).
 */
void ludlc_platform_request_tx(struct ludlc_connection *conn);

/**
 * @brief Hook for link watchdog expiry (optional transport wake-up).
 *
 * Invoked from the connection watchdog timer path when the peer has been silent
 * too long. The platform may unblock a blocking @c read/poll path or schedule
 * work so the stack can observe @c LUDLC_CONN_TIMEOUT_F.
 *
 * @param[in,out] conn Connection whose watchdog fired.
 *
 * @note Not all ports need this; a minimal stub is valid if timeout is polled
 *       elsewhere.
 */
void ludlc_platform_conn_timeout(struct ludlc_connection *conn);

/**
 * @brief Tear down per-connection platform resources (pipes, events, etc.).
 *
 * Called from @ref ludlc_connection_cleanup after timers are stopped. Must not
 * touch the core connection state beyond @c conn->pconn.
 *
 * @param[in,out] conn Connection being destroyed.
 */
void ludlc_platform_conn_destroy(struct ludlc_connection *conn);

/**
 * @brief Allocate and initialize per-connection platform resources.
 *
 * Called during @ref ludlc_connection_init before timers are created.
 *
 * @param[in,out] conn Connection to set up.
 *
 * @retval 0  Success.
 * @retval <0 Failure (e.g. @c -errno); connection init should abort.
 */
int ludlc_platform_conn_init(struct ludlc_connection *conn);

/** @} */

#endif /* __LUDLC_PLATFORM_API_H__ */

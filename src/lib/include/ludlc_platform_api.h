// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file ludlc_platform_api.h
 *
 * @brief LuDLC platform abstraction layer API.
 *
 * This header defines the set of functions that must be implemented by the
 * target platform to provide services like memory allocation, timers, and
 * synchronization primitives (which are defined in ludlc_platform_config.h).
 *
 * These functions can be provided as standard C functions or, for performance,
 * as inline functions by defining the appropriate `LUDLC_PLATFORM_*_INLINED`
 * macros.
 *
 * Copyright (C) 2025 Andrey VOLKOV <andrey@volkov.fr> and LuDLC Contributors
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
 * @brief Initializes a platform-specific timer.
 *
 * @param conn Pointer to the associated LuDLC connection (passed to the callback).
 * @param timer Pointer to the platform's timer handle to be initialized.
 * @param cb The callback function to execute when the timer expires.
 * @return 0 on success, or a negative error code on failure.
 */
int ludlc_platform_init_timer(struct ludlc_connection *conn,
		ludlc_platform_timer_t *timer,
		ludlc_timer_cb_t cb);
/**
 * @brief Starts or restarts a platform timer.
 *
 * @param timer Pointer to the initialized timer handle.
 * @param delay The initial delay before the first expiration, in microseconds.
 * @param period The period for subsequent expirations, in microseconds.
 * If 0, the timer is a one-shot timer.
 * @return 0 on success, or a negative error code on failure.
 */
int ludlc_platform_start_timer(ludlc_platform_timer_t *timer,
				ludlc_timestamp_t delay,
				ludlc_timestamp_t period);
/**
 * @brief Stops a running platform timer.
 *
 * @param timer Pointer to the timer handle to stop.
 * @return 0 on success, or a negative error code on failure.
 */
int ludlc_platform_stop_timer(ludlc_platform_timer_t *timer);

/**
 * @brief Destroy platform timer.
 *
 * @param timer Pointer to the timer handle to delete.
 */
void ludlc_platform_destroy_timer(ludlc_platform_timer_t *timer);

struct ludlc_connection *ludlc_timer_arg_to_conn(ludlc_platform_timer_arg_t arg);

/**
 * @brief Requests an immediate transmission.
 *
 * This function signals the LuDLC transmit task/thread (if any) to wake
 * up and attempt to send a packet. It sets a flag to force transmission
 * even if no new data has been queued (e.g., to send a PING with ACK).
 *
 * @param conn Pointer to the connection structure.
 */
void ludlc_platform_request_tx(struct ludlc_connection *conn);

int ludlc_platform_conn_init(struct ludlc_connection *conn);

#endif /* __LUDLC_PLATFORM_API_H__ */

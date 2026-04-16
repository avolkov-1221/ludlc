// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file ludlc_zephyr_platform.c
 *
 * @brief LuDLC platform abstraction layer for Zephyr.
 *
 * This file defines the platform-specific abstractions required by the LuDLC
 * library when running on the Zephyr RTOS. It includes definitions for timers,
 * locks, checksums, memory allocation, and timers, mapping them to Zephyr's
 * native APIs.
 *
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * This file is licensed under either the Apache License, Version 2.0
 * or the GNU General Public License, version 2 or (at your option)
 * any later version.
 */

#define LUDLC_LOG_REGISTER	1
#include <errno.h>
#include <zephyr/kernel.h>

#include <ludlc_platform.h>
#include <ludlc.h>
#include <ludlc_private.h>

/* --- Public Timer API --- */
int ludlc_platform_init_timer(struct ludlc_connection *conn,
			      ludlc_platform_timer_t *timer,
			      ludlc_timer_cb_t cb)
{
	if (timer) {
		/* Initialize Zephyr's timer with the above trampoline */
		k_timer_init(timer, cb, NULL);
		k_timer_user_data_set(timer, (void *)conn);

		return 0;
	}

	return -EINVAL;
}

/**
 * @brief Retrieves the current system timestamp in microseconds.
 *
 * This function is part of the LuDLC protocol callback structure.
 * @return The current timestamp in microseconds. */
/* --- Timestamp --- */
int ludlc_default_get_timestamp(ludlc_timestamp_t *ts)
{
	if (!ts)
		return -EINVAL;

	if (!IS_ENABLED(CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER)) {
		*ts = (ludlc_timestamp_t)k_cyc_to_us_floor64(k_cycle_get_32());
	} else {
		*ts = (ludlc_timestamp_t)k_cyc_to_us_floor64(k_cycle_get_64());
	}

	return 0;
}

/**
 * @brief Requests an immediate transmission.
 *
 * This function signals the LuDLC transmit task/thread (if any) to wake
 * up and attempt to send a packet. It sets a flag to force transmission
 * even if no new data has been queued (e.g., to send a PING with ACK).
 *
 * @param conn Pointer to the connection structure.
 */
void ludlc_platform_request_tx(struct ludlc_connection *conn)
{
	k_event_post(&conn->pconn.tx_events, LUDLC_CONN_FORCE_TX_F);
}

void ludlc_platform_conn_timeout(struct ludlc_connection *conn)
{

}

int ludlc_platform_conn_init(struct ludlc_connection *conn)
{
	k_event_init(&conn->pconn.tx_events);
	return 0;
}

LOG_MODULE_REGISTER(ludlc);

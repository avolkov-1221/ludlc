// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file ludlc_zephyr_platform.c
 *
 * @brief LuDLC platform abstraction layer for Zephyr.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * The platform-specific parts the LuDLC core needs on the Zephyr RTOS:
 * timers, timestamps and the per-connection wakeups, mapped onto Zephyr's
 * native APIs.
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
		k_timer_init(timer, cb, NULL);
		k_timer_user_data_set(timer, (void *)conn);

		return 0;
	}

	return -EINVAL;
}

/**
 * @brief Default @ref ludlc_get_timestamp_t for Zephyr.
 *
 * Returns current cycle counter value. Any monotonic unit with enough
 * resolution to tell sequential packets apart is valid for a custom callback.
 *
 * @param[out] ts Receives current monotonic time in cycles.
 * @retval 0 on success.
 * @retval -EINVAL if @p ts is NULL.
 */
/* --- Timestamp --- */
int ludlc_default_get_timestamp(ludlc_timestamp_t *ts)
{
	if (!ts)
		return -EINVAL;

	if (!IS_ENABLED(CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER)) {
		*ts = (ludlc_timestamp_t)k_cycle_get_32();
	} else {
		*ts = (ludlc_timestamp_t)k_cycle_get_64();
	}

	return 0;
}

/**
 * @brief Requests an immediate transmission.
 *
 * Wakes the TX thread and forces a send even when no new data has been
 * queued, for example to carry a PING with an ACK.
 *
 * @param conn Pointer to the connection structure.
 */
void ludlc_platform_request_tx(struct ludlc_connection *conn)
{
	k_event_post(&conn->pconn.tx_events, LUDLC_CONN_FORCE_TX_F);
}

void ludlc_platform_conn_timeout(struct ludlc_connection *conn)
{
	k_event_post(&conn->pconn.rx_event, LUDLC_CONN_RX_TOUT_EVT);
}

int ludlc_platform_conn_init(struct ludlc_connection *conn)
{
	k_event_init(&conn->pconn.tx_events);
	k_event_init(&conn->pconn.rx_event);

	return 0;
}

LOG_MODULE_REGISTER(ludlc, LOG_LEVEL_DBG);

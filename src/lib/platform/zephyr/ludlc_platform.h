// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file zephyr/ludlc_platform.h
 *
 * @brief LuDLC platform abstraction layer for Zephyr.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * Defines the platform-specific abstractions the LuDLC library needs on the
 * Zephyr RTOS: atomics, locks, memory allocation and timers, mapped onto
 * Zephyr's native APIs. Checksums are not here; they belong to the transport.
 */

#ifndef __LUDLC_PLATFORM_CONFIG_H__
#define __LUDLC_PLATFORM_CONFIG_H__

#include <sys/types.h>
#include <zephyr/types.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/byteorder.h>

#include <ludlc_types.h>

struct ludlc_platform_connection {
#define LUDLC_CONN_FORCE_TX_F		BIT(0)
#define LUDLC_CONN_TX_EXIT_EVT		BIT(1)
#define LUDLC_PLATFORM_LAST_TX_EVENT	2

	struct k_event		tx_events;

#define LUDLC_CONN_RX_TOUT_EVT		BIT(0)
	struct k_event		rx_event;
};

/* --- Atomics --- */
typedef atomic_t ludlc_platform_atomic_t;

static inline bool ludlc_platform_test_bit(long nr,
					   ludlc_platform_atomic_t *addr)
{
	return atomic_test_bit(addr, nr);
}

static inline void ludlc_platform_set_bit(long nr,
					  ludlc_platform_atomic_t *addr)
{
	atomic_set_bit(addr, nr);
}

static inline void ludlc_platform_clear_bit(long nr,
					    ludlc_platform_atomic_t *addr)
{
	atomic_clear_bit(addr, nr);
}

static inline bool ludlc_platform_test_and_clear_bit(
					long nr,
					ludlc_platform_atomic_t *addr)
{
	return atomic_test_and_clear_bit(addr, nr);
}

static inline bool
ludlc_platform_test_and_set_bit(long nr, ludlc_platform_atomic_t *addr)
{
	return atomic_test_and_set_bit(addr, nr);
}

/* --- Locks --- */
#define LUDLC_DECLARE_LOCK(name)	struct k_mutex name
#define LUDLC_LOCK_INIT(lk)		k_mutex_init(lk)
#define LUDLC_LOCK(lk)			k_mutex_lock(lk, K_FOREVER)
#define LUDLC_UNLOCK(lk)		k_mutex_unlock(lk)

/* --- Allocation --- */
static inline void *ludlc_platform_alloc(size_t sz)
{
	return k_calloc(1, sz);
}

static inline void ludlc_platform_free(void *ptr)
{
	k_free(ptr);
}

/* --- Timers --- */
typedef struct k_timer ludlc_platform_timer_t;
typedef ludlc_platform_timer_t *ludlc_platform_timer_arg_t;

/**
 * @typedef ludlc_timer_cb_t
 * @brief Callback function type for platform timers.
 *
 * @param arg Platform timer argument provided by the timer backend.
 */
typedef void (*ludlc_timer_cb_t)(ludlc_platform_timer_arg_t arg);

static inline struct ludlc_connection *ludlc_timer_arg_to_conn(
						ludlc_platform_timer_arg_t arg)
{
	return k_timer_user_data_get(arg);
}

static inline int ludlc_platform_start_timer(ludlc_platform_timer_t *timer,
					     ludlc_timestamp_t delay,
					     ludlc_timestamp_t period)
{
	if (timer) {
		k_timer_start(timer, K_USEC(delay),
			      period ? K_USEC(period) : K_NO_WAIT);
		return 0;
	}
	return -EINVAL;
}

static inline int ludlc_platform_stop_timer(ludlc_platform_timer_t *timer)
{
	if (timer != NULL) {
		k_timer_stop(timer);
	}
	return 0;
}

static inline void ludlc_platform_destroy_timer(ludlc_platform_timer_t *timer)
{
	ARG_UNUSED(timer);
	/* Do nothing */
}

static inline void ludlc_platform_conn_destroy(struct ludlc_connection *conn)
{
	ARG_UNUSED(conn);
	/* Do nothing */
}

#endif /*__LUDLC_PLATFORM_CONFIG_H__*/

/* SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only) */
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

#define LUDLC_CONTAINER_OF(p, t, m)	CONTAINER_OF(p, t, m)
/** @brief Bits in one @ref ludlc_platform_atomic_t. */
#define LUDLC_BITS_PER_WORD		ATOMIC_BITS

/** @brief Words of ludlc_platform_atomic_t needed to hold @p n bits. */
#define LUDLC_BITS_TO_WORDS(n)		ATOMIC_BITMAP_SIZE(n)

struct ludlc_platform_connection {
#define LUDLC_CONN_FORCE_TX_F		BIT(0)
#define LUDLC_CONN_TX_EXIT_EVT		BIT(1)
/* The watchdog, for a TX thread that also serves the receive path. */

#if IS_ENABLED(CONFIG_LUDLC_SERIAL_SINGLE_THREAD)
#define LUDLC_CONN_RX_TOUT_EVT		BIT(2)
#define LUDLC_PLATFORM_LAST_TX_EVENT	3
	union {
		struct k_event		rx_event;
		struct k_event		tx_events;
	};
#else
#define LUDLC_CONN_RX_TOUT_EVT		BIT(0)
#define LUDLC_PLATFORM_LAST_TX_EVENT	2
#define LUDLC_PLATFORM_LAST_RX_EVENT	1

	struct k_event		rx_event;
	struct k_event		tx_events;
#endif

	/**
	 * @brief Ask the transport for a TX round.
	 *
	 * Posting to @c tx_events is enough for a transport whose TX thread
	 * waits on it. A transport that blocks elsewhere - an SPI peripheral
	 * waits on the controller, having no way to start a transfer itself -
	 * would not see the event until that wait ends, which may be what the
	 * request is meant to bring about. Such a transport sets this
	 * callback, which runs in the caller's context. Optional, NULL when
	 * not needed.
	 */
	void (*start_tx)(struct ludlc_connection *conn);

	/**
	 * @brief Whether the caller runs on a thread of this link.
	 *
	 * Set by the transport, which owns the thread objects and compares
	 * the caller with them, before it starts them. What
	 * ludlc_in_callback() asks; never from an interrupt.
	 */
	bool (*in_callback)(struct ludlc_connection *conn);
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

/**
 * @brief Atomically sets every bit of @p mask.
 * @param mask The bits to set.
 * @param addr The word they live in.
 */
static inline void ludlc_platform_set_mask(ludlc_platform_atomic_t mask,
					   ludlc_platform_atomic_t *addr)
{
	atomic_or(addr, mask);
}

/**
 * @brief Atomically clears every bit of @p mask.
 * @param mask The bits to clear.
 * @param addr The word they live in.
 */
static inline void ludlc_platform_clear_mask(ludlc_platform_atomic_t mask,
					     ludlc_platform_atomic_t *addr)
{
	atomic_and(addr, ~mask);
}

/**
 * @name Bitsets
 *
 * A bit here is picked at run time, to claim a slot out of a pool, rather
 * than named like the flags elsewhere. The calls above already take a bit
 * number and reach the word it lands in, so only the storage is declared here.
 * @{
 */
/** @brief Declares storage for a bitset of @p nbits bits. */
#define LUDLC_DECLARE_BITSET(name, nbits)	ATOMIC_DEFINE(name, nbits)

/** @} */

/* --- Locks --- */
#define LUDLC_DECLARE_LOCK(name)	struct k_mutex name
#define LUDLC_LOCK_INIT(lk)		k_mutex_init(lk)
#define LUDLC_LOCK(lk)			k_mutex_lock(lk, K_FOREVER)
#define LUDLC_UNLOCK(lk)		k_mutex_unlock(lk)

/* --- Allocation --- */
#if CONFIG_HEAP_MEM_POOL_SIZE > 0

static inline void *ludlc_platform_alloc(size_t sz)
{
	return k_calloc(1, sz);
}

static inline void ludlc_platform_free(void *ptr)
{
	k_free(ptr);
}

#else /* No heap configured: the caller must fall back on static storage. */

static inline void *ludlc_platform_alloc(size_t sz)
{
	(void)sz;

	return NULL;
}

static inline void ludlc_platform_free(void *ptr)
{
	(void)ptr;
}

#endif /* CONFIG_HEAP_MEM_POOL_SIZE */

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
					     const unsigned int delay,
					     const unsigned int period)
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

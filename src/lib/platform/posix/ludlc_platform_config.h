// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file ludlc_config.h
 *
 * @brief LuDLC POSIX platform related definitions
 *
 * This file provides the concrete implementation of the LuDLC platform
 * abstraction layer for POSIX-compliant systems (e.g., Linux).
 * It defines types and macros for locks, timers, atomics, and FIFOs
 * using pthreads, C11 atomics, and standard C libraries.
 *
 * Copyright (C) 2025 Andrey VOLKOV <andrey@volkov.fr> and LuDLC Contributors
 *
 * This file is licensed under either the Apache License, Version 2.0,
 * or the GNU General Public License, version 2 or (at your option)
 * any later version.
 */

#ifndef __LUDLC_CONFIG_H__
#define __LUDLC_CONFIG_H__

#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <endian.h>
#include <stdatomic.h>

#include <ludlc_types.h>
#include <ludlc_autoconf.h>
#include "kfifo.h"

/**
 * @brief Declares a platform-specific lock (maps to pthread_mutex_t).
 * @param name The variable name for the lock.
 */
#define LUDLC_DECLARE_LOCK(name)	pthread_mutex_t name

/**
 * @brief Initializes a platform lock (maps to pthread_mutex_init).
 * @param lk Pointer to the lock object (pthread_mutex_t*).
 */
#define LUDLC_LOCK_INIT(lk)		pthread_mutex_init(lk, NULL)

/**
 * @brief Acquires a platform lock (maps to pthread_mutex_lock).
 * @param lk Pointer to the lock object.
 */
#define LUDLC_LOCK(lk)			pthread_mutex_lock(lk)

/**
 * @brief Releases a platform lock (maps to pthread_mutex_unlock).
 * @param lk Pointer to the lock object.
 */
#define LUDLC_UNLOCK(lk)		pthread_mutex_unlock(lk)

/**
 * @brief Declares a platform-specific wait queue.
 * @details This maps to a structure containing a POSIX condition variable
 * (`pthread_cond_t`) and its associated `pthread_mutex_t`.
 * @param wq The variable name for the wait queue structure.
 */
#define LUDLC_DECLARE_WQ(wq)		\
	struct {			\
		pthread_cond_t  cv;	\
		pthread_mutex_t lock;	\
	} wq

/**
 * @brief Locks the mutex associated with a wait queue.
 * @param wq The wait queue instance.
 */
#define LUDLC_WQ_LOCK(wq)		LUDLC_LOCK(&(wq).lock)

/**
 * @brief Unlocks the mutex associated with a wait queue.
 * @param wq The wait queue instance.
 */
#define LUDLC_WQ_UNLOCK(wq)		LUDLC_UNLOCK(&(wq).lock)

/**
 * @brief Waits on a condition variable until an expression is true.
 * @details Atomically releases the wait queue's lock and blocks
 * until signaled. Re-acquires the lock before returning.
 * Handles spurious wakeups by re-checking the expression.
 *
 * @param wq The wait queue instance.
 * @param expression The C expression to check. Loop continues while
 * this is false.
 */
#define LUDLC_WQ_WAIT_EVENT(wq, expression)	do { \
		while (!(expression)) {				\
			pthread_cond_wait(&(wq.cv), &(wq.lock));	\
		}						\
	} while (0)

/**
 * @brief Wakes up one thread waiting on the condition variable.
 * @details This macro locks the wait queue, executes the provided
 * expression (which should set the state checked by the waiter),
 * signals the condition variable, and unlocks the queue.
 * @param wq The wait queue instance.
 * @param expression An expression to execute while holding the lock
 * (e.g., setting a flag).
 */
#define LUDLC_WQ_WAKEUP(wq, expression)		do {	\
		LUDLC_WQ_LOCK(wq);		\
		(expression);			\
		pthread_cond_signal(&(wq).cv);	\
		LUDLC_WQ_UNLOCK(wq);		\
	} while (0)

/**
 * @brief LuDLC timestamp type for POSIX.
 * @details Defined as uint64_t, typically representing microseconds.
 */
typedef uint64_t 			ludlc_timestamp_t;

/* Values for CRC-16/KERMIT (little-endian version of XMODEM one) */
/** @brief Initial value for the CRC-16/KERMIT checksum. */
#define LUDLC_CSUM_INIT_VALUE		0
/** @brief Expected final value for a valid CRC-16/KERMIT checksum. */
#define LUDLC_CSUM_VERIFY_VALUE		0
/** @brief LuDLC checksum type, defined as uint16_t for CRC-16. */
typedef uint16_t			ludlc_csum_t;

/**
 * @brief Converts checksum to network byte order (which is little-endian
 * for CRC-16/KERMIT).
 * @param csum The host-order checksum.
 * @return The little-endian checksum.
 */
#define LUDLC_CSUM_HTON(csum)		htole16(csum)

/**
 * @brief Declares a kfifo instance.
 * @param fifo The variable name for the kfifo structure.
 */
#define LUDLC_DECLARE_KFIFO(fifo)	struct kfifo fifo

/**
 * @brief Initializes a kfifo instance.
 * @param fifo Pointer to the kfifo structure.
 * @param buf Pointer to the data buffer.
 * @param sz The size of the buffer.
 */
#define LUDLC_KFIFO_INIT(fifo, buf, sz) kfifo_init(fifo, buf, sz)

/** @brief Flag indicating that platform allocation functions are inlined. */
#define LUDLC_PLATFORM_ALLOC_INLINED	1
/**
 * @brief (Inline) Allocates memory using standard `cmalloc`.
 * @param sz Size to allocate.
 * @return Pointer to allocated and zeroed memory, or NULL on failure.
 */
static inline void *ludlc_platform_alloc(size_t sz)
{
	return calloc(sz, 1);
}

/**
 * @brief (Inline) Frees memory using standard `free`.
 * @param ptr Pointer to the memory to free.
 */
static inline void ludlc_platform_free(void *ptr)
{
	free(ptr);
}

/**
 @brief The LuDLC POSIX platform timer structure.
 *
 * This structure wraps a POSIX timer (`timer_t`) and manages its
 * state, including the user callback and synchronization flags.
 */
typedef struct {
	/**< The underlying POSIX timer ID. */
	timer_t posix_timer_id;

	/**< LuDLC's callback function to call. */
	ludlc_timer_cb_t function;
	/**< User data (connection) for the callback. */
	struct ludlc_connection *conn;

	/**< True if the timer is armed (running). */
	atomic_bool pending;
	/**< True if the timer handler is currently executing. */
	atomic_bool handler_running;

} ludlc_platform_timer_t;

/**
 * @brief Platform atomic type, mapping to C11 `atomic_uint`.
 */
typedef atomic_uint ludlc_platform_atomic_t;

/**
 * @brief (Inline) Atomically tests a bit in an atomic variable.
 * @param nr The bit number (0-based) to test.
 * @param addr Pointer to the atomic variable.
 * @return true if the bit was set, false otherwise.
 */
static inline bool ludlc_platform_test_bit(long nr,
		ludlc_platform_atomic_t *addr)
{
	return (atomic_load(addr) & (1U << nr)) != 0;
}

/**
 * @brief (Inline) Atomically sets a bit in an atomic variable.
 * @param nr The bit number (0-based) to set.
 * @param addr Pointer to the atomic variable.
 */
static inline void ludlc_platform_set_bit(long nr,
		ludlc_platform_atomic_t *addr)
{
	atomic_fetch_or(addr, 1U << nr);
}

/**
 * @brief (Inline) Atomically clears a bit in an atomic variable.
 * @param nr The bit number (0-based) to clear.
 * @param addr Pointer to the atomic variable.
 */
static inline void ludlc_platform_clear_bit(long nr,
		ludlc_platform_atomic_t *addr)
{
	atomic_fetch_and(addr, ~(1U << nr));
}

/**
 * @brief (Inline) Atomically tests and clears a bit.
 * @param nr The bit number (0-based) to test and clear.
 * @param addr Pointer to the atomic variable.
 * @return true if the bit was set before being cleared, false otherwise.
 */
static inline bool ludlc_platform_test_and_clear_bit(long nr,
		ludlc_platform_atomic_t *addr)
{
	uint prev_val = atomic_fetch_and(addr, ~(1U << nr));
	return !!(prev_val & (1U << nr));
}

/**
 * @brief (Inline) Atomically tests and sets a bit.
 * @param nr The bit number (0-based) to test and set.
 * @param addr Pointer to the atomic variable.
 * @return true if the bit was set before the operation, false otherwise.
 */
static inline bool ludlc_platform_test_and_set_bit(long nr,
		ludlc_platform_atomic_t *addr)
{
	uint prev_val = atomic_fetch_or(addr, 1U << nr);
	return !!(prev_val & (1U << nr));
}

/* Include platform-specific hardware definitions (e.g., serial args) */
#include "ludlc_hardware.h"

#endif /*__LUDLC_CONFIG_H__*/

// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file posix/ludlc_platform.h
 *
 * @brief LuDLC POSIX platform related definitions.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * The LuDLC platform abstraction layer for POSIX systems: types and macros for
 * locks, timers, atomics and FIFOs, built on pthreads, C11 atomics and the
 * standard C library. Checksums are not here; they belong to the transport.
 */

#ifndef __LUDLC_CONFIG_H__
#define __LUDLC_CONFIG_H__

#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>
#include <limits.h>

#include <ludlc_types.h>
#include <ludlc_autoconf.h>

/** Round @a n up to a multiple of @a m (unsigned arithmetic, @a m > 0). */
#define LUDLC_ROUND_UP(n, m) \
	((((n) + (m)-1U) / (m)) * (m))

#ifndef LUDLC_MIN
#define LUDLC_MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/* kfifo.h uses LUDLC_ROUND_UP and LUDLC_MIN, so it is included after them. */
#include "kfifo.h"

#define LUDLC_CONTAINER_OF(ptr, type, member) \
	((type *)((uint8_t *)(ptr) - offsetof(type, member)))

/** @brief Bits in one @ref ludlc_platform_atomic_t. */
#define LUDLC_BITS_PER_WORD	(sizeof(ludlc_platform_atomic_t) * CHAR_BIT)

/** @brief Words of ludlc_platform_atomic_t needed to hold @p n bits. */
#define LUDLC_BITS_TO_WORDS(n)						\
	(((n) + LUDLC_BITS_PER_WORD - 1) / LUDLC_BITS_PER_WORD)


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

/* ludlc_timestamp_t comes from ludlc_types.h. */

/**
 * @brief Platform atomic type, mapping to C11 `atomic_uint`.
 */
typedef atomic_uint ludlc_platform_atomic_t;

struct ludlc_platform_connection {
#define LUDLC_CONN_FORCE_TX_F		0
#define LUDLC_CONN_TX_EXIT_EVT		1
#define LUDLC_PLATFORM_LAST_TX_EVENT	2
	ludlc_platform_atomic_t tx_events;

	int tx_pipe[2];

	/**
	 * @brief Pipe FDs used to unblock the RX thread.
	 * `rx_pipe[0]` is the read end (polled by RX), `rx_pipe[1]` the write
	 * end (written by destroy and by the watchdog).
	 */
	 int rx_pipe[2];
};

/**
 * @brief Declares a kfifo instance.
 * @param fifo The variable name for the kfifo structure.
 */
#define LUDLC_DECLARE_RING_BUF(fifo)	struct kfifo fifo

/**
 * @brief Initializes a kfifo instance.
 * @param fifo Pointer to the kfifo structure.
 * @param buf Pointer to the data buffer.
 * @param sz The size of the buffer.
 */
#define LUDLC_RING_BUF_INIT(fifo, buf, sz) kfifo_init(fifo, buf, sz)

/**
 * @brief Allocates memory using standard `calloc`.
 * @param sz Size to allocate.
 * @return Pointer to allocated and zeroed memory, or NULL on failure.
 */
static inline void *ludlc_platform_alloc(size_t sz)
{
	return calloc(1, sz);
}

/**
 * @brief Frees memory using standard `free`.
 * @param ptr Pointer to the memory to free.
 */
static inline void ludlc_platform_free(void *ptr)
{
	free(ptr);
}

/* --- Timers --- */
typedef struct ludlc_connection *ludlc_platform_timer_arg_t;

/**
 * @typedef ludlc_timer_cb_t
 * @brief Callback function type for platform timers.
 *
 * @param arg Platform timer argument provided by the timer backend.
 */
typedef void (*ludlc_timer_cb_t)(ludlc_platform_timer_arg_t arg);

/**
 * @brief The LuDLC POSIX platform timer structure.
 *
 * Wraps a POSIX timer (`timer_t`) with the callback and the flags that track
 * its state.
 */
typedef struct {
	timer_t posix_timer_id;

	ludlc_timer_cb_t function;
	/** User data (connection) for the callback. */
	struct ludlc_connection *conn;

	/** True if the timer is armed (running). */
	atomic_bool pending;
	/** True if the timer was given a period, so it re-arms itself. */
	atomic_bool periodic;
	/** True if the timer handler is currently executing. */
	atomic_bool handler_running;

} ludlc_platform_timer_t;

static inline struct ludlc_connection *ludlc_timer_arg_to_conn(
		ludlc_platform_timer_arg_t arg)
{
	return arg;
}

/*
 * Each of these picks the word the bit lands in, so that a single flag word
 * and a whole bitmap are addressed the same way, as they are on Linux and
 * Zephyr. C11 has no bitmap helpers of its own to use instead.
 */
#define LUDLC_ATOMIC_WORD(addr, nr)	(&(addr)[(nr) / LUDLC_BITS_PER_WORD])
#define LUDLC_ATOMIC_MASK(nr)		(1U << ((nr) % LUDLC_BITS_PER_WORD))

/**
 * @brief Atomically tests a bit.
 * @param nr The bit number (0-based) to test.
 * @param addr The word, or the first word of a bitmap.
 * @return true if the bit was set, false otherwise.
 */
static inline bool ludlc_platform_test_bit(long nr,
		ludlc_platform_atomic_t *addr)
{
	return (atomic_load(LUDLC_ATOMIC_WORD(addr, nr)) &
		LUDLC_ATOMIC_MASK(nr)) != 0;
}

/**
 * @brief Atomically sets a bit.
 * @param nr The bit number (0-based) to set.
 * @param addr The word, or the first word of a bitmap.
 */
static inline void ludlc_platform_set_bit(long nr,
		ludlc_platform_atomic_t *addr)
{
	atomic_fetch_or(LUDLC_ATOMIC_WORD(addr, nr), LUDLC_ATOMIC_MASK(nr));
}

/**
 * @brief Atomically clears a bit.
 * @param nr The bit number (0-based) to clear.
 * @param addr The word, or the first word of a bitmap.
 */
static inline void ludlc_platform_clear_bit(long nr,
		ludlc_platform_atomic_t *addr)
{
	atomic_fetch_and(LUDLC_ATOMIC_WORD(addr, nr), ~LUDLC_ATOMIC_MASK(nr));
}

/**
 * @brief Atomically tests and clears a bit.
 * @param nr The bit number (0-based) to test and clear.
 * @param addr The word, or the first word of a bitmap.
 * @return true if the bit was set before being cleared, false otherwise.
 */
static inline bool ludlc_platform_test_and_clear_bit(long nr,
		ludlc_platform_atomic_t *addr)
{
	unsigned int prev = atomic_fetch_and(LUDLC_ATOMIC_WORD(addr, nr),
					     ~LUDLC_ATOMIC_MASK(nr));

	return (prev & LUDLC_ATOMIC_MASK(nr)) != 0;
}

/**
 * @brief Atomically tests and sets a bit.
 * @param nr The bit number (0-based) to test and set.
 * @param addr The word, or the first word of a bitmap.
 * @return true if the bit was set before the operation, false otherwise.
 */
static inline bool ludlc_platform_test_and_set_bit(long nr,
		ludlc_platform_atomic_t *addr)
{
	unsigned int prev = atomic_fetch_or(LUDLC_ATOMIC_WORD(addr, nr),
					    LUDLC_ATOMIC_MASK(nr));

	return (prev & LUDLC_ATOMIC_MASK(nr)) != 0;
}

/**
 * @brief Atomically sets every bit of @p mask.
 * @param mask The bits to set.
 * @param addr The word they live in.
 */
static inline void ludlc_platform_set_mask(ludlc_platform_atomic_t mask,
					   ludlc_platform_atomic_t *addr)
{
	atomic_fetch_or(addr, mask);
}

/**
 * @brief Atomically clears every bit of @p mask.
 * @param mask The bits to clear.
 * @param addr The word they live in.
 */
static inline void ludlc_platform_clear_mask(ludlc_platform_atomic_t mask,
					     ludlc_platform_atomic_t *addr)
{
	atomic_fetch_and(addr, ~mask);
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
#define LUDLC_DECLARE_BITSET(name, nbits)				\
	ludlc_platform_atomic_t name[LUDLC_BITS_TO_WORDS(nbits)]

/** @} */

#endif /*__LUDLC_CONFIG_H__*/

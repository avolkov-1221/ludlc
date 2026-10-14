/* SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only) */
/**
 * @file ludlc_platform_api.h
 *
 * @brief LuDLC platform abstraction layer API.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
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
 */

#ifndef __LUDLC_PLATFORM_API_H__
#define __LUDLC_PLATFORM_API_H__

#include <ludlc_types.h>
#include <ludlc_platform.h>

/**
 * @def LUDLC_TIMER_US_T
 * @brief Integer type a timer delay is passed to the platform in.
 *
 * Microseconds, and an @c unsigned int unless a port overrides it. A target
 * whose @c int cannot hold the delays the core computes - the watchdog is
 * three keepalive periods, so a one-second ping needs three million - defines
 * this in its @c ludlc_platform.h before this header, widening the two
 * parameters below for that port only.
 */
#ifndef LUDLC_TIMER_US_T
#define LUDLC_TIMER_US_T	unsigned int
#endif

/**
 * @typedef ludlc_timer_us_t
 * @brief @ref LUDLC_TIMER_US_T, as a type.
 */
typedef LUDLC_TIMER_US_T ludlc_timer_us_t;

/**
 * @name Atomic bit operations
 *
 * @c ludlc_platform_test_and_set_bit() and
 * @c ludlc_platform_test_and_clear_bit() must return the previous value
 * without being split into a test and a change, since a bit set in between
 * would be dropped; the other three need no more than they say. None has to
 * order surrounding accesses, because the core touches every shared flag
 * either under @c conn->lock or with one of these. A target with no
 * instruction for the pair holds interrupts off across it instead.
 *
 * @c ludlc_platform_set_mask() and @c ludlc_platform_clear_mask() do the same
 * for several bits of one word at once, and are how a caller changes a group
 * of flags without reading the word and writing it back, which would drop a
 * bit an interrupt set in between. Every target has an atomic mask of its
 * own, so no port works them out of the single bit calls.
 *
 * The same four reach a bitset, where the bit is a slot number rather than a
 * name, so a port takes a bit number that may be worked out while running and
 * finds the word it lands in. What a port declares separately is the storage,
 * with @c LUDLC_DECLARE_BITSET(), and the count below if the generic one
 * would cost it a call.
 * @{
 */
#ifndef ludlc_platform_ffz
/**
 * @brief The first bit of @p word which is not set.
 *
 * @param word One word of a bitset.
 * @return Its number, or @ref LUDLC_BITS_PER_WORD when the word is full.
 */
static inline unsigned int ludlc_platform_ffz(ludlc_platform_atomic_t word)
{
	const ludlc_platform_atomic_t free = ~word;

	if (!free) {
		return LUDLC_BITS_PER_WORD;
	}

	/* Whichever count is as wide as the word, so that a target narrower
	 * than long keeps clear of the 64-bit helper. The other arm folds
	 * away. A port whose count is worse than its own instruction, as one
	 * without a count at all is, says so and replaces this.
	 */
	if (sizeof(free) <= sizeof(unsigned int)) {
		return (unsigned int)__builtin_ctz((unsigned int)free);
	}

	return (unsigned int)__builtin_ctzll((unsigned long long)free);
}
#endif /* ludlc_platform_ffz */

/**
 * @brief Claims the first free bit of @p set.
 * @param set Bitset declared with @ref LUDLC_DECLARE_BITSET.
 * @param nbits Its width, the same one it was declared with.
 * @return The claimed bit, or -1 when they are all taken.
 */
static inline int ludlc_bitset_claim(ludlc_platform_atomic_t *set,
				     unsigned int nbits)
{
	const unsigned int words = LUDLC_BITS_TO_WORDS(nbits);
	unsigned int w;

	for (w = 0; w < words; w++) {
		unsigned int bit;

		/* The search only reads, so another caller may take the bit
		 * first. Look again, which starts past the one just lost.
		 */
		while ((bit = ludlc_platform_ffz(set[w])) <
		       LUDLC_BITS_PER_WORD) {
			const unsigned int nr = w * LUDLC_BITS_PER_WORD + bit;

			if (nr >= nbits) {
				return -1;
			}

			if (!ludlc_platform_test_and_set_bit(nr, set)) {
				return (int)nr;
			}
		}
	}

	return -1;
}
/** @} */

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
 * @note The delay type is the port's; see @ref LUDLC_TIMER_US_T.
 */
int ludlc_platform_start_timer(ludlc_platform_timer_t *timer,
				const ludlc_timer_us_t delay,
				const ludlc_timer_us_t period);

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
 * Marks the connection as having something to send and, on threaded
 * transports, unblocks the TX worker. Used for a handshake, data or an
 * ACK/PING, and to run the transmitter with no new data for a keepalive.
 *
 * @param[in,out] conn Active connection.
 *
 * @note Callable from timer callbacks and other threads. A port that calls it
 *       from a signal handler must make its own implementation
 *       async-signal-safe.
 */
void ludlc_platform_request_tx(struct ludlc_connection *conn);

/**
 * @brief Hook for link watchdog expiry (optional transport wake-up).
 *
 * Called from the watchdog timer path when the peer has been silent too long.
 * The platform may unblock a blocking read or poll, or schedule work, so that
 * the transport notices and takes the link down.
 *
 * @param[in,out] conn Connection whose watchdog fired.
 *
 * @note Not all ports need this; a stub is valid where the timeout is
 *       noticed elsewhere.
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

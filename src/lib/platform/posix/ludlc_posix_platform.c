// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file ludlc_posix.c
 *
 * @brief LuDLC POSIX platform-specific implementations.
 *
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * This file is licensed under either the Apache License, Version 2.0,
 * or the GNU General Public License, version 2 or (at your option)
 * any later version.
 */

#include <unistd.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sched.h>
#include <signal.h>
#include <time.h>

#include <ludlc.h>
#include <ludlc_private.h>

/**
 * @brief Internal "trampoline" handler for ALL timer expirations.
 *
 * This is the single function registered with `timer_create` via
 * `SIGEV_THREAD`. It receives the pointer to our `ludlc_platform_timer_t`
 * struct and safely calls the user-specified LuDLC callback.
 *
 * It uses atomic flags to manage race conditions between
 * `ludlc_platform_stop_timer` and the timer's own expiration,
 * ensuring the callback is not executed if the timer was stopped.
 *
 * @param sv The `sigval` union containing the pointer (`sival_ptr`) to
 * the `ludlc_platform_timer_t` structure.
 */
static void posix_timer_handler(union sigval sv)
{
	ludlc_platform_timer_t *timer = (ludlc_platform_timer_t*)sv.sival_ptr;

	if (timer == NULL)
		return;

	/* Mark that the handler is executing to prevent deletion */
	atomic_exchange(&timer->handler_running, true);

	/*
	 * Atomically check and clear the 'pending' flag.
	 * If the flag was 'true' (meaning the timer expired *before*
	 * a stop was called), we execute the callback.
	 * If 'false' (stop was called first), we do nothing.
	 */
	if (atomic_exchange(&timer->pending, false) && timer->function)
		timer->function(timer->conn);

	/* Mark that the handler has finished */
	atomic_exchange(&timer->handler_running, false);
}

/**
 * @brief Initializes a LuDLC platform timer for POSIX.
 *
 * This function creates an underlying POSIX timer (`timer_t`) associated
 * with the `CLOCK_MONOTONIC` clock. It configures the timer to
 * notify via a new thread (`SIGEV_THREAD`), which will execute
 * the `posix_timer_handler` trampoline.
 *
 * @param conn Pointer to the LuDLC connection, passed to the callback.
 * @param timer Pointer to the `ludlc_platform_timer_t` structure to initialize.
 * @param cb The LuDLC timer callback function to be called on expiration.
 * @return 0 on success.
 * @return -EINVAL if `timer` is NULL.
 * @return A negative error code from `timer_create` on failure.
 */
int ludlc_platform_init_timer(struct ludlc_connection *conn,
				ludlc_platform_timer_t *timer,
				ludlc_timer_cb_t cb)
{
	int ret;
	struct sigevent sev = {0};

	if (!timer)
		return -EINVAL;

	timer->function = cb;
	timer->conn = conn;
	timer->pending = false;
	timer->handler_running = false;

	sev.sigev_notify = SIGEV_THREAD;
	sev.sigev_notify_function = posix_timer_handler;
	sev.sigev_value.sival_ptr = timer; /* Pass our struct to the handler */
	sev.sigev_notify_attributes = NULL;

	/* Create the underlying POSIX timer. */
	/* We use CLOCK_MONOTONIC, as it's not affected by system time changes. */
	if (timer_create(CLOCK_MONOTONIC, &sev, &timer->posix_timer_id))
		return -errno;

	return 0;
}

/**
 * @brief Implements @c ludlc_proto_cb::get_timestamp (POSIX).
 *
 * Uses @c CLOCK_MONOTONIC; writes microseconds to @a out_ts.
 *
 * @param[out] out_ts Receives elapsed time in microseconds.
 * @return 0 on success, \c -EINVAL if @a out_ts is NULL, or \c -errno
 * from @c clock_gettime.
 */
int ludlc_default_get_timestamp(ludlc_timestamp_t *out_ts)
{
	struct timespec ts;

	if (!out_ts) {
		return -EINVAL;
	}

	if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
		return -errno;
	}

	*out_ts = ts.tv_sec * 1000000U + (ludlc_timestamp_t)(ts.tv_nsec / 1000);
	return 0;
}

/**
 * @brief Starts or restarts a LuDLC platform timer.
 *
 * This function arms (or re-arms) the specified POSIX timer with a
 * given delay and period.
 *
 * @param timer Pointer to the initialized `ludlc_platform_timer_t` structure.
 * @param delay The initial delay before the first expiration, in microseconds.
 * @param period The period for subsequent expirations, in microseconds.
 * If 0, the timer is a one-shot timer.
 * @return 0 on success.
 * @return -EINVAL if `timer` is NULL.
 * @return A negative error code from `timer_settime` on failure.
 */
int ludlc_platform_start_timer(ludlc_platform_timer_t *timer,
				ludlc_timestamp_t delay,
				ludlc_timestamp_t period)
{
	struct itimerspec its = {0};

	if (!timer)
		return -EINVAL;

	/* Convert microseconds to a relative timespec */
	its.it_value.tv_sec = delay / 1000000ULL;
	its.it_value.tv_nsec = (delay % 1000000ULL) * 1000ULL;

	its.it_interval.tv_sec = period / 1000000ULL;
	its.it_interval.tv_nsec = (period % 1000000ULL) * 1000ULL;

	/* Set pending flag *before* arming the timer to avoid a race. */
	timer->pending = true;

	/* Arm the timer (flags = 0 means relative time) */
	if (timer_settime(timer->posix_timer_id, 0, &its, NULL) == -1) {
		/* Rollback: failed to arm */
		timer->pending = false;
		return -errno;
	}

	return 0;
}

/**
 * @brief Stops a running LuDLC platform timer.
 *
 * This function disarms the underlying POSIX timer and clears the
 * `pending` flag to prevent any in-flight expirations from
 * executing the callback.
 *
 * @note This function handles a race condition where the timer might
 * expire and the handler might be scheduled just as this stop function
 * is called. It atomically clears the `pending` flag, which the
 * `posix_timer_handler` checks before executing the user callback.
 *
 * @param timer Pointer to the `ludlc_platform_timer_t` structure to stop.
 * @return 0 on success.
 * @return A negative error code from `timer_settime` if disarming fails.
 */
int ludlc_platform_stop_timer(ludlc_platform_timer_t *timer)
{
	if (timer == NULL)
		return 0;

	/*
	 * Atomically clear the pending flag.
	 * This signals to the timer_handler (if it's running or
	 * about to run) that it should *not* execute the callback.
	 */
	timer->pending = false;

	/* Disarm the underlying POSIX timer to prevent future triggers. */
	struct itimerspec its = {0}; /* {0, 0} disarms the timer */

	if (timer_settime(timer->posix_timer_id, 0, &its, NULL) == -1) {
		/* This is not fatal, but we should report it. */
		/* The handler logic will still prevent the callback from */
		/* running thanks to the 'was_pending' check. */
		return -errno;
	}

	/*
	 * Wait if the handler is currently executing.
	 * This provides a stronger guarantee that the callback is not
	 * running on another thread when this function returns.
	 */
	while (timer->handler_running) {
		(void)sched_yield();
	}

	return 0;
}

/**
 * @brief Delete a LuDLC platform timer.
 *
 * This function disarms the underlying POSIX timer and clears the
 * `pending` flag to prevent any in-flight expirations from
 * executing the callback.
 *
 * @param timer Pointer to the `ludlc_platform_timer_t` structure to delete.
 */
void ludlc_platform_destroy_timer(ludlc_platform_timer_t *timer)
{
	if (timer) {
		ludlc_platform_stop_timer(timer);

		timer_delete(timer->posix_timer_id);
	}
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
	/* Set the force TX flag */
	ludlc_platform_set_bit(LUDLC_CONN_FORCE_TX_F, &conn->pconn.tx_events);

	/* Wake up the TX thread by writing to the pipe */
	char msg = 'T';
	if (write(conn->pconn.tx_pipe[1], &msg, 1) == -1) {
		/* Handle error, e.g., pipe closed or full.
		 * It's not a critical error if the pipe is full (EAGAIN),
		 * as the FORCE_TX flag is already set. Only log other errors. */
		if (errno != EAGAIN) {
			LUDLC_LOG_ERROR("Failed to write to TX wakeup pipe: %s",
				strerror(errno));
		}
	}
}

void ludlc_platform_conn_timeout(struct ludlc_connection *conn)
{
	char msg = 'T';
	if (write(conn->pconn.rx_pipe[1], &msg, 1) == -1) {
		/* Handle error, e.g., pipe closed or full.
		 * It's not a critical error if the pipe is full (EAGAIN),
		 * as the FORCE_TX flag is already set. Only log other errors. */
		if (errno != EAGAIN) {
			LUDLC_LOG_ERROR("Failed to write to TX wakeup pipe: %s",
				strerror(errno));
		}
	}
}

void ludlc_platform_conn_destroy(struct ludlc_connection *conn)
{
	if (conn->pconn.tx_pipe[0] >= 0) {
		close(conn->pconn.tx_pipe[0]);
		conn->pconn.tx_pipe[0] = -1;
	}

	if (conn->pconn.tx_pipe[1] >= 0) {
		close(conn->pconn.tx_pipe[1]);
		conn->pconn.tx_pipe[1] = -1;
	}

	if (conn->pconn.rx_pipe[0] >= 0) {
		close(conn->pconn.rx_pipe[0]);
		conn->pconn.rx_pipe[0] = -1;
	}

	if (conn->pconn.rx_pipe[1] >= 0) {
		close(conn->pconn.rx_pipe[1]);
		conn->pconn.rx_pipe[1] = -1;
	}

}

int ludlc_platform_conn_init(struct ludlc_connection *conn)
{
	conn->pconn.rx_pipe[0] = -1;
	conn->pconn.rx_pipe[1] = -1;

	/* Create rx pipe */
	if (pipe(conn->pconn.rx_pipe) < 0) {
		return -errno;
	}

	/* Set read end of pipe to non-blocking */
	fcntl(conn->pconn.rx_pipe[0], F_SETFL, O_NONBLOCK);

	conn->pconn.tx_pipe[0] = -1;
	conn->pconn.tx_pipe[1] = -1;

	if (pipe(conn->pconn.tx_pipe) < 0) {
		int ret = -errno;

		close(conn->pconn.rx_pipe[0]);
		conn->pconn.rx_pipe[0] = -1;

		close(conn->pconn.rx_pipe[1]);
		conn->pconn.rx_pipe[1] = -1;
		return ret;
	}

	fcntl(conn->pconn.tx_pipe[0], F_SETFL, O_NONBLOCK);
	return 0;
}

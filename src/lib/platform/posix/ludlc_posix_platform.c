// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only)
/**
 * @file ludlc_posix_platform.c
 *
 * @brief LuDLC POSIX platform-specific implementations.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * The platform services the core calls on a POSIX system: its timers, run
 * as POSIX timers that notify a thread, the monotonic timestamp, and the two
 * pipes of a connection, which wake its TX thread for a send and its RX
 * thread when the link times out.
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
 * @brief Internal trampoline handler for all timer expirations.
 *
 * Registered with `timer_create` via `SIGEV_THREAD`. Atomic flags keep it
 * from racing with `ludlc_platform_stop_timer()`, so the callback does not
 * run if the timer was stopped.
 *
 * @param sv The `sigval` union carrying the `ludlc_platform_timer_t` pointer.
 */
static void posix_timer_handler(union sigval sv)
{
	ludlc_platform_timer_t *timer = (ludlc_platform_timer_t*)sv.sival_ptr;

	if (timer == NULL)
		return;

	/* Marks the handler as executing, so a stop or delete waits for it. */
	atomic_exchange(&timer->handler_running, true);

	/*
	 * A stop clears the flag, so finding it clear means the callback must
	 * not run. A periodic timer keeps it set, as it stays armed for its
	 * next expiry.
	 */
	if (atomic_exchange(&timer->pending, atomic_load(&timer->periodic)) &&
	    timer->function)
		timer->function(timer->conn);

	atomic_exchange(&timer->handler_running, false);
}

/**
 * @brief Initializes a LuDLC platform timer for POSIX.
 *
 * Creates a `CLOCK_MONOTONIC` POSIX timer that notifies through a new thread
 * (`SIGEV_THREAD`) running the `posix_timer_handler` trampoline.
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
	timer->periodic = false;
	timer->handler_running = false;

	sev.sigev_notify = SIGEV_THREAD;
	sev.sigev_notify_function = posix_timer_handler;
	sev.sigev_value.sival_ptr = timer; /* Pass our struct to the handler */
	sev.sigev_notify_attributes = NULL;

	/* CLOCK_MONOTONIC, so system time changes do not affect the timer. */
	if (timer_create(CLOCK_MONOTONIC, &sev, &timer->posix_timer_id))
		return -errno;

	return 0;
}

/**
 * @brief Default @ref ludlc_get_timestamp_t for POSIX.
 *
 * Uses @c CLOCK_MONOTONIC_RAW; returns nanoseconds. Any monotonic unit with
 * enough resolution to tell sequential packets apart is valid for a custom
 * callback.
 *
 * @param[out] out_ts Receives elapsed monotonic time in nanoseconds.
 * @return 0 on success, \c -EINVAL if @a out_ts is NULL, or \c -errno
 * from @c clock_gettime.
 */
int ludlc_default_get_timestamp(ludlc_timestamp_t *out_ts)
{
	struct timespec ts;

	if (!out_ts) {
		return -EINVAL;
	}

	if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) == -1) {
		return -errno;
	}

	*out_ts = (ludlc_timestamp_t)
			((uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec);
	return 0;
}

/**
 * @brief Starts or restarts a LuDLC platform timer.
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
				const unsigned int delay,
				const unsigned int period)
{
	struct itimerspec its = {0};

	if (!timer)
		return -EINVAL;

	its.it_value.tv_sec = delay / 1000000ULL;
	its.it_value.tv_nsec = (delay % 1000000ULL) * 1000ULL;

	its.it_interval.tv_sec = period / 1000000ULL;
	its.it_interval.tv_nsec = (period % 1000000ULL) * 1000ULL;

	/* Set both flags *before* arming the timer to avoid a race. */
	atomic_store(&timer->periodic, period != 0);
	timer->pending = true;

	/* flags = 0: the timeout is relative. */
	if (timer_settime(timer->posix_timer_id, 0, &its, NULL) == -1) {
		timer->pending = false;
		return -errno;
	}

	return 0;
}

/**
 * @brief Stops a running LuDLC platform timer.
 *
 * Disarms the POSIX timer and clears the `pending` flag so that an expiry
 * already in flight does not run the callback.
 *
 * @note Clearing `pending` atomically is what closes the race with an expiry
 * scheduled just as this is called: `posix_timer_handler` checks the flag
 * before calling the user callback.
 *
 * @param timer Pointer to the `ludlc_platform_timer_t` structure to stop.
 * @return 0 on success.
 * @return A negative error code from `timer_settime` if disarming fails.
 */
int ludlc_platform_stop_timer(ludlc_platform_timer_t *timer)
{
	if (timer == NULL)
		return 0;

	/* Tells the handler, running or about to run, not to call back. */
	timer->pending = false;

	struct itimerspec its = {0}; /* {0, 0} disarms the timer */

	if (timer_settime(timer->posix_timer_id, 0, &its, NULL) == -1) {
		/*
		 * Not fatal: the cleared pending flag still stops the
		 * callback.
		 */
		return -errno;
	}

	/*
	 * Wait out a handler already running, so the callback is not on
	 * another thread when this returns.
	 */
	while (timer->handler_running) {
		(void)sched_yield();
	}

	return 0;
}

/**
 * @brief Delete a LuDLC platform timer.
 *
 * Stops the timer first, so an expiry already in flight cannot run the
 * callback.
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
 * Wakes the TX thread and forces a send even when no new data has been
 * queued, for example to carry a PING with an ACK.
 *
 * @param conn Pointer to the connection structure.
 */
void ludlc_platform_request_tx(struct ludlc_connection *conn)
{
	ludlc_platform_set_bit(LUDLC_CONN_FORCE_TX_F, &conn->pconn.tx_events);

	char msg = 'T';
	if (write(conn->pconn.tx_pipe[1], &msg, 1) == -1) {
		/*
		 * A full pipe (EAGAIN) is harmless: the FORCE_TX flag is
		 * already set. Log anything else.
		 */
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
		/*
		 * A full pipe (EAGAIN) is harmless: the RX thread wakes on
		 * what is already queued. Log anything else.
		 */
		if (errno != EAGAIN) {
			LUDLC_LOG_ERROR("Failed to write to TX wakeup pipe: %s",
				strerror(errno));
		}
	}
}

/* The transport knows its threads: ask it. */
bool ludlc_in_callback(struct ludlc_connection *conn)
{
	return conn && conn->pconn.in_callback && conn->pconn.in_callback(conn);
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

/*
 * Neither end may block. The reader polls both pipes, and a write finding one
 * full is not needed, a wakeup being queued already.
 */
static int ludlc_pipe_set_nonblock(int fd[2])
{
	int i;

	for (i = 0; i < 2; i++) {
		int flags = fcntl(fd[i], F_GETFL);

		if (flags < 0 ||
		    fcntl(fd[i], F_SETFL, flags | O_NONBLOCK) < 0) {
			return -errno;
		}
	}

	return 0;
}

int ludlc_platform_conn_init(struct ludlc_connection *conn)
{
	int ret;

	conn->pconn.rx_pipe[0] = -1;
	conn->pconn.rx_pipe[1] = -1;
	conn->pconn.tx_pipe[0] = -1;
	conn->pconn.tx_pipe[1] = -1;

	if (pipe(conn->pconn.rx_pipe) < 0) {
		return -errno;
	}

	ret = ludlc_pipe_set_nonblock(conn->pconn.rx_pipe);
	if (ret) {
		goto err_destroy;
	}

	if (pipe(conn->pconn.tx_pipe) < 0) {
		ret = -errno;
		goto err_destroy;
	}

	ret = ludlc_pipe_set_nonblock(conn->pconn.tx_pipe);
	if (ret) {
		goto err_destroy;
	}

	return 0;

err_destroy:
	ludlc_platform_conn_destroy(conn);

	return ret;
}

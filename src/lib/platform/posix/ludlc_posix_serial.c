// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only)
/**
 * @file ludlc_posix_serial.c
 *
 * @brief LuDLC POSIX serial (UART) transport implementation.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * The serial transport on a POSIX system:
 * - opens and configures the TTY (UART) device;
 * - runs the RX and TX threads that feed the framing;
 * - implements `ludlc_serial_connection_create()` and
 *   `ludlc_serial_connection_destroy()`.
 *
 * The checksum the framing runs comes from the caller in
 * `ludlc_platform_args_t`; this file provides only the default one.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>

#include "ludlc_posix.h"
#include <ludlc.h>
#include <ludlc_private.h>
#include <ludlc_serial.h>
#include <ludlc_framing.h>

/** @brief Flag: TX thread started successfully. */
#define TX_THREAD_OK	(1<<0)
/** @brief Flag: RX thread started successfully. */
#define RX_THREAD_OK	(1<<1)

/** @brief How long the shutdown waits for the DISCONNECT to go out, in ms. */
#define LUDLC_DISCONNECT_WAIT_MS	50U

/**
 * @struct ludlc_serial_connection
 * @brief POSIX serial-specific connection structure.
 *
 * Wraps the core `ludlc_connection` with the thread IDs and the UART file
 * descriptor.
 */
struct ludlc_serial_connection {
	/** Internal state flags (TX_THREAD_OK, RX_THREAD_OK). */
	unsigned int flags;
	pthread_t rx_thread;
	pthread_t tx_thread;
	int uart_fd;

	LUDLC_DECLARE_RING_BUF(tx_fifo);

	struct ludlc_connection conn;

	/** Framing state of this link, allocated at open. The RX and TX
	 * threads drive the decoder and the encoder.
	 */
	struct ludlc_framing_state *fs;

	/** Checksum this link runs. Borrowed from the caller's args. */
	const struct ludlc_csum *csum;

	/** Storage behind @c tx_fifo, which the encoder writes into directly.
	 * Sized independently of the frame.
	 */
	uint8_t tx_buf[LUDLC_TX_FIFO_SIZE];
};

#ifdef LUDLC_STATIC_CONN
static struct ludlc_serial_connection g_conn[LUDLC_STATIC_CONN_NUM];
static LUDLC_DECLARE_BITSET(g_conn_busy, LUDLC_STATIC_CONN_NUM);

/**
 * @brief Takes a serial connection from the static pool.
 * @return Pointer to a free connection, or NULL if the pool is full.
 */
static inline struct ludlc_serial_connection *alloc_serial_connection(void)
{
	const int slot = ludlc_bitset_claim(g_conn_busy, LUDLC_STATIC_CONN_NUM);

	if (slot < 0) {
		return NULL;
	}

	/* A reused slot still holds the last link's state, which the heap
	 * path never sees.
	 */
	memset(&g_conn[slot], 0, sizeof(g_conn[slot]));

	return &g_conn[slot];
}

/**
 * @brief Returns a serial connection to the static pool.
 * @param sconn The connection to release (must be from @ref g_conn).
 */
static inline void free_serial_connection(struct ludlc_serial_connection *sconn)
{
	ludlc_platform_clear_bit(sconn - g_conn, g_conn_busy);
}
#else
/**
 * @brief Allocates a new serial connection using platform's malloc.
 * @return Pointer to the allocated connection, or NULL on failure.
 */
static inline struct ludlc_serial_connection *alloc_serial_connection(void)
{
	return ludlc_platform_alloc(sizeof(struct ludlc_serial_connection));
}

/**
 * @brief Frees a serial connection using platform's free.
 * @param conn The connection structure to free.
 */
static inline void free_serial_connection(struct ludlc_serial_connection *conn)
{
	ludlc_platform_free(conn);
}
#endif /* LUDLC_STATIC_CONN */

/**
 * @def LUDLC_BAUD_UNSUPPORTED
 * @brief What @ref baud2speed returns for a rate this platform lacks.
 *
 * @c speed_t is unsigned, so the sentinel is named rather than compared
 * against a bare -1.
 */
#define LUDLC_BAUD_UNSUPPORTED	((speed_t)-1)

/**
 * @brief Converts a numeric baud rate to a POSIX `speed_t` constant.
 * (Borrowed from minicom).
 *
 * @param baud The numeric baud rate (e.g., 9600, 115200).
 * @return The corresponding `speed_t` constant (e.g., `B9600`), or
 * @ref LUDLC_BAUD_UNSUPPORTED if this platform has no constant for it.
 */
static speed_t baud2speed(unsigned long baud)
{
	speed_t spd = LUDLC_BAUD_UNSUPPORTED;

	switch (baud / 100) {
	case 0:
#ifdef B0
		spd = B0;
#else
		spd = 0;
#endif
		break;
	case 3:
		spd = B300;
		break;
	case 6:
		spd = B600;
		break;
	case 12:
		spd = B1200;
		break;
	case 24:
		spd = B2400;
		break;
	case 48:
		spd = B4800;
		break;
	case 96:
		spd = B9600;
		break;
#ifdef B19200
	case 192:
		spd = B19200;
		break;
#else /* B19200 */
#  ifdef EXTA
	case 192:
		spd = EXTA;
		break;
#else    /* EXTA */
	case 192:
		spd = B9600;
		break;
#endif   /* EXTA */
#endif	 /* B19200 */
#ifdef B38400
	case 384:
		spd = B38400;
		break;
#else /* B38400 */
#  ifdef EXTB
	case 384:
		spd = EXTB;
		break;
#else     /* EXTB */
	case 384:
		spd = B9600;
		break;
#endif    /* EXTB */
#endif	 /* B38400 */
#ifdef B57600
	case 576:
		spd = B57600;
		break;
#endif
#ifdef B115200
	case 1152:
		spd = B115200;
		break;
#endif
#ifdef B230400
	case 2304:
		spd = B230400;
		break;
#endif
#ifdef B460800
	case 4608:
		spd = B460800;
		break;
#endif
#ifdef B500000
	case 5000:
		spd = B500000;
		break;
#endif
#ifdef B576000
	case 5760:
		spd = B576000;
		break;
#endif
#ifdef B921600
	case 9216:
		spd = B921600;
		break;
#endif
#ifdef B1000000
	case 10000:
		spd = B1000000;
		break;
#endif
#ifdef B1152000
	case 11520:
		spd = B1152000;
		break;
#endif
#ifdef B1500000
	case 15000:
		spd = B1500000;
		break;
#endif
#ifdef B2000000
	case 20000:
		spd = B2000000;
		break;
#endif
#ifdef B2500000
	case 25000:
		spd = B2500000;
		break;
#endif
#ifdef B3000000
	case 30000:
		spd = B3000000;
		break;
#endif
#ifdef B3500000
	case 35000:
		spd = B3500000;
		break;
#endif
#ifdef B4000000
	case 40000:
		spd = B4000000;
		break;
#endif
	}

	return spd;
}

/**
 * @brief Configures the TTY (serial port) settings.
 *
 * Sets the port to raw mode (no canonical processing, no echo),
 * 8-bit, no parity, 1 stop bit (8N1), and no software/hardware
 * flow control.
 *
 * @param fd The file descriptor for the TTY device.
 * @param baud The numeric baud rate (e.g., 115200).
 * @param parity Parity setting (0 for no parity).
 * @return 0 on success, or -errno on failure.
 */
static int set_interface_attribs(int fd, unsigned long baud, int parity)
{
	struct termios tty;
	speed_t speed;

	if (tcgetattr(fd, &tty) != 0) {
		return -errno;
	}

	speed = baud2speed(baud);
	if (speed == LUDLC_BAUD_UNSUPPORTED) {
		LUDLC_LOG_ERROR("unsupported baud rate %lu", baud);
		return -EINVAL;
	}

	cfsetospeed(&tty, speed);
	cfsetispeed(&tty, speed);

	/* 8-bit chars */
	tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
	tty.c_iflag &= ~IGNBRK;		/* disable break processing */
	tty.c_lflag = 0;		/* no signaling chars, no echo, */
					/* no canonical processing */
	tty.c_oflag = 0;		/* no remapping, no delays */
	tty.c_cc[VMIN] = 0;		/* read doesn't block */
	tty.c_cc[VTIME] = 5;		/* 0.5 seconds read timeout */

	tty.c_iflag &= ~(IXON | IXOFF | IXANY);	/* shut off xon/xoff ctrl */

	tty.c_cflag |= (CLOCAL | CREAD);	/* ignore modem controls, */
						/* enable reading */
	tty.c_cflag &= ~(PARENB | PARODD);	/* shut off parity */
	tty.c_cflag |= parity;
	tty.c_cflag &= ~CSTOPB;
#ifdef CRTSCTS
	tty.c_cflag &= ~CRTSCTS;
#endif
	if (tcsetattr(fd, TCSANOW, &tty) != 0) {
		return -errno;
	}

	return 0;
}

/**
 * @brief Sets the blocking mode for TTY reads.
 *
 * @param fd The file descriptor for the TTY device.
 * @param should_block 1 to set blocking (VMIN=1), 0 for non-blocking (VMIN=0).
 */
static void set_blocking(int fd, int should_block)
{
	struct termios tty;

	memset(&tty, 0, sizeof tty);

	if (tcgetattr(fd, &tty) != 0) {
		return;
	}

	tty.c_cc[VMIN] = should_block ? 1 : 0;
	tty.c_cc[VTIME] = 5; /* 0.5 seconds read timeout */

	tcsetattr(fd, TCSANOW, &tty);
}

/**
 * @brief Reads data from the serial port.
 *
 * Polls the UART descriptor and the RX wakeup pipe, which
 * `ludlc_serial_connection_destroy()` writes to in order to unblock this
 * function during shutdown.
 *
 * @param sconn Pointer to the POSIX serial connection state.
 * @param buf Buffer to store read data.
 * @param buf_size Size of the read buffer.
 * @param timeout_ms Timeout for the poll.
 * @return Number of bytes read on success.
 * @return -EAGAIN on timeout.
 * @return 0 if unblocked by the event pipe (shutdown).
 * @return -errno on poll/read error.
 */
static
ssize_t ludlc_serial_read(struct ludlc_serial_connection *sconn,
		void *buf, size_t buf_size, int timeout_ms)
{
	int ret;

	struct pollfd pfd[2];

	pfd[0].fd = sconn->conn.pconn.rx_pipe[0];
	pfd[0].events = POLLIN;

	pfd[1].fd = sconn->uart_fd;
	pfd[1].events = POLLIN | POLLERR | POLLHUP;

	ret = poll(pfd, 2, timeout_ms);

	if (ret == -1) {
		LUDLC_LOG_ERROR("poll failed, errno = %d\n", errno);
		return -errno;
	}

	if (ret == 0) {
		/* Timeout, no data */
		return -EAGAIN;
	}

	/* Any wakeup without UART data is a transport error or an unblock. */
	ret = -EPIPE;
	if (pfd[0].revents & POLLIN) {
		char tmp;
		/* Drain pipe to coalesce wakeups and avoid stale events. */
		while (read(pfd[0].fd, &tmp, 1) > 0)
			;
	}

	if (pfd[1].revents & POLLIN) {
		ret = read(pfd[1].fd, buf, buf_size);

		/*
		 * poll() can race with the producer: normalize transient
		 * read errors to -errno so callers can handle -EAGAIN.
		 */
		if (ret < 0)
			ret = -errno;
	}

	return ret;
}

/**
 * @brief Whether the caller is the RX or the TX thread of @p conn.
 *
 * What ludlc_in_callback() asks the transport. A thread not started, or
 * already joined, leaves a stale ID behind: the flags say which IDs count.
 *
 * @param conn The connection.
 * @return true on one of the two threads.
 */
static bool ludlc_serial_in_callback(struct ludlc_connection *conn)
{
	struct ludlc_serial_connection *sconn =
		container_of(conn, struct ludlc_serial_connection, conn);
	pthread_t self = pthread_self();

	return ((sconn->flags & RX_THREAD_OK) &&
		pthread_equal(self, sconn->rx_thread)) ||
	       ((sconn->flags & TX_THREAD_OK) &&
		pthread_equal(self, sconn->tx_thread));
}

/**
 * @brief Main thread function for the serial receiver (RX).
 *
 * Reads octets from the TTY and feeds them to the framing decoder. Completed
 * packets are delivered through @ref ludlc_receive.
 *
 * @param arg A void pointer to the `struct ludlc_serial_connection`.
 * @return Always returns NULL.
 */
static void *ludlc_rx_serial_thread(void *arg)
{
	struct ludlc_serial_connection *sconn = arg;
	struct ludlc_connection *conn = &sconn->conn;
	uint8_t buf[16];
	/* The frame being decoded: a packet and its checksum. */
	uint8_t packet[LUDLC_FRAME_BUF_SIZE];
	const ludlc_frame_size_t pkt_cap = ludlc_conn_frame_sz(conn,
							      sconn->csum);
	ssize_t ret;

	LUDLC_LOG_DEBUG("Start serial RX thread");

	ludlc_dec_init(sconn->fs);
	ludlc_dec_prep(sconn->fs, sconn->csum, packet, pkt_cap);

	while (conn->conn_state != LUDLC_STATE_TERMINATE) {
		ssize_t i;

		ret = ludlc_serial_read(sconn, buf, sizeof(buf), -1);
		if (ret < 0) {
			if (ret != -EAGAIN) {
				ludlc_handle_disconnect(conn);
				/* A partial frame cannot be completed after
				 * a break in the line.
				 */
				ludlc_dec_init(sconn->fs);
				ludlc_dec_prep(sconn->fs, sconn->csum, packet,
					       pkt_cap);
			}
			continue;
		}

		for (i = 0; i < ret;) {
			ludlc_timestamp_t ts;
			bool done;

			i += (ssize_t)ludlc_dec_feed(sconn->fs, sconn->csum,
						     conn, &buf[i],
						     (size_t)(ret - i), &done);
			if (!done)
				continue;

			if (!sconn->conn.cfg.get_timestamp(&ts)) {
				ludlc_packet_t *pkt = (ludlc_packet_t *)packet;
				ludlc_payload_size_t sz =
						ludlc_dec_size(sconn->fs);

				if ((pkt->hdr.id.tx_id & LUDLC_PING_FLAG) == 0) {
					LUDLC_LOG_TRACE(
					"packet tx_id=%x, ack_id=%x, ch=%d, sz=%zu",
					pkt->hdr.id.tx_id,
					pkt->hdr.id.ack_id, pkt->hdr.chan,
					sz);
				}
				ludlc_receive(&sconn->conn, pkt, sz, ts);
			} else {
				LUDLC_LOG_ERROR(
				  "Can't get the timestamp, drop the packet");
			}

			ludlc_dec_prep(sconn->fs, sconn->csum, packet, pkt_cap);
		}
	}

	LUDLC_LOG_DEBUG("Exit from serial RX thread");

	return NULL;
}

/**
 * @brief Drains the transmit (TX) FIFO to the serial transport.
 *
 * @param sconn Pointer to the POSIX serial connection state.
 * @return Bytes written, 0 if nothing moved, or a negative errno.
 */
static int send_tx_fifo(struct ludlc_serial_connection *sconn)
{
	uint8_t *data_ptr;
	unsigned int len;
	ssize_t written = 0;

	len = kfifo_get_claim(&sconn->tx_fifo, &data_ptr, UINT_MAX);
	if (len) {
		written = write(sconn->uart_fd, data_ptr, len);
		if (written == -1) {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				written = -errno; /* Actual error */
			} else {
				 /* No bytes written, but not an error */
				written = 0;
			}
		} else if (written > 0) {
			kfifo_get_finish(&sconn->tx_fifo, written);
		}
	}

	return written;
}

/**
 * @brief Load the next core packet into the framing encoder, if it is idle.
 */
static void ludlc_posix_enc_load(struct ludlc_serial_connection *sconn)
{
	const void *hdr, *payload;
	ludlc_payload_size_t hdr_sz, pay_sz;
	int pending;

	if (ludlc_enc_busy(sconn->fs))
		return;

	if (!ludlc_platform_test_and_clear_bit(LUDLC_CONN_FORCE_TX_F,
					       &sconn->conn.pconn.tx_events))
		return;

	pending = ludlc_get_packet_to_send(&sconn->conn, &hdr, &hdr_sz,
					       &payload, &pay_sz);
	if (pending >= 0) {
		/* Close the run with an EOF only when
		 * nothing follows this frame.
		 */
		ludlc_enc_prep(sconn->fs, hdr, hdr_sz, payload, pay_sz,
			       pending != 0);

		if (!pending) {
			ludlc_platform_request_tx(&sconn->conn);
		}
	}
}

/**
 * @brief Main thread function for the serial transmitter (TX).
 *
 * Pumps the framing encoder into the TX fifo and drains the fifo to the TTY.
 *
 * @param arg A void pointer to the `struct ludlc_serial_connection`.
 * @return Always returns NULL.
 */
static void *ludlc_tx_serial_thread(void *arg)
{
	struct ludlc_serial_connection *sconn = arg;
	struct ludlc_connection *conn = &sconn->conn;
	bool can_sleep = true;
	struct pollfd pfd[2];
	short int uart_events = POLLERR | POLLHUP;

	LUDLC_LOG_DEBUG("Start serial TX thread");

	ludlc_enc_init(sconn->fs);

	/* Always poll for internal events (new packet, termination) */
	pfd[0].fd = conn->pconn.tx_pipe[0];
	pfd[0].events = POLLIN;

	pfd[1].fd = sconn->uart_fd;

	while (conn->conn_state != LUDLC_STATE_TERMINATE) {
		int ret;

		if (can_sleep &&
		    !ludlc_platform_test_bit(LUDLC_CONN_FORCE_TX_F,
					     &conn->pconn.tx_events) &&
		    !ludlc_enc_busy(sconn->fs) &&
		    kfifo_is_empty(&sconn->tx_fifo)) {

			pfd[1].events = uart_events;

			ret = poll(pfd, 2, -1);

			if (ret < 0) {
				if (errno == EINTR) {
					continue;
				}

				LUDLC_LOG_ERROR("TX thread poll failed: %s",
						strerror(errno));
				break;
			}

			if (pfd[0].revents & POLLIN) {
				unsigned int tmp;

				while (read(pfd[0].fd, &tmp, sizeof(tmp)) > 0)
					;
			}

			if (conn->conn_state == LUDLC_STATE_TERMINATE) {
				break;
			}

			if (pfd[1].revents & POLLOUT) {
				ret = send_tx_fifo(sconn);
				if (ret < 0) {
					ludlc_enc_init(sconn->fs);
					LUDLC_LOG_ERROR(
						"Drop packet due to failed start_tx");
					LUDLC_INC_STATS(&sconn->conn, dropped);
					ludlc_handle_disconnect(conn);
				} else if (!kfifo_is_empty(&sconn->tx_fifo)) {
					uart_events |= POLLOUT;
				} else {
					uart_events &= ~POLLOUT;
				}
			}

			if (kfifo_is_full(&sconn->tx_fifo)) {
				uart_events |= POLLOUT;
				continue;
			}
		}

		ludlc_posix_enc_load(sconn);

		/* A full fifo leaves the rest of the frame for the next pass,
		 * woken by POLLOUT. The encoder writes into the fifo's own
		 * storage, so the frame is never copied.
		 */
		while (ludlc_enc_busy(sconn->fs)) {
			uint8_t *dst;
			unsigned int room;

			room = kfifo_put_claim(&sconn->tx_fifo, &dst,
					       UINT_MAX);
			if (!room)
				break;

			kfifo_put_finish(&sconn->tx_fifo,
				(unsigned int)ludlc_enc_fill(sconn->fs,
							     sconn->csum, dst,
							     room));

			if (!ludlc_enc_busy(sconn->fs))
				LUDLC_INC_STATS(&sconn->conn, tx_packet);
		}

		if (kfifo_is_full(&sconn->tx_fifo) &&
		    ludlc_enc_busy(sconn->fs))
			can_sleep = true;
		else if (!ludlc_enc_busy(sconn->fs) &&
			 kfifo_is_empty(&sconn->tx_fifo) &&
			 !ludlc_platform_test_bit(LUDLC_CONN_FORCE_TX_F,
						  &conn->pconn.tx_events))
			can_sleep = true;
		else
			can_sleep = false;

		ret = send_tx_fifo(sconn);
		if (ret < 0) {
			ludlc_enc_init(sconn->fs);
			LUDLC_LOG_ERROR("Drop packet due to failed start_tx");
			LUDLC_INC_STATS(&sconn->conn, dropped);
			can_sleep = true;
		} else if (!kfifo_is_empty(&sconn->tx_fifo)) {
			uart_events |= POLLOUT;
			can_sleep = true;
		} else {
			uart_events &= ~POLLOUT;
		}
	}

	LUDLC_LOG_DEBUG("Exit from serial TX thread");
	return NULL;
}

/**
 * @brief Destroys and cleans up a LuDLC serial connection.
 *
 * Signals the RX and TX threads to terminate, joins them,
 * closes all file descriptors, and frees the connection structure.
 *
 * @param conn The *core* `ludlc_connection` pointer.
 */
void ludlc_serial_connection_destroy(struct ludlc_connection *conn)
{
	struct ludlc_serial_connection *sconn =
		container_of(conn, struct ludlc_serial_connection, conn);

	/* Send a DISCONNECT before going, so that the peer need not wait out
	 * its watchdog. The core takes the link down once the frame has
	 * drained, which is what the bounded wait below is for.
	 */
	if (ludlc_platform_test_bit(LUDLC_CONN_INITED_F, &conn->flags) &&
	    ludlc_send_disconnect(conn) == 0) {
		unsigned int ms = LUDLC_DISCONNECT_WAIT_MS;

		while (ms-- && conn->conn_state == LUDLC_STATE_CONNECTED) {
			/* No descriptors to watch: just a 1 ms sleep. */
			poll(NULL, 0, 1);
		}

		tcdrain(sconn->uart_fd);
	}

	/* Signal threads to terminate */
	conn->conn_state = LUDLC_STATE_TERMINATE;

	/* Unblock and join TX thread */
	if (sconn->flags & TX_THREAD_OK) {
		char msg = 'x';

		write(conn->pconn.tx_pipe[1], &msg, 1);
		pthread_join(sconn->tx_thread, NULL);
	}

	/* Unblock and join RX thread (by writing to the pipe) */
	if (sconn->flags & RX_THREAD_OK) {
		const char msg = 'x';

		write(conn->pconn.rx_pipe[1], &msg, 1);
		pthread_join(sconn->rx_thread, NULL);
	}

	sconn->flags = 0;

	if (sconn->uart_fd >= 0) {
		ludlc_connection_cleanup(conn);
		close(sconn->uart_fd);
	}

	ludlc_framing_destroy(sconn->fs);
	free_serial_connection(sconn);
}

/**
 * @brief Creates and initializes a new LuDLC serial connection.
 *
 * Opens and configures the TTY, initializes the core connection and starts
 * the RX and TX threads.
 *
 * @param args Platform arguments: port name, baud rate, the checksum and the
 * cfg to open the link with.
 * @param[out] conn On success, this pointer is updated to point to the
 * new core `ludlc_connection`.
 * @param cb User-facing event callbacks.
 * @param user_arg User data passed to the event callbacks as is.
 * @return 0 on success.
 * @return -ENOMEM on allocation failure.
 * @return -errno on system call failures (pipe, open, pthread_create).
 */
int ludlc_serial_connection_create(const ludlc_platform_args_t *args,
		struct ludlc_connection **conn,
		const struct ludlc_conn_cb *cb,
		void *user_arg)
{
	struct ludlc_serial_connection *sconn = NULL;
	struct ludlc_conn_cfg cfg;
	int ret;

	if (!args || !conn)
		return -EINVAL;

	*conn = NULL;

	cfg = *ludlc_args_conn_cfg(args);

	/* A link may run no checksum, but one it runs must be complete and fit
	 * the register it is computed in.
	 */
	if (args->csum && (!args->csum->csum || !args->csum->octets ||
			   args->csum->octets > LUDLC_MAX_CSUM_SIZE))
		return -EINVAL;

	/*
	 * A TTY delivers octets and nothing else, so a framing that needs the
	 * transport to supply the frame length would never complete a frame.
	 */
	if (args->framing && args->framing->needs_wire_len) {
		LUDLC_LOG_ERROR("%s framing needs a transport that says where "
				"a frame ends", args->framing->name);
		return -EINVAL;
	}

	sconn = alloc_serial_connection();
	if (!sconn)
		return -ENOMEM;

	sconn->csum = args->csum;
	sconn->uart_fd = -1;

	sconn->fs = ludlc_framing_create(args->framing);
	if (!sconn->fs) {
		ret = -ENOMEM;
		goto err;
	}

	sconn->uart_fd = open(args->port, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (sconn->uart_fd < 0) {
		ret = -errno;
		goto err;
	}

	LUDLC_RING_BUF_INIT(&sconn->tx_fifo, sconn->tx_buf, LUDLC_TX_FIFO_SIZE);

	ret = ludlc_connection_init(&sconn->conn, &cfg, cb, user_arg);
	if (ret) {
		LUDLC_LOG_ERROR("Can't open a link of %u payload octets: %d",
				(unsigned int)ludlc_cfg_max_payload(&cfg),
				ret);
		goto err;
	}

	ret = set_interface_attribs(sconn->uart_fd, args->baudrate, 0);
	if (ret) {
		goto err;
	}

	/* Set before the threads run, so each knows itself from its start. */
	sconn->conn.pconn.in_callback = ludlc_serial_in_callback;

	ret = pthread_create(&sconn->rx_thread, NULL,
			ludlc_rx_serial_thread, sconn);
	if (!ret) {
		sconn->flags |= RX_THREAD_OK;
		ret = pthread_create(&sconn->tx_thread, NULL,
				ludlc_tx_serial_thread, sconn);
		if (!ret) {
			sconn->flags |= TX_THREAD_OK;
			*conn = &sconn->conn;
			ret = ludlc_connect(&sconn->conn);
			if (!ret) {
				return 0;
			}
		} else {
			ret = -ret;
		}
	} else {
		ret = -ret;
	}

err:
	/* Set above, before ludlc_connect() had its say. */
	*conn = NULL;
	ludlc_serial_connection_destroy(&sconn->conn);
	return ret;
}

/*
 * CRC-16/KERMIT, which the other LuDLC platforms send and a Linux host runs by
 * default under this name. Computed bit by bit, a host opening few links.
 */
static ludlc_csum_t ludlc_crc16_ccitt_pr0_byte(ludlc_csum_t crc, uint8_t data)
{
	int i;

	crc ^= data;
	for (i = 0; i < 8; i++)
		crc = (crc & 1U) ? (crc >> 1) ^ 0x8408U : crc >> 1;

	return crc;
}

static const struct ludlc_csum ludlc_crc16_ccitt_pr0 = {
	.name = "crc16-ccitt-pr0",
	.csum = ludlc_crc16_ccitt_pr0_byte,
	.octets = 2,
};

/**
 * @brief 115200 baud, and this build's default framing and checksum.
 *
 * No port: there is no console UART to assume on a host, so the caller names
 * the device it wants before opening the link.
 */
void ludlc_default_serial_platform_args(ludlc_platform_args_t *args)
{
	static ludlc_platform_args_t def_args = {
		.baudrate = 115200U,
		.csum = &ludlc_crc16_ccitt_pr0,
	};

	if (args) {
		*args = def_args;
	}
}

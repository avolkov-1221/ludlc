// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file ludlc_serial.c
 *
 * @brief LuDLC POSIX serial (UART) hardware implementation.
 *
 * This file provides the implementation for the serial transport
 * layer on a POSIX system. It handles:
 * - Opening and configuring TTY (UART) devices.
 * - Implementing the `ludlc_proto_cb` transport callbacks for read, write, etc.
 * - The CRC-16/KERMIT checksum implementation.
 * - Public API functions (`ludlc_serial_connection_create`,
 * `ludlc_serial_connection_destroy`) for managing the serial connection
 * lifecycle, including thread creation and cleanup.
 *
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * This file is licensed under either the Apache License, Version 2.0,
 * or the GNU General Public License, version 2 or (at your option)
 * any later version.
 */

#include <stdint.h>
#include <termios.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>

#include "ludlc_posix.h"
#include <ludlc.h>
#include <ludlc_private.h>
#include <ludlc_serial.h>
#include <ludlc_serial_enc_impl.h>

/** @brief Flag: TX thread started successfully. */
#define TX_THREAD_OK	(1<<0)
/** @brief Flag: RX thread started successfully. */
#define RX_THREAD_OK	(1<<1)

/**
 * @struct ludlc_serial_connection
 * @brief POSIX serial-specific connection structure.
 *
 * This structure wraps the core `ludlc_connection` and adds
 * platform-specific data, such as thread IDs and file descriptors
 * for the UART and an internal event pipe.
 */
struct ludlc_serial_connection {
	/*!< Internal state flags (TX_THREAD_OK, RX_THREAD_OK). */
	unsigned flags;
	/*!< POSIX thread ID for the receiver. */
	pthread_t rx_thread;
	/*!< POSIX thread ID for the transmitter. */
	pthread_t tx_thread;
	/*!< File descriptor for the UART (TTY) device. */
	int uart_fd;
	/**
	 * @brief Pipe FDs used to unblock the RX thread.
	 * `event_fd[0]` is the read end (polled by RX).
	 * `event_fd[1]` is the write end (written to by destroy).
	 */
	int event_fd[2];

	/**< Platform-level TX buffer FIFO. */
	LUDLC_DECLARE_RING_BUF(tx_fifo);

	struct ludlc_connection conn;

	/** @brief Platform-level transmit buffer. */
	uint8_t tx_buf[64];
};

#ifdef CONFIG_LUDLC_STATIC_CONN
/** @brief Static pool of serial connections. */
static struct ludlc_serial_connection g_conn[CONFIG_LUDLC_STATIC_CONN_NUM];

/**
 * @brief Allocates a serial connection from the static pool.
 * @return Pointer to a free connection, or NULL if the pool is full.
 */
static inline struct ludlc_serial_connection *alloc_serial_connection(void)
{
	/* TODO: Implement static pool allocation logic */
	if(free_conn != 0) {

	}
	return NULL;
}

/**
 * @brief Returns a serial connection to the static pool.
 * @param conn The connection to free.
 */
static inline void free_serial_connection(struct ludlc_connection *conn)
{
	/* TODO: Implement static pool free logic */
	size_t idx = conn - g_conn;

	if(idx < sizeof(g_conn) / sizeof(g_conn[0]))
		idx;
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
#endif /* CONFIG_LUDLC_STATIC_CONN */

/**
 * @brief Converts a numeric baud rate to a POSIX `speed_t` constant.
 * (Borrowed from minicom).
 *
 * @param baud The numeric baud rate (e.g., 9600, 115200).
 * @return The corresponding `speed_t` constant (e.g., `B9600`), or -1
 * if the baud rate is not supported.
 */
speed_t baud2speed(unsigned long baud)
{
	speed_t spd = -1;

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
	case 192:	spd = EXTA;	break;
#  else /* EXTA */
	case 192:	spd = B9600;	break;
#  endif /* EXTA */
#endif	 /* B19200 */
#ifdef B38400
	case 384:
		spd = B38400;
		break;
#else /* B38400 */
#  ifdef EXTB
	case 384:	spd = EXTB;	break;
#   else /* EXTB */
	case 384:	spd = B9600;	break;
#   endif /* EXTB */
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

	if(speed != -1) {
		cfsetospeed(&tty, speed);
		cfsetispeed(&tty, speed);
	}

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
static void set_blocking (int fd, int should_block)
{
	struct termios tty;
	memset(&tty, 0, sizeof tty);

	if (tcgetattr(fd, &tty) != 0) {
		return;
	}

	tty.c_cc[VMIN] = should_block ? 1 : 0;
	tty.c_cc[VTIME] = 5; // 0.5 seconds read timeout

	tcsetattr(fd, TCSANOW, &tty);
}

/**
 * @brief Reads data from the serial port.
 *
 * Implements the `rx_read` callback for `ludlc_proto_cb`.
 * This function uses `poll()` to wait for data on either the
 * UART file descriptor or the internal `event_fd` pipe. The pipe
 * is used by `ludlc_serial_connection_destroy` to unblock this
 * function during shutdown.
 *
 * @param arg The user argument (pointer to the core connection).
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

	pfd[0].fd = sconn->uart_fd;
	pfd[0].events = POLLIN;

	pfd[1].fd = sconn->event_fd[0]; /* Read end of the unblock pipe */
	pfd[1].events = POLLIN;

	ret = poll(pfd, 2, timeout_ms);

	if (ret == -1) {
		printf("poll failed, errno = %d\n", errno);
		return -errno;
	}

	if (ret == 0) {
		/* Timeout, no data */
		return -EAGAIN;
	}

	/* Check for UART data */
	if (pfd[0].revents & POLLIN)
		return read(pfd[0].fd, buf, buf_size);

	/* Pipe data available -> external unblock */
	if (pfd[1].revents & POLLIN) {
		char tmp;
		read(pfd[1].fd, &tmp, 1); /* clear the pipe */
	}

	return -EPIPE; /* Unblocked by pipe, signal to exit */
}

/**
 * @brief Main thread function for the serial receiver (RX).
 *
 * This thread continuously reads bytes from the underlying transport
 * (via `conn->proto->rx_read`) and feeds them into the serial
 * decoder state machine (`ludlc_serial_decode`).
 *
 * When a complete, valid packet is decoded, `ludlc_serial_decode`
 * will internally call `ludlc_receive` to process it.
 *
 * The thread terminates when the connection state becomes
 * `LUDLC_STATE_TERMINATE`.
 *
 * @param arg A void pointer to the `struct ludlc_connection` object.
 * @return Always returns NULL.
 */
static void *ludlc_rx_serial_thread(void *arg)
{
	struct ludlc_serial_connection *sconn = arg;
	struct ludlc_connection *conn = &sconn->conn;
	struct ludlc_sdec_state dec_state;

	LUDLC_LOG_DEBUG("Start serial RX thread");

	ludlc_serial_decoder_init(&dec_state);

	while (conn->conn_state != LUDLC_STATE_TERMINATE) {
		int c;
		/* Read one byte at a time, blocking indefinitely (-1) */
		ssize_t ret = ludlc_serial_read(sconn, &c, 1, -1);
		if (ret < 0) {
			if (ret != -EAGAIN) {
				/* Handle transport error */
				conn->cb->on_disconnect(conn, conn->user_ctx);
				continue;
			}
			break; /* EAGAIN might signal a non-blocking exit */
		}

		/* Feed the byte to the decoder state machine */
		ludlc_serial_decode(conn, &dec_state, c & 0xff);
	}

	LUDLC_LOG_DEBUG("Exit form serial RX thread");

	return NULL;
}

/**
 * @brief Drains the transmit (TX) FIFO to the serial transport.
 *
 * This function is called by the TX thread after one or more bytes
 * have been added to the `tx_fifo`. It attempts to write all
 * pending bytes from the FIFO to the platform's `tx_write` callback.
 *
 * @param conn Pointer to the connection structure.
 * @return 0 on success (all bytes written or FIFO empty).
 * @return A negative error code if `tx_write` fails.
 */
static int send_tx_fifo(struct ludlc_serial_connection *sconn)
{
	uint8_t *data_ptr;
	unsigned int len;
	ssize_t written = 0;

	len = kfifo_out_linear_ptr(&sconn->tx_fifo, &data_ptr, UINT_MAX);
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
			kfifo_skip_count(&sconn->tx_fifo, written);
		}
	}

	return written;
}

/**
 * @brief Main thread function for the serial transmitter (TX).
 *
 * This thread manages the "pull" side of the transmit pipeline. It waits
 * on a wait queue until it's signaled to send data (by
 * `ludlc_request_tx`) or the previous packet has been fully encoded.
 *
 * When woken:
 * 1. If the serial encoder is idle, it fetches the next LuDLC packet
 * (or PING) from the core logic via `ludlc_get_packet_to_send`.
 * 2. It resets the ping timer.
 * 3. It generates one byte from the serial encoder (`ludlc_serial_encode`).
 * 4. It places this byte into the `tx_fifo`.
 * 5. It calls `serial_start_tx` to drain the `tx_fifo` to the hardware.
 *
 * This process repeats until the encoder is idle again, at which point
 * it may go back to sleep waiting for the next packet.
 *
 * @param arg A void pointer to the `struct ludlc_connection` object.
 * @return Always returns NULL.
 */
static void *ludlc_tx_serial_thread(void *arg)
{
	struct ludlc_serial_connection *sconn = arg;
	struct ludlc_connection *conn = &sconn->conn;
	struct ludlc_senc_state enc_state;
	bool packet_encoded = true;

	LUDLC_LOG_DEBUG("Start serial TX thread");

	ludlc_serial_encoder_init(&enc_state);

	while (conn->conn_state != LUDLC_STATE_TERMINATE) {
		struct pollfd pfd[2];
		int nfds = 0;
		int timeout_ms = -1; /* Default to infinite wait */
		int poll_ret;

		/* Always poll for internal events (new packet, termination) */
		pfd[nfds].fd = conn->pconn.tx_pipe[0];
		pfd[nfds].events = POLLIN;
		nfds++;

		/* If TX FIFO has data, also poll for UART writability */
		if (!kfifo_is_empty(&sconn->tx_fifo)) {
			pfd[nfds].fd = sconn->uart_fd;
			pfd[nfds].events = POLLOUT;
			nfds++;
			/* Short timeout if we have data to send,
			 * to allow processing internal events
			 */
			timeout_ms = 100;
		} else {
			/* If FIFO is empty, and no new packet is being encoded,
			 * we can wait indefinitely for a new internal event. */
			if (packet_encoded &&
				!ludlc_platform_test_bit(LUDLC_CONN_FORCE_TX_F,
					&conn->pconn.tx_events)) {
				timeout_ms = -1;
			}
		}

		poll_ret = poll(pfd, nfds, timeout_ms);

		if (poll_ret == -1) {
			if (errno == EINTR) {
				continue; /* Interrupted by signal, retry poll */
			}
			LUDLC_LOG_ERROR("TX thread poll failed: %s",
				strerror(errno));
			break; /* Fatal error */
		}

		if (poll_ret == 0) {
			/* Timeout, no events. This can happen if timeout_ms
			 * is not -1.
			 *
			 *  Try to drain FIFO anyway, in case a partial write
			 *  happened just before poll.
			 */
			send_tx_fifo(sconn);
			continue;
		}

		/* Check for internal wakeup event */
		if (pfd[0].revents & POLLIN) {
			/* Clear the pipe by reading the byte(s) */
			char tmp;

			while (read(pfd[0].fd, &tmp, 1) > 0) {
				if (tmp == 'X') {
					/* Termination signal received, exit thread */
					goto out;
				}
			}
		}

		/* Check for UART writability */
		if (nfds > 1 && (pfd[1].revents & POLLOUT)) {
			send_tx_fifo(sconn);
		}

		/* If encoder is idle and we are forced to send (or need to send a new packet) */
		if (ludlc_serial_encoder_idle(&enc_state) &&
			ludlc_platform_test_and_clear_bit(LUDLC_CONN_FORCE_TX_F,
				&conn->pconn.tx_events)) {
			/* Encoder is ready for a new packet. Get one. */
			int is_tx_empty = ludlc_get_packet_to_send(conn,
				&enc_state.hdr,
				&enc_state.hdr_size,
				&enc_state.payload,
				&enc_state.payload_size);
			if (is_tx_empty < 0) {
				/* TODO: Error getting packet */
				LUDLC_LOG_ERROR(
					"Failed to get packet to send: %d",
					is_tx_empty);
				continue;
			}

			/* Postpone the ping timer for one PING_TIME later */
			ludlc_platform_start_timer(&conn->ping_timer,
					CONFIG_LUDLC_PING_TIME,
					CONFIG_LUDLC_PING_TIME);
			/* Send EOF marker only if queue is empty */
			ludlc_serial_encoder_send_eof(&enc_state,
					is_tx_empty != 0);
			packet_encoded = false;
		}

		/* Generate one octet from the encoder and queue it,
		 * if space is available
		 */
		if (!ludlc_serial_encoder_idle(&enc_state) &&
				!kfifo_is_full(&sconn->tx_fifo)) {
			uint8_t byte = ludlc_serial_encode(conn, &enc_state);
			kfifo_put(&sconn->tx_fifo, byte);

			if (ludlc_serial_encoder_idle(&enc_state)) {
				/* The full packet has been encoded */
				LUDLC_INC_STATS(conn, tx_packet);
				packet_encoded = true;
			}

			/* After putting a byte, try to drain the FIFO immediately */
			send_tx_fifo(sconn);
		} else if (!kfifo_is_empty(&sconn->tx_fifo)) {
			/* If encoder is idle but FIFO still has data
			 * (e.g., from previous partial write), try to drain
			 */
			send_tx_fifo(sconn);
		}
	}
out:
	LUDLC_LOG_DEBUG("Exit form serial TX thread");

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
	struct ludlc_platform_connection *pconn = &conn->pconn;

	/* Signal threads to terminate */
	conn->conn_state = LUDLC_STATE_TERMINATE;

	/* Unblock and join TX thread */
	if (sconn->flags & TX_THREAD_OK) {
		char msg = 'X';
		/* Wake up TX thread */
		write(conn->pconn.tx_pipe[1], &msg, 1);
		pthread_join(sconn->tx_thread, NULL);
	}

	/* Unblock and join RX thread (by writing to the pipe) */
	if(sconn->flags & RX_THREAD_OK) {
		const char msg = 'x';
		write(sconn->event_fd[1], &msg, 1);
		pthread_join(sconn->rx_thread, NULL);
	}

	sconn->flags = 0;

	/* Clean up core connection state */
	if (sconn->uart_fd >= 0 &&
			sconn->event_fd[0] >= 0 && sconn->event_fd[1] >= 0)
		ludlc_connection_cleanup(conn);

	/* Close file descriptors */
	if(sconn->uart_fd >= 0)
		close(sconn->uart_fd);

	if(sconn->event_fd[0] >= 0)
		close(sconn->event_fd[0]);

	if(sconn->event_fd[1] >= 0)
		close(sconn->event_fd[1]);

	/* Free the container structure */
	free_serial_connection(sconn);
}

/**
 * @brief Creates and initializes a new LuDLC serial connection.
 *
 * Allocates memory, opens and configures the TTY device, creates an
 * internal pipe for thread signaling, initializes the core connection,
 * and spawns the RX and TX threads.
 *
 * @param args Platform arguments (port name, baud rate).
 * @param[out] conn On success, this pointer is updated to point to the
 * new core `ludlc_connection`.
 * @param cb User-facing event callbacks.
 * @return 0 on success.
 * @return -ENOMEM on allocation failure.
 * @return -errno on system call failures (pipe, open, pthread_create).
 */
int ludlc_serial_connection_create(const ludlc_platform_args_t *args,
		struct ludlc_connection **conn,
		const struct ludlc_proto_cb *proto,
		const struct ludlc_conn_cb *cb)
{
	struct ludlc_serial_connection *sconn = NULL;
	int ret;

	sconn = alloc_serial_connection();
	if (!sconn)
		return -ENOMEM;

	sconn->uart_fd = -1;

	/* Create event pipe */
	sconn->event_fd[0] = -1;
	sconn->event_fd[1] = -1;
	if (pipe(sconn->event_fd) < 0) {
		ret = -errno;

		if(sconn->event_fd[0] >= 0) {
			close(sconn->event_fd[0]);
			sconn->event_fd[0] = -1;
		}

		if(sconn->event_fd[1] >= 0) {
			close(sconn->event_fd[1]);
			sconn->event_fd[1] = -1;
		}

		goto err;
	}

	/* Set read end of pipe to non-blocking */
	fcntl(sconn->event_fd[0], F_SETFL, O_NONBLOCK);

	/* Open the TTY device */
	sconn->uart_fd = open(args->port, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if(sconn->uart_fd < 0) {
		ret = -errno;
		goto err;
	}

	/* Initialize the core LuDLC connection */
	ret = ludlc_connection_init(&sconn->conn, proto, cb, sconn);
	if(ret) {
		goto err;
	}

	/* Configure TTY settings */
	ret = set_interface_attribs (sconn->uart_fd, args->baudrate, 0);
	if(ret) {
		goto err;
	}

	/* Start RX thread */
	ret = pthread_create(&sconn->rx_thread, NULL,
			ludlc_rx_serial_thread, &sconn);
	if (!ret) {
		sconn->flags |= RX_THREAD_OK;
		/* Start TX thread */
		ret = pthread_create(&sconn->tx_thread, NULL,
				ludlc_tx_serial_thread, sconn);
		if(!ret) {
			sconn->flags |= TX_THREAD_OK;
			*conn = &sconn->conn;
			/* Request an initial TX (to start handshake) */
			ludlc_platform_request_tx(&sconn->conn);
			return 0;
		}
	}

	/* pthread_create sets errno on its own, convert to negative */
	ret = -ret;

err:
	/* Cleanup on failure */
	ludlc_serial_connection_destroy(&sconn->conn);
	return ret;
}

/**
 * @brief Fills a platform arguments structure with default serial settings.
 *
 * @param args The structure to fill.
 */
void ludlc_default_serial_platform_args(ludlc_platform_args_t *args)
{
	static ludlc_platform_args_t def_args = {
		.port = "/dev/ttyV0",
		.baudrate = 115200U,
	};

	if(args)
		*args = def_args;
}

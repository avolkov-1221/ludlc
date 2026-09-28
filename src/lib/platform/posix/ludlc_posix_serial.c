// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
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
 * - runs the RX and TX threads that decode and encode the frames;
 * - implements `ludlc_serial_connection_create()` and
 *   `ludlc_serial_connection_destroy()`.
 *
 * The checksum the frames carry comes from the caller in
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
#include <ludlc_serial_enc_impl.h>

/** @brief Flag: TX thread started successfully. */
#define TX_THREAD_OK	(1<<0)
/** @brief Flag: RX thread started successfully. */
#define RX_THREAD_OK	(1<<1)

#define TX_BUF_SIZE \
	LUDLC_ROUND_UP(2U * (unsigned int)LUDLC_MAX_PACKET_SIZE, sizeof(void *))

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

	/** Checksum this link runs. Borrowed from the caller's args. */
	const struct ludlc_csum *csum;

	/** @brief Platform-level transmit buffer. */
	uint8_t tx_buf[TX_BUF_SIZE];
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

	if (speed != -1) {
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
		printf("poll failed, errno = %d\n", errno);
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
 * @brief Main thread function for the serial receiver (RX).
 *
 * This thread continuously reads bytes from the TTY and feeds them into the
 * serial decoder state machine (`ludlc_serial_decode`).
 *
 * When a complete, valid packet is decoded, `ludlc_serial_decode`
 * will internally call `ludlc_receive` to process it.
 *
 * @param arg A void pointer to the `struct ludlc_serial_connection`.
 * @return Always returns NULL.
 */
static void *ludlc_rx_serial_thread(void *arg)
{
	enum {rx_idle, rx_handle } state = rx_idle;
	struct ludlc_serial_connection *sconn = arg;
	struct ludlc_connection *conn = &sconn->conn;
	struct ludlc_sdec_state dec_state;
	uint8_t buf[16];
	/* The frame being decoded: a packet and its checksum. */
	uint8_t packet[LUDLC_FRAME_BUF_SIZE];
	ssize_t ret, cnt;

	LUDLC_LOG_DEBUG("Start serial RX thread");

	ludlc_serial_decoder_init(&dec_state, sconn->csum);

	ludlc_serial_decoder_prep(&dec_state, packet, sizeof(packet));

	while (conn->conn_state != LUDLC_STATE_TERMINATE) {
		switch (state) {
		case rx_idle:
			ret = ludlc_serial_read(sconn, buf, sizeof(buf), -1);
			if (ret < 0) {
				if (ret != -EAGAIN) {
					/* Handle transport error/timeout in core. */
					ludlc_handle_disconnect(conn);
					ludlc_serial_decoder_prep(&dec_state,
							 packet,
							 sizeof(packet));
				}
				continue;
			}

			if (ret == 0)
				continue;
			state = rx_handle;
			cnt = 0;

		case rx_handle:
			/* Feed the byte to the decoder state machine */
			if (ludlc_serial_decode(conn, &dec_state, buf[cnt])) {
				ludlc_timestamp_t ts;

				if (conn->cfg.get_timestamp(&ts) == 0) {
					ludlc_packet_t *pkt =
						(ludlc_packet_t *)packet;
					if((pkt->hdr.id.tx_id &
							LUDLC_PING_FLAG) == 0) {
						LUDLC_LOG_TRACE(
							"packet tx_id=%x, "
							"ack_id=%x, ch=%d, "
							"sz=%d",
							pkt->hdr.id.tx_id,
							pkt->hdr.id.ack_id,
							pkt->hdr.chan,
							dec_state.size);
					}
					ludlc_receive(conn, pkt,
							dec_state.size,
							ts);
				} else {
					LUDLC_LOG_ERROR(
						"Can't get the timestamp, "
						"drop the packet");
				}

				ludlc_serial_decoder_prep(&dec_state,
							packet,
							sizeof(packet));
			}

			if (++cnt == ret)
				state = rx_idle;
		}
	}
out:
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

static void complete_disconnect(struct ludlc_connection *conn)
{
	ludlc_handle_disconnect(conn);
	ludlc_platform_clear_bit(
		LUDLC_CONN_DISC_AFTER_TX_F,
		&conn->flags);
}

/**
 * @brief Main thread function for the serial transmitter (TX).
 *
 * Loads the next frame when @c LUDLC_CONN_FORCE_TX_F is set and the encoder
 * has no primed header (@c hdr_size == 0 after @ref ludlc_serial_encoder_init).
 * Emits octets whenever the TX FIFO has space (including idle @c LUDLC_SOF
 * stuffing when unprimed). After a full frame, re-inits encoder state so stale
 * @c hdr / sizes cannot be re-sent on a later poll wake.
 *
 * @param arg A void pointer to the `struct ludlc_serial_connection`.
 * @return Always returns NULL.
 */
static void *ludlc_tx_serial_thread(void *arg)
{
	struct ludlc_serial_connection *sconn = arg;
	struct ludlc_connection *conn = &sconn->conn;
	struct ludlc_senc_state enc_state;
	bool disconnect_after_tx = false;
	bool can_sleep = true;
	struct pollfd pfd[2];
	short int uart_events = POLLERR | POLLHUP;

	LUDLC_LOG_DEBUG("Start serial TX thread");

	ludlc_serial_encoder_init(&enc_state, sconn->csum);

	/* Always poll for internal events (new packet, termination) */
	pfd[0].fd = conn->pconn.tx_pipe[0];
	pfd[0].events = POLLIN;

	pfd[1].fd = sconn->uart_fd;

	while (conn->conn_state != LUDLC_STATE_TERMINATE) {
		int ret;

		if (can_sleep &&
		    !ludlc_platform_test_and_clear_bit(LUDLC_CONN_FORCE_TX_F,
						      &conn->pconn.tx_events)) {

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
					ludlc_serial_encoder_init(&enc_state,
								  sconn->csum);
					LUDLC_LOG_ERROR(
						"Drop packet due to failed start_tx");
					LUDLC_INC_STATS(&sconn->conn, dropped);

					complete_disconnect(conn);
					disconnect_after_tx = false;
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

			if (disconnect_after_tx) {
				if (!ludlc_serial_encoder_idle(&enc_state) ||
				    !kfifo_is_empty(&sconn->tx_fifo)) {
					uart_events |= POLLOUT;
					continue;
				}

				complete_disconnect(conn);
				disconnect_after_tx = false;
			}

			if (!ludlc_platform_test_and_clear_bit(
				    LUDLC_CONN_FORCE_TX_F,
				    &conn->pconn.tx_events)) {
				continue;
			}
		}

		if (ludlc_serial_encoder_idle(&enc_state)) {
			/* Encoder is ready for a new packet. Get one. */
			int is_tx_empty = ludlc_get_packet_to_send(
				&sconn->conn,
				&enc_state.hdr,
				&enc_state.hdr_size,
				&enc_state.payload,
				&enc_state.payload_size);

			if (is_tx_empty < 0) {
				ludlc_serial_encoder_init(&enc_state, sconn->csum);
				LUDLC_LOG_ERROR(
					"Failed to get packet to send: %d",
					is_tx_empty);
				continue;
			}

			disconnect_after_tx =
					ludlc_is_disconnect_after_tx_packet(
						conn, enc_state.hdr,
						enc_state.hdr_size,
						enc_state.payload_size);

			/* Postpone the ping timer for one PING_TIME later */
			ludlc_platform_start_timer(&sconn->conn.ping_timer,
					CONFIG_LUDLC_PING_TIME,
					CONFIG_LUDLC_PING_TIME);

			if (!is_tx_empty) {
				/* We are sending data, demand to send more */
				ludlc_platform_request_tx(&sconn->conn);
			}

			/* Send EOF marker only if queue is empty */
			ludlc_serial_encoder_send_eof(&enc_state,
					is_tx_empty != 0);
			can_sleep = false;
		}

		/* Generate one octet from the encoder and queue it */
		if (!kfifo_is_full(&sconn->tx_fifo)) {
			uint8_t byte = ludlc_serial_encode(&enc_state);
			kfifo_put(&sconn->tx_fifo, byte);

			if (ludlc_serial_encoder_idle(&enc_state)) {
				/* The full packet has been encoded */
				LUDLC_INC_STATS(&sconn->conn, tx_packet);
				can_sleep = true;
			}
		}

		ret = send_tx_fifo(sconn);
		if (ret < 0) {
			ludlc_serial_encoder_init(&enc_state, sconn->csum);
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
	struct ludlc_platform_connection *pconn = &conn->pconn;

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

	/* Free the container structure */
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
 * @return 0 on success.
 * @return -ENOMEM on allocation failure.
 * @return -errno on system call failures (pipe, open, pthread_create).
 */
int ludlc_serial_connection_create(const ludlc_platform_args_t *args,
		struct ludlc_connection **conn,
		const struct ludlc_conn_cb *cb)
{
	struct ludlc_serial_connection *sconn = NULL;
	int ret;

	if (!args || !conn)
		return -EINVAL;

	/* The frames carry a checksum, so the caller must supply one, and it
	 * must fit the register it is computed in.
	 */
	if (!args->csum || !args->csum->csum ||
	    !args->csum->octets || args->csum->octets > LUDLC_MAX_CSUM_SIZE)
		return -EINVAL;

	sconn = alloc_serial_connection();
	if (!sconn)
		return -ENOMEM;

	sconn->csum = args->csum;
	sconn->uart_fd = -1;

	/* Open the TTY device */
	sconn->uart_fd = open(args->port, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (sconn->uart_fd < 0) {
		ret = -errno;
		goto err;
	}

	LUDLC_RING_BUF_INIT(&sconn->tx_fifo, sconn->tx_buf, TX_BUF_SIZE);

	/* Initialize the core LuDLC connection */
	ret = ludlc_connection_init(&sconn->conn, ludlc_args_conn_cfg(args), cb,
				    sconn);
	if (ret) {
		goto err;
	}

	ret = set_interface_attribs(sconn->uart_fd, args->baudrate, 0);
	if (ret) {
		goto err;
	}

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
	/* Cleanup on failure */
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
 * @brief Fills a platform arguments structure with default serial settings.
 *
 * @param args The structure to fill.
 */
void ludlc_default_serial_platform_args(ludlc_platform_args_t *args)
{
	static ludlc_platform_args_t def_args = {
		.baudrate = 115200U,
		.csum = &ludlc_crc16_ccitt_pr0,
	};

	if(args)
		*args = def_args;
}

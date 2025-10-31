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
 * Copyright (C) 2025 Andrey VOLKOV <andrey@volkov.fr> and LuDLC Contributors
 *
 * This file is licensed under either the Apache License, Version 2.0,
 * or the GNU General Public License, version 2 or (at your option)
 * any later version.
 */

#include <stdio.h>

#include <fcntl.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <errno.h>
#include <error.h>
#include <stddef.h>
#include <termios.h>
#include <poll.h>

#include <ludlc.h>
#include <ludlc_private.h>
#include <ludlc_serial.h>

/** @brief Flag: TX thread started successfully. */
#define TX_THREAD_OK	(1<<0)
/** @brief Flag: RX thread started successfully. */
#define RX_THREAD_OK	(1<<1)

/**
 * @brief Get the pointer to the containing structure.
 * @param ptr Pointer to the member.
 * @param type The type of the container structure.
 * @param member The name of the member within the structure.
 * @return Pointer to the container structure.
 */
#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))

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
	/*!< The core LuDLC connection state. */
	struct ludlc_connection conn;
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
 * @brief Lookup table for CRC-16/KERMIT calculation.
 */
static const uint16_t crc16_table[256] = {
	0x0000, 0x1189, 0x2312, 0x329B, 0x4624, 0x57AD, 0x6536, 0x74BF,
	0x8C48, 0x9DC1, 0xAF5A, 0xBED3, 0xCA6C, 0xDBE5, 0xE97E, 0xF8F7,
	0x1081, 0x0108, 0x3393, 0x221A, 0x56A5, 0x472C, 0x75B7, 0x643E,
	0x9CC9, 0x8D40, 0xBFDB, 0xAE52, 0xDAED, 0xCB64, 0xF9FF, 0xE876,
	0x2102, 0x308B, 0x0210, 0x1399, 0x6726, 0x76AF, 0x4434, 0x55BD,
	0xAD4A, 0xBCC3, 0x8E58, 0x9FD1, 0xEB6E, 0xFAE7, 0xC87C, 0xD9F5,
	0x3183, 0x200A, 0x1291, 0x0318, 0x77A7, 0x662E, 0x54B5, 0x453C,
	0xBDCB, 0xAC42, 0x9ED9, 0x8F50, 0xFBEF, 0xEA66, 0xD8FD, 0xC974,
	0x4204, 0x538D, 0x6116, 0x709F, 0x0420, 0x15A9, 0x2732, 0x36BB,
	0xCE4C, 0xDFC5, 0xED5E, 0xFCD7, 0x8868, 0x99E1, 0xAB7A, 0xBAF3,
	0x5285, 0x430C, 0x7197, 0x601E, 0x14A1, 0x0528, 0x37B3, 0x263A,
	0xDECD, 0xCF44, 0xFDDF, 0xEC56, 0x98E9, 0x8960, 0xBBFB, 0xAA72,
	0x6306, 0x728F, 0x4014, 0x519D, 0x2522, 0x34AB, 0x0630, 0x17B9,
	0xEF4E, 0xFEC7, 0xCC5C, 0xDDE5, 0xA95A, 0xB8D3, 0x8A48, 0x9BC1,
	0x7387, 0x620E, 0x5095, 0x411C, 0x35A3, 0x242A, 0x16B1, 0x0738,
	0xFFCF, 0xEE46, 0xDCDD, 0xCD54, 0xB9EB, 0xA862, 0x9AF9, 0x8B70,
	0x8408, 0x9581, 0xA71A, 0xB693, 0xC22C, 0xD3A5, 0xE13E, 0xF0B7,
	0x0840, 0x19C9, 0x2B52, 0x3AD1, 0x4E64, 0x5FED, 0x6D76, 0x7CFF,
	0x9489, 0x8500, 0xB79B, 0xA612, 0xD2AD, 0xC324, 0xF1BF, 0xE036,
	0x18C1, 0x0948, 0x3BD3, 0x2A5A, 0x5EE5, 0x4F6C, 0x7DF7, 0x6C7E,
	0xA50A, 0xB483, 0x8618, 0x9791, 0xE32E, 0xF2A7, 0xC03C, 0xD1B5,
	0x2942, 0x38CB, 0x0A50, 0x1BD9, 0x6F66, 0x7EEF, 0x4C74, 0x5DFD,
	0xB58B, 0xA402, 0x9699, 0x8710, 0xF3AF, 0xE226, 0xD0BD, 0xC134,
	0x39C3, 0x284A, 0x1AD1, 0x0B58, 0x7FE7, 0x6E6E, 0x5CF5, 0x4D7C,
	0xC60C, 0xD785, 0xE51E, 0xF497, 0x8028, 0x91A1, 0xA33A, 0xB2B3,
	0x4A44, 0x5BCD, 0x6956, 0x78DF, 0x0C60, 0x1DE9, 0x2F72, 0x3EFB,
	0xD68D, 0xC704, 0xF59F, 0xE416, 0x90A9, 0x8120, 0xB3BB, 0xA232,
	0x5AC5, 0x4B4C, 0x79D7, 0x685E, 0x1CE1, 0x0D68, 0x3FF3, 0x2E7A,
	0xE70E, 0xF687, 0xC41C, 0xD595, 0xA12A, 0xB0A3, 0x8238, 0x93B1,
	0x6B46, 0x7ACF, 0x4854, 0x59DD, 0x2D62, 0x3CEB, 0x0E70, 0x1FF9,
	0xF78F, 0xE606, 0xD49D, 0xC514, 0xB1AB, 0xA022, 0x92B9, 0x8330,
	0x7BC7, 0x6A4E, 0x58D5, 0x495C, 0x3DE3, 0x2C6A, 0x1EF1, 0x0F78
};

/**
 * @brief Calculates the running CRC-16/KERMIT checksum for a single byte.
 *
 * This is a mandatory function for the `ludlc_proto_cb` if hardware
 * does not have integrated packet validation (like typical UARTs).
 *
 * @param crc The current CRC value.
 * @param data The new byte to incorporate into the CRC.
 * @return The updated CRC value.
 */
static ludlc_csum_t ludlc_csum_byte(ludlc_csum_t crc, uint8_t data)
{
	return (ludlc_csum_t)((crc >> 8) ^
			crc16_table[(uint8_t)(crc & 0xFF) ^ data]);
}

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
		cfsetospeed(&tty, baud);
		cfsetispeed(&tty, baud);
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
	tty.c_cc[VTIME] = 5;		// 0.5 seconds read timeout

	tcsetattr(fd, TCSANOW, &tty);
}

/**
 * @brief Gets the current high-resolution monotonic timestamp.
 *
 * Implements the `get_timestamp` callback for `ludlc_proto_cb`.
 * Exits on failure as this is considered a fatal error.
 *
 * @return The current time in microseconds.
 */
static ludlc_timestamp_t ludlc_get_timestamp(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
		exit(EXIT_FAILURE);
	}

	return ts.tv_sec * 1000000U + (uint64_t) (ts.tv_nsec / 1000);
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
ssize_t ludlc_serial_read(void *arg, void *buf, size_t buf_size, int timeout_ms)
{
	int ret;
	struct ludlc_serial_connection *sconn = container_of(arg,
			struct ludlc_serial_connection, conn);

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
 * @brief Writes data to the serial port.
 *
 * Implements the `tx_write` callback for `ludlc_proto_cb`.
 * This function will loop until all requested bytes are written
 * or an error occurs (handling partial writes).
 *
 * @param arg The user argument (pointer to the core connection).
 * @param buf Buffer of data to write.
 * @param buf_size Number of bytes to write.
 * @return The number of bytes written on success.
 * @return -errno on a write error.
 */
static ssize_t ludlc_serial_write(void *arg, const void *buf, size_t buf_size)
{
	struct ludlc_serial_connection *sconn = container_of(arg,
			struct ludlc_serial_connection, conn);

	ssize_t written = 0;

	for (written = 0; written < (ssize_t)buf_size;) {
		ssize_t n = write(sconn->uart_fd, buf + written,
				buf_size - written);
		if (n > 0) {
			written += n;
		} else if (n == -1) {
			/* Handle write error */
			return -errno;
		}
		/* n == 0 is unlikely in blocking write but handled by loop */
	}

	return written;
}

/**
 * @brief The `ludlc_proto_cb` implementation for the POSIX serial transport.
 */
static struct ludlc_proto_cb proto = {
	.tx_write = ludlc_serial_write,

	.rx_read = ludlc_serial_read,

	.csum_byte = ludlc_csum_byte,
	.get_timestamp = ludlc_get_timestamp,
};

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

	/* Signal threads to terminate */
	conn->conn_state = LUDLC_STATE_TERMINATE;

	/* Unblock and join TX thread */
	if(sconn->flags & TX_THREAD_OK) {
		LUDLC_WQ_WAKEUP(conn->tx_wq, {});
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
	ludlc_connection_cleanup(&sconn->conn);

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
		struct ludlc_conn_cb *cb)
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
		ret = errno;
		goto err;
	}

	/* Set read end of pipe to non-blocking */
	fcntl(sconn->event_fd[0], F_SETFL, O_NONBLOCK);

	/* Open the TTY device */
	sconn->uart_fd = open(args->port, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if(sconn->uart_fd < 0) {
		ret = errno;
		goto err;
	}

	/* Initialize the core LuDLC connection */
	ret = ludlc_connection_init(&sconn->conn, &proto, cb, &sconn->conn);
	if(ret) {
		goto err;
	}

	/* Configure TTY settings */
	ret = set_interface_attribs (sconn->uart_fd, args->baudrate, 0);
	if(ret) {
		goto err;
	}
	/* set no blocking */
	set_blocking(sconn->uart_fd, 0);

	/* Start RX thread */
	ret = pthread_create(&sconn->rx_thread, NULL,
			ludlc_rx_serial_thread, &sconn->conn);
	if (!ret) {
		sconn->flags |= RX_THREAD_OK;
		/* Start TX thread */
		ret = pthread_create(&sconn->tx_thread, NULL,
				ludlc_tx_serial_thread, &sconn->conn);
		if(!ret) {
			sconn->flags |= TX_THREAD_OK;
			*conn = &sconn->conn;
			/* Request an initial TX (to start handshake) */
			ludlc_request_tx(&sconn->conn);
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

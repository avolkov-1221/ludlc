// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file ludlc_zephyr_serial.c
 *
 * @brief LuDLC serial transport implementation for Zephyr.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * UART communication in interrupt-driven and asynchronous modes, the RX and TX
 * threads, and the Zephyr primitives they use: events, FIFOs and memory slabs.
 * A link runs the asynchronous API where its UART can receive with it, which
 * is by DMA in most drivers, and the interrupt-driven API otherwise.
 * The checksum the framing runs comes from the caller in
 * `ludlc_platform_args_t`, from those in ludlc_zephyr_csum.c or its own.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/spinlock.h>
#include <zephyr/net_buf.h>
#include <zephyr/drivers/uart.h>

#include <ludlc.h>
#include <ludlc_dt.h>
#include <ludlc_platform.h>
#include <ludlc_private.h>
#include <ludlc_serial.h>
#include <ludlc_framing.h>
#include <ludlc_logger.h>

/* A ludlc,uart node no link could be opened from stops the build here. */
DT_FOREACH_STATUS_OKAY(ludlc_uart, LUDLC_DT_CHECK)

#define RX_THREAD_STACK_SIZE 1024
#ifdef CONFIG_LUDLC_SERIAL_SINGLE_THREAD
/* The one thread also runs the receive path, and needs its stack. */
#define TX_THREAD_STACK_SIZE RX_THREAD_STACK_SIZE
#else
#define TX_THREAD_STACK_SIZE 512
#endif

/*
 * The priorities of arguments not built from a devicetree node: the defaults
 * of ludlc,uart.yaml.
 */
#define RX_THREAD_PRIO 7
#define TX_THREAD_PRIO 7

#ifdef CONFIG_LUDLC_NUM_RX_BUFFERS
#define NUM_RX_BUFFERS CONFIG_LUDLC_NUM_RX_BUFFERS
#else
#define NUM_RX_BUFFERS 3
#endif

/*
 * The size of each of the two buffers a UART receives into with the
 * asynchronous API. The octets are decoded as the driver reports them, so the
 * buffers only have to cover the time between two reports.
 */
#ifdef CONFIG_LUDLC_SERIAL_ASYNC_RX_BUF_SIZE
#define RX_ASYNC_BUF_SIZE CONFIG_LUDLC_SERIAL_ASYNC_RX_BUF_SIZE
#else
#define RX_ASYNC_BUF_SIZE 64
#endif

/*
 * The driver reports what it has received once the line has been idle for
 * this many characters, or for RX_ASYNC_TIMEOUT_US where the baud rate is not
 * known. Never for no time at all, which leaves a driver to its own idea of
 * an idle line. The STM32 one then waits for the idle-line interrupt, and
 * loses it when the line goes idle while it swaps its DMA buffers: the end of
 * a frame stays unreported until the next frame arrives.
 */
#define RX_ASYNC_TIMEOUT_CHARS	2
#define RX_ASYNC_TIMEOUT_US	100

#define TX_SPACE_AVAIL_EVT	BIT(LUDLC_PLATFORM_LAST_TX_EVENT)
#define TX_EMPTY_EVT		BIT(LUDLC_PLATFORM_LAST_TX_EVENT + 1)

#if IS_ENABLED(CONFIG_LUDLC_SERIAL_SINGLE_THREAD)
/* RX_READY_EVT, posted to tx_events when no RX thread exists. */
#define RX_READY_EVT		BIT(LUDLC_PLATFORM_LAST_TX_EVENT + 2)
/* What else wakes the TX thread when it serves the receive path too. */
#define TX_RX_EVENTS		(RX_READY_EVT | LUDLC_CONN_RX_TOUT_EVT)
#else
#define RX_READY_EVT		BIT(LUDLC_PLATFORM_LAST_RX_EVENT)
/* Ends the RX thread. The single thread exits on LUDLC_CONN_TX_EXIT_EVT. */
#define RX_EXIT_EVT		BIT(LUDLC_PLATFORM_LAST_RX_EVENT + 1)
#define TX_RX_EVENTS		0
#endif

/** @brief How long the shutdown waits for the DISCONNECT to go out, in ms. */
#define LUDLC_DISCONNECT_WAIT_MS	50U

#define TX_ASYNC_BUSY	0
#define USE_ASYNC_API	1

struct rx_packet_item {
	void *fifo_reserved;
	ludlc_payload_size_t size;
	/* The frame being decoded: a packet and its checksum. */
	uint8_t payload[LUDLC_FRAME_BUF_SIZE];
};

#define RX_BUF_ALIGNED_SIZE	\
	ROUND_UP(sizeof(struct rx_packet_item), sizeof(void *))

struct ludlc_serial_connection {
#ifndef CONFIG_LUDLC_SERIAL_SINGLE_THREAD
	K_KERNEL_STACK_MEMBER(rx_thread_stack, RX_THREAD_STACK_SIZE);
#endif
	K_KERNEL_STACK_MEMBER(tx_thread_stack, TX_THREAD_STACK_SIZE);

	uint8_t rx_slab_bufs[RX_BUF_ALIGNED_SIZE * NUM_RX_BUFFERS];
	uint8_t	tx_rb_buffer[LUDLC_TX_FIFO_SIZE];
#if IS_ENABLED(CONFIG_UART_ASYNC_API)
	/* What the UART receives into with the asynchronous API. */
	uint8_t rx_async_buf[2][RX_ASYNC_BUF_SIZE];
	/* The one of the two the driver was last given. */
	uint8_t rx_async_idx;
#endif

	ludlc_platform_atomic_t flags;
	const struct device *uart_dev;	/* Zephyr UART device */

	k_tid_t tx_thread;
#ifndef CONFIG_LUDLC_SERIAL_SINGLE_THREAD
	k_tid_t rx_thread;
	struct k_thread rx_thread_data;
#endif
	struct k_thread tx_thread_data;

	struct k_fifo		rx_fifo;
	struct k_mem_slab	rx_slab;

	atomic_ptr_t		curr_rx_buf;

	/** Framing state of this link, allocated at open. The decoder runs
	 * in the UART IRQ, or in the callback of the asynchronous API.
	 */
	struct ludlc_framing_state *fs;

	struct ring_buf		tx_rb;

	struct ludlc_connection	conn;  /* Core LuDLC connection */

	/* Checksum this link runs. Borrowed from the caller's args. */
	const struct ludlc_csum *csum;
};

#ifdef CONFIG_LUDLC_STATIC_SERIAL_CONN
static struct ludlc_serial_connection
	g_conn[CONFIG_LUDLC_STATIC_SERIAL_CONN_NUM];
static LUDLC_DECLARE_BITSET(g_conn_busy, CONFIG_LUDLC_STATIC_SERIAL_CONN_NUM);

/**
 * @brief Takes a serial connection from the static pool.
 * @return Pointer to a free zeroed connection, or NULL if the pool is full.
 */
static inline struct ludlc_serial_connection *alloc_serial_connection(void)
{
	const int slot = ludlc_bitset_claim(g_conn_busy,
					CONFIG_LUDLC_STATIC_SERIAL_CONN_NUM);

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
 * @param conn The connection to release (must be from @ref g_conn).
 */
static inline void free_serial_connection(struct ludlc_serial_connection *conn)
{
	ludlc_platform_clear_bit(conn - g_conn, g_conn_busy);
}
#else
/**
 * @brief Allocates a new serial connection on the heap.
 * @return Pointer to a zeroed connection, or NULL on failure.
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
#endif /* CONFIG_LUDLC_STATIC_SERIAL_CONN */

/* --- UART Callbacks --- */

/** @brief Checks if the link runs the asynchronous UART API. */
static inline bool ludlc_serial_is_async(struct ludlc_serial_connection *sconn)
{
	return IS_ENABLED(CONFIG_UART_ASYNC_API) &&
	       ludlc_platform_test_bit(USE_ASYNC_API, &sconn->flags);
}

/**
 * @brief Feeds received octets to the framing decoder.
 *
 * Runs in the UART interrupt, or in the callback of the asynchronous API.
 * Each frame the decoder completes is queued for the receive path, and the
 * decoder goes on in the next free buffer.
 *
 * @param sconn The connection.
 * @param buf The octets as they came off the wire.
 * @param n How many.
 * @return true if all of @p buf was taken.
 * @return false if no buffer is free. The octets left are dropped, and the
 * receive path hands a buffer back once it has read a frame.
 */
static bool ludlc_serial_rx_feed(struct ludlc_serial_connection *sconn,
				 const uint8_t *buf, size_t n)
{
	struct rx_packet_item *rx_buf = atomic_ptr_get(&sconn->curr_rx_buf);
	size_t i;

	if (!rx_buf) {
		return false;
	}

	for (i = 0; i < n; ) {
		struct rx_packet_item *next_buf = NULL;
		bool done;

		i += ludlc_dec_feed(sconn->fs, sconn->csum, &sconn->conn,
				    &buf[i], n - i, &done);
		if (!done) {
			continue;
		}

		rx_buf->size = ludlc_dec_size(sconn->fs);

		if (!k_mem_slab_alloc(&sconn->rx_slab, (void **)&next_buf,
				      K_NO_WAIT)) {
			ludlc_dec_prep(sconn->fs, sconn->csum,
				       next_buf->payload,
				       ludlc_conn_frame_sz(&sconn->conn,
							   sconn->csum));
		}

		atomic_ptr_set(&sconn->curr_rx_buf, next_buf);

		k_fifo_put(&sconn->rx_fifo, rx_buf);

		k_event_post(&sconn->conn.pconn.rx_event, RX_READY_EVT);

		if (!next_buf) {
			LUDLC_INC_STATS(&sconn->conn, overrun);
			return false;
		}

		/* The frames that follow in this run go to the new buffer. */
		rx_buf = next_buf;
	}

	return true;
}

/**
 * @brief UART interrupt callback.
 *
 * Handles transmit-ready and receive-ready events: received bytes are fed to
 * the framing decoder.
 */
static void ludlc_serial_irq_cb(const struct device *dev, void *user_data)
{
	struct ludlc_serial_connection *sconn = user_data;
	/* Read in chunks rather than one octet at a time: the decoder accepts
	 * a run of them, which keeps the loop out of the IRQ.
	 */
	uint8_t buf[8];
	int n;

	uart_irq_update(dev);

	if (uart_irq_tx_ready(dev)) {
		uint8_t *data_ptr = NULL;
		uint32_t len = ring_buf_get_ptr(&sconn->tx_rb, &data_ptr, 0);

		if (len > 0) {
			int written = uart_fifo_fill(dev, data_ptr, len);

			if (written > 0) {
				ring_buf_consume(&sconn->tx_rb, written);
				k_event_post(&sconn->conn.pconn.tx_events,
					     TX_SPACE_AVAIL_EVT);

			} else {
				ring_buf_consume(&sconn->tx_rb, 0);
				if (written < 0) {
					LUDLC_LOG_ERROR(
						"uart_fifo_fill() failed: %d",
						written);
					ring_buf_reset(&sconn->tx_rb);
					uart_irq_tx_disable(dev);
					k_event_post(
						&sconn->conn.pconn.tx_events,
						TX_EMPTY_EVT |
							TX_SPACE_AVAIL_EVT);
				}
			}
		} else {
			ring_buf_consume(&sconn->tx_rb, len);
			uart_irq_tx_disable(dev);
			k_event_post(&sconn->conn.pconn.tx_events,
				     TX_EMPTY_EVT | TX_SPACE_AVAIL_EVT);
		}
	}

	if (!uart_irq_rx_ready(dev) ||
	    !atomic_ptr_get(&sconn->curr_rx_buf)) {
		return;
	}

	while ((n = uart_fifo_read(dev, buf, sizeof(buf))) > 0) {
		if (!ludlc_serial_rx_feed(sconn, buf, (size_t)n)) {
			/* Until the receive path hands a buffer back. */
			uart_irq_rx_disable(sconn->uart_dev);
			LUDLC_LOG_TRACE("RX-IRQ slab empty, disable RX IRQ");
			return;
		}
	}
}

/**
 * @brief Pass the frames the decoder completed to the core.
 *
 * Runs in the RX thread, or in the TX thread when there is no RX thread. Each
 * buffer goes back to the decoder if it ran out of them, and to the slab
 * otherwise. With the interrupt-driven API that also re-enables the RX IRQ.
 * @param sconn Pointer to the `ludlc_serial_connection` structure.
 */
static void ludlc_serial_rx(struct ludlc_serial_connection *sconn)
{
	struct rx_packet_item *rx_buf;

	while (sconn->conn.conn_state != LUDLC_STATE_TERMINATE &&
	       (rx_buf = k_fifo_get(&sconn->rx_fifo, K_NO_WAIT)) != NULL) {
		ludlc_timestamp_t ts;

		if (sconn->conn.cfg.get_timestamp(&ts) == 0) {
			const ludlc_ping_packet_t *pid =
				(const ludlc_ping_packet_t *)rx_buf->payload;
			LUDLC_LOG_TRACE("<< id=0x%02x ack=0x%02x len=%u "
					"stats{bad=%u drop=%u jab=%u ov=%u}",
					(unsigned int)pid->tx_id,
					(unsigned int)pid->ack_id,
					(unsigned int)rx_buf->size,
					(unsigned int)sconn->conn.stats.bad_csum,
					(unsigned int)sconn->conn.stats.dropped,
					(unsigned int)sconn->conn.stats.jabber,
					(unsigned int)sconn->conn.stats.overrun);
			ludlc_receive(&sconn->conn,
					(ludlc_packet_t *)rx_buf->payload,
					rx_buf->size, ts);
		}

		if (!atomic_ptr_get(&sconn->curr_rx_buf)) {
			ludlc_dec_prep(sconn->fs, sconn->csum, rx_buf->payload,
				ludlc_conn_frame_sz(&sconn->conn, sconn->csum));

			atomic_ptr_set(&sconn->curr_rx_buf, rx_buf);
			if (!ludlc_serial_is_async(sconn)) {
				uart_irq_rx_enable(sconn->uart_dev);
			}
		} else {
			k_mem_slab_free(&sconn->rx_slab, rx_buf);
		}
	}
}

#ifndef CONFIG_LUDLC_SERIAL_SINGLE_THREAD
/**
 * @brief RX serial thread entry point.
 *
 * Passes the frames queued on `rx_fifo` to the core, and takes the link down
 * when the watchdog fires.
 * @param p1 Pointer to the `ludlc_serial_connection` structure.
 * @param p2 Unused.
 * @param p3 Unused.
 */
static void ludlc_serial_rx_thread(void *p1, void *p2, void *p3)
{
	struct ludlc_serial_connection *sconn = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	LUDLC_LOG_DEBUG("Start the serial RX thread");

	while (sconn->conn.conn_state != LUDLC_STATE_TERMINATE) {
		uint32_t evt;

		ludlc_serial_rx(sconn);

		evt = k_event_wait_safe(&sconn->conn.pconn.rx_event,
					RX_READY_EVT | RX_EXIT_EVT |
					LUDLC_CONN_RX_TOUT_EVT,
					false, K_FOREVER);

		if (evt & RX_EXIT_EVT) {
			break;
		}

		if (evt & LUDLC_CONN_RX_TOUT_EVT) {
			ludlc_handle_disconnect(&sconn->conn);
		}
	}

	LUDLC_LOG_DEBUG("Exit from the serial RX thread");
}
#endif /* !CONFIG_LUDLC_SERIAL_SINGLE_THREAD */

/**
 * @brief Initiates serial transmission.
 *
 * Starts or continues sending from the TX ring buffer, using the asynchronous
 * or the interrupt-driven UART API.
 * @param sconn Pointer to the `ludlc_serial_connection` structure.
 */
static void ludlc_serial_start_tx(struct ludlc_serial_connection *sconn)
{
	if (ludlc_serial_is_async(sconn)) {
		uint8_t *data_ptr;
		uint32_t len;

		if (ludlc_platform_test_and_set_bit(TX_ASYNC_BUSY,
						    &sconn->flags)) {
			return;
		}

		len = ring_buf_get_ptr(&sconn->tx_rb, &data_ptr, 0);

		if (len > 0) {
			int ret = uart_tx(sconn->uart_dev, data_ptr, len,
					  SYS_FOREVER_US);

			if (ret < 0 && ret != -EBUSY) {
				ring_buf_consume(&sconn->tx_rb, 0);
				ludlc_platform_clear_bit(TX_ASYNC_BUSY,
							 &sconn->flags);
				ludlc_platform_request_tx(&sconn->conn);
				k_event_post(&sconn->conn.pconn.tx_events,
					     TX_EMPTY_EVT | TX_SPACE_AVAIL_EVT);
			}

		} else {
			ludlc_platform_clear_bit(TX_ASYNC_BUSY, &sconn->flags);
			k_event_post(&sconn->conn.pconn.tx_events,
				     TX_EMPTY_EVT | TX_SPACE_AVAIL_EVT);
		}

	} else if (IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN)) {
		uart_irq_tx_enable(sconn->uart_dev);
	}
}

#if IS_ENABLED(CONFIG_UART_ASYNC_API)
/**
 * @brief Starts receiving with the asynchronous UART API.
 *
 * @param sconn The connection.
 * @return 0 on success, or what uart_rx_enable() returns: a negative errno
 * if the UART cannot receive this way.
 */
static int ludlc_serial_async_rx_start(struct ludlc_serial_connection *sconn)
{
	int32_t timeout = RX_ASYNC_TIMEOUT_US;
	struct uart_config cfg;

	/* A character is ten bits on the wire. */
	if (IS_ENABLED(CONFIG_UART_USE_RUNTIME_CONFIGURE) &&
	    !uart_config_get(sconn->uart_dev, &cfg) && cfg.baudrate) {
		timeout = DIV_ROUND_UP(RX_ASYNC_TIMEOUT_CHARS * 10U *
				       USEC_PER_SEC, cfg.baudrate);
	}

	sconn->rx_async_idx = 0;

	return uart_rx_enable(sconn->uart_dev, sconn->rx_async_buf[0],
			      RX_ASYNC_BUF_SIZE, timeout);
}

/**
 * @brief Asynchronous UART callback.
 *
 * Feeds the received octets to the framing decoder and keeps the driver in
 * buffers. On `UART_TX_DONE` and `UART_TX_ABORTED` it releases the sent bytes
 * from the TX ring buffer and wakes the TX thread.
 * @param dev Pointer to the UART device.
 * @param evt Pointer to the UART event structure.
 * @param user_data Pointer to the `ludlc_serial_connection` structure.
 */
static void ludlc_serial_async_cb(const struct device *dev,
				  struct uart_event *evt, void *user_data)
{
	struct ludlc_serial_connection *sconn = user_data;

	switch (evt->type) {
	case UART_RX_RDY:
		/* With no buffer free the octets are dropped: the driver
		 * cannot be held back, and the decoder finds the next frame
		 * by itself.
		 */
		(void)ludlc_serial_rx_feed(sconn,
				evt->data.rx.buf + evt->data.rx.offset,
				evt->data.rx.len);
		break;

	case UART_RX_BUF_REQUEST:
		/* What a buffer held has been decoded by the time the driver
		 * leaves it, so the two take turns.
		 */
		sconn->rx_async_idx ^= 1U;
		(void)uart_rx_buf_rsp(dev,
				sconn->rx_async_buf[sconn->rx_async_idx],
				RX_ASYNC_BUF_SIZE);
		break;

	case UART_RX_DISABLED:
		/* The driver stops by itself on an error, or once it is out
		 * of buffers. A frame is lost, and the link goes on.
		 */
		if (sconn->conn.conn_state != LUDLC_STATE_TERMINATE &&
		    ludlc_serial_is_async(sconn)) {
			(void)ludlc_serial_async_rx_start(sconn);
		}
		break;

	case UART_TX_DONE:
	case UART_TX_ABORTED:
		ring_buf_consume(&sconn->tx_rb, evt->data.tx.len);
		ludlc_platform_clear_bit(TX_ASYNC_BUSY, &sconn->flags);

		if (evt->type == UART_TX_ABORTED) {
			ludlc_platform_request_tx(&sconn->conn);
		}

		k_event_post(&sconn->conn.pconn.tx_events, TX_SPACE_AVAIL_EVT);
		ludlc_serial_start_tx(sconn);
		break;

	default:
		break;
	}
}
#endif /* CONFIG_UART_ASYNC_API */

/**
 * @brief Whether the caller is the RX or the TX thread of @p conn.
 *
 * What ludlc_in_callback() asks the transport. Compared with the thread
 * objects, which exist before the threads run. One thread does both
 * directions with CONFIG_LUDLC_SERIAL_SINGLE_THREAD.
 *
 * @param conn The connection.
 * @return true on one of the link's threads.
 */
static bool ludlc_serial_in_callback(struct ludlc_connection *conn)
{
	struct ludlc_serial_connection *sconn =
		CONTAINER_OF(conn, struct ludlc_serial_connection, conn);
	k_tid_t self = k_current_get();

#ifndef CONFIG_LUDLC_SERIAL_SINGLE_THREAD
	if (self == &sconn->rx_thread_data) {
		return true;
	}
#endif
	return self == &sconn->tx_thread_data;
}

/**
 * @brief TX serial thread entry point.
 *
 * Pumps the framing encoder into the TX ring and starts the UART transmitter.
 * With CONFIG_LUDLC_SERIAL_SINGLE_THREAD it also serves the receive path in
 * place of the RX thread.
 */
static void ludlc_serial_tx_thread(void *p1, void *p2, void *p3)
{
	struct ludlc_serial_connection *sconn = p1;
	bool packet_done = true;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	LUDLC_LOG_DEBUG("Start the serial TX thread");

	ludlc_enc_init(sconn->fs);

	while (sconn->conn.conn_state != LUDLC_STATE_TERMINATE) {
		uint32_t evt;
		uint32_t wait_mask = 0U;

		/* An idle encoder loads a frame only on request, whatever the
		 * room in the ring: a wake for space alone must not send one.
		 */
		if (packet_done) {
			wait_mask = LUDLC_CONN_FORCE_TX_F;
		} else if (ring_buf_space_get(&sconn->tx_rb) == 0U) {
			wait_mask = TX_SPACE_AVAIL_EVT;
		}

		/* Mid-frame with free space in the ring there is nothing to wait
		 * for, but the receive path. If served here, still gets its
		 * turn.
		 */
		if (wait_mask || TX_RX_EVENTS != 0) {
			evt = k_event_wait_safe(&sconn->conn.pconn.tx_events,
					wait_mask | LUDLC_CONN_TX_EXIT_EVT |
						TX_RX_EVENTS,
					false,
					wait_mask ? K_FOREVER : K_NO_WAIT);

			if (evt & LUDLC_CONN_TX_EXIT_EVT) {
				break;
			}

			if (IS_ENABLED(CONFIG_LUDLC_SERIAL_SINGLE_THREAD)) {
				if (evt & LUDLC_CONN_RX_TOUT_EVT) {
					ludlc_handle_disconnect(&sconn->conn);
				}

				if (evt & RX_READY_EVT) {
					ludlc_serial_rx(sconn);
				}
			}

			if(wait_mask && (wait_mask & evt) == 0) {
				continue;
			}
		}

		if (!ludlc_enc_busy(sconn->fs)) {
			const void *hdr, *payload;
			ludlc_payload_size_t hdr_sz, pay_sz;
			int pending = ludlc_get_packet_to_send(&sconn->conn,
					&hdr, &hdr_sz, &payload, &pay_sz);
			if (pending >= 0) {
				/* Close the run with an EOF only when nothing
				 * follows this frame.
				 */
				ludlc_enc_prep(sconn->fs, hdr, hdr_sz,
						payload, pay_sz,
						pending != 0);
				if (!pending) {
					ludlc_platform_request_tx(&sconn->conn);
				}
				packet_done = false;
			}
		}

		/* A full ring leaves the rest of the frame for the next pass,
		 * woken by TX_SPACE_AVAIL_EVT. The encoder writes into the
		 * ring's own storage, so the frame is never copied.
		 */
		while (ludlc_enc_busy(sconn->fs)) {
			uint8_t *dst;
			uint32_t spc = ring_buf_put_ptr(&sconn->tx_rb, &dst, 0);
			if (!spc)
				break;

			ring_buf_commit(&sconn->tx_rb,
					ludlc_enc_fill(sconn->fs, sconn->csum,
							dst, spc));

			if (!ludlc_enc_busy(sconn->fs)) {
				packet_done = true;
				LUDLC_INC_STATS(&sconn->conn, tx_packet);
			}
		}

		ludlc_serial_start_tx(sconn);
	}

	LUDLC_LOG_DEBUG("Exit from the serial TX thread");
}

/* --- Connection Management --- */
/**
 * @brief Destroys a LuDLC serial connection.
 *
 * Stops the threads, disables the UART IRQs and frees the connection.
 * @param conn Pointer to the core `ludlc_connection` structure.
 */
void ludlc_serial_connection_destroy(struct ludlc_connection *conn)
{
	struct ludlc_serial_connection *sconn =
		CONTAINER_OF(conn, struct ludlc_serial_connection, conn);

	/*
	 * Send a DISCONNECT before going, so that the peer need not wait out
	 * its watchdog. The core takes the link down once the frame has
	 * drained, which is what the bounded wait below is for.
	 */
	if (ludlc_send_disconnect(conn) == 0) {
		unsigned int ms = LUDLC_DISCONNECT_WAIT_MS;

		while (ms-- && conn->conn_state == LUDLC_STATE_CONNECTED) {
			k_sleep(K_MSEC(1));
		}
	}

	conn->conn_state = LUDLC_STATE_TERMINATE;

#if !IS_ENABLED(CONFIG_LUDLC_SERIAL_SINGLE_THREAD)
	k_event_post(&conn->pconn.rx_event, RX_EXIT_EVT);
#endif
	k_event_post(&conn->pconn.tx_events, LUDLC_CONN_TX_EXIT_EVT);

	if (ludlc_serial_is_async(sconn)) {
		(void)uart_rx_disable(sconn->uart_dev);
	} else if (IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN)) {
		uart_irq_rx_disable(sconn->uart_dev);
	}

	if (sconn->tx_thread) {
		k_thread_join(sconn->tx_thread, K_FOREVER);
	}

#if !IS_ENABLED(CONFIG_LUDLC_SERIAL_SINGLE_THREAD)
	if (sconn->rx_thread) {
		k_thread_join(sconn->rx_thread, K_FOREVER);
	}
#endif
	ludlc_connection_cleanup(&sconn->conn);
	ludlc_framing_destroy(sconn->fs);
	free_serial_connection(sconn);
}


/**
 * @brief Creates and initializes a new LuDLC serial connection.
 *
 * Allocates the connection, sets up the UART and starts the RX and TX
 * threads, or the one thread with CONFIG_LUDLC_SERIAL_SINGLE_THREAD.
 * @param arg Platform arguments: the UART device, the checksum and the cfg to
 * open the link with.
 * @param conn Output pointer to the newly created core `ludlc_connection`.
 * @param cb Callback structure for the LuDLC connection.
 * @param user_arg User data passed to the event callbacks as is.
 * @return 0 on success, or a negative errno value on failure.
 */
int ludlc_serial_connection_create(const ludlc_platform_args_t *arg,
		   struct ludlc_connection **conn,
		   const struct ludlc_conn_cb *cb,
		   void *user_arg)
{
	struct ludlc_serial_connection *sconn = NULL;
	struct rx_packet_item *rx_buf = NULL;
	struct ludlc_conn_cfg cfg;
	int ret = 0;

	if (!arg || !device_is_ready(arg->dev)) {
		LUDLC_LOG_ERROR("UART device not ready");
		return -ENODEV;
	}

	cfg = *ludlc_args_conn_cfg(arg);

	/* A link may run no checksum, but one it runs must be complete and fit
	 * the register it is computed in.
	 */
	if (arg->csum && (!arg->csum->csum || !arg->csum->octets ||
			  arg->csum->octets > LUDLC_MAX_CSUM_SIZE)) {
		LUDLC_LOG_ERROR("Unusable checksum for the link");
		return -EINVAL;
	}

	/*
	 * A UART delivers octets and nothing else, so a framing that needs the
	 * transport to supply the frame length would never complete a frame.
	 */
	if (arg->framing && arg->framing->needs_wire_len) {
		LUDLC_LOG_ERROR("%s framing needs a transport that says where "
				"a frame ends", arg->framing->name);
		return -EINVAL;
	}

	sconn = alloc_serial_connection();
	if (!sconn) {
		return -ENOMEM;
	}

	sconn->uart_dev = arg->dev;
	sconn->csum = arg->csum;

	sconn->fs = ludlc_framing_create(arg->framing);
	if (!sconn->fs) {
		ret = -ENOMEM;
		goto err;
	}

	k_fifo_init(&sconn->rx_fifo);
	k_mem_slab_init(&sconn->rx_slab,
			sconn->rx_slab_bufs,
			RX_BUF_ALIGNED_SIZE,
			NUM_RX_BUFFERS);

	ludlc_dec_init(sconn->fs);

	if (k_mem_slab_alloc(&sconn->rx_slab, (void **)&rx_buf, K_NO_WAIT)) {
		ret = -ENOMEM;
		goto err;
	}

	ring_buf_init(&sconn->tx_rb, sizeof(sconn->tx_rb_buffer),
			sconn->tx_rb_buffer);

	ret = -ENOTSUP;

#if IS_ENABLED(CONFIG_UART_ASYNC_API)
	/*
	 * The asynchronous API first: its driver moves the octets by DMA
	 * where the UART has it, which a fast link needs. Receiving starts
	 * here, to find out whether this UART can, and what arrives is
	 * dropped until the decoder gets its buffer below.
	 */
	ret = uart_callback_set(sconn->uart_dev, ludlc_serial_async_cb,
			(void *)sconn);
	if (!ret) {
		ret = ludlc_serial_async_rx_start(sconn);
		if (!ret) {
			ludlc_platform_set_bit(USE_ASYNC_API, &sconn->flags);
		} else {
			(void)uart_callback_set(sconn->uart_dev, NULL, NULL);
		}
	}
#endif

	if (IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN) && ret) {
		ret = uart_irq_callback_user_data_set(sconn->uart_dev,
				ludlc_serial_irq_cb,
				(void *)sconn);
	}

	if (ret) {
		goto err;
	}

	LUDLC_LOG_DEBUG("The link runs the %s UART API",
			ludlc_serial_is_async(sconn) ? "asynchronous" :
						       "interrupt-driven");

	ret = ludlc_connection_init(&sconn->conn, &cfg, cb, user_arg);
	if (ret) {
		LUDLC_LOG_ERROR("Can't open a link of %u payload octets: %d",
				(unsigned int)ludlc_cfg_max_payload(&cfg), ret);
		goto err;
	}

	/*
	 * A byte stream carries no frame length, so the framing finds the end
	 * itself; the transport only provides the buffer to gather into.
	 */
	ludlc_dec_prep(sconn->fs, sconn->csum, rx_buf->payload,
			ludlc_conn_frame_sz(&sconn->conn, sconn->csum));

	/* Set before the threads run, so each knows itself from its start. */
	sconn->conn.pconn.in_callback = ludlc_serial_in_callback;

#ifndef CONFIG_LUDLC_SERIAL_SINGLE_THREAD
	sconn->rx_thread = k_thread_create(&sconn->rx_thread_data,
			sconn->rx_thread_stack,
			K_KERNEL_STACK_SIZEOF(sconn->rx_thread_stack),
			ludlc_serial_rx_thread,
			sconn, NULL, NULL,
			arg->thread.rx_prio, 0, K_NO_WAIT);
	if (!sconn->rx_thread) {
		ret = -EFAULT;
		goto err;
	}

	k_thread_name_set(sconn->rx_thread, "LuDLC RX thread");
#endif

	sconn->tx_thread = k_thread_create(&sconn->tx_thread_data,
			sconn->tx_thread_stack,
			K_KERNEL_STACK_SIZEOF(sconn->tx_thread_stack),
			ludlc_serial_tx_thread,
			sconn, NULL, NULL,
			arg->thread.tx_prio, 0, K_NO_WAIT);
	if (!sconn->tx_thread) {
		ret = -EFAULT;
		goto err;
	}

	if (IS_ENABLED(CONFIG_LUDLC_SERIAL_SINGLE_THREAD)) {
		k_thread_name_set(sconn->tx_thread, "LuDLC IO thread");
	} else {
		k_thread_name_set(sconn->tx_thread, "LuDLC TX thread");
	}

	/* The decoder has its buffer from here on, and takes what arrives. */
	atomic_ptr_set(&sconn->curr_rx_buf, rx_buf);

	if (!ludlc_serial_is_async(sconn)) {
		uart_irq_rx_enable(sconn->uart_dev);
	}

	*conn = &sconn->conn;
	ret = ludlc_connect(&sconn->conn); /* Start handshake */
	if (!ret) {
		return 0;
	}

err:
	ludlc_serial_connection_destroy(&sconn->conn);
	*conn = NULL;
	return ret;
}

/**
 * @brief The link the devicetree describes, or else the board's uart0, this
 * build's default framing, and CRC-16/KERMIT, which the other LuDLC platforms
 * send and a Linux host runs by default.
 *
 * A board with neither gets no device, which the create call then refuses.
 */
void ludlc_default_serial_platform_args(ludlc_platform_args_t *args)
{
#if DT_HAS_COMPAT_STATUS_OKAY(ludlc_uart)
	static const ludlc_platform_args_t def_args =
		LUDLC_UART_ARGS_DT(DT_COMPAT_GET_ANY_STATUS_OKAY(ludlc_uart));
#else
	static const ludlc_platform_args_t def_args = {
		.dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(uart0)),
		.csum = &ludlc_crc16_ccitt_pr0,
		.thread = {
			.rx_prio = RX_THREAD_PRIO,
			.tx_prio = TX_THREAD_PRIO,
		},
	};
#endif

	if (args) {
		*args = def_args;
	}
}

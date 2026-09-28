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
 * The checksum the framing runs comes from the caller in
 * `ludlc_platform_args_t`; this file provides only the default one.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/crc.h>
#include <zephyr/spinlock.h>
#include <zephyr/net_buf.h>
#include <zephyr/drivers/uart.h>

#include <ludlc.h>
#include <ludlc_platform.h>
#include <ludlc_private.h>
#include <ludlc_serial.h>
#include <ludlc_framing.h>
#include <ludlc_logger.h>

#define RX_THREAD_STACK_SIZE 1024
#define TX_THREAD_STACK_SIZE 512

#define RX_THREAD_PRIO 7
#define TX_THREAD_PRIO 7

#ifdef CONFIG_LUDLC_NUM_RX_BUFFERS
#define NUM_RX_BUFFERS CONFIG_LUDLC_NUM_RX_BUFFERS
#else
#define NUM_RX_BUFFERS 3
#endif

#define RX_READY_EVT	BIT(1)
#define RX_EXIT_EVT	BIT(2)

#define TX_SPACE_AVAIL_EVT	BIT(LUDLC_PLATFORM_LAST_TX_EVENT)
#define TX_EMPTY_EVT		BIT(LUDLC_PLATFORM_LAST_TX_EVENT + 1)

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
	K_KERNEL_STACK_MEMBER(rx_thread_stack, RX_THREAD_STACK_SIZE);
	K_KERNEL_STACK_MEMBER(tx_thread_stack, TX_THREAD_STACK_SIZE);

	uint8_t rx_slab_bufs[RX_BUF_ALIGNED_SIZE * NUM_RX_BUFFERS];
	uint8_t	tx_rb_buffer[LUDLC_TX_FIFO_SIZE];

	ludlc_platform_atomic_t flags;
	const struct device *uart_dev;	/* Zephyr UART device */

	k_tid_t rx_thread;
	k_tid_t tx_thread;
	struct k_thread rx_thread_data;
	struct k_thread tx_thread_data;

	struct k_fifo		rx_fifo;
	struct k_mem_slab	rx_slab;

	atomic_ptr_t		curr_rx_buf;

	/** Framing state of this link, allocated at open. The decoder runs
	 * in the UART IRQ.
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

/** @brief Arm the decoder to gather the next frame into @p buf. */
static void ludlc_serial_rx_arm(struct ludlc_serial_connection *sconn,
				void *buf)
{
	ludlc_dec_prep(sconn->fs, sconn->csum, buf,
		       ludlc_conn_frame_sz(&sconn->conn, sconn->csum));
}

/**
 * @brief Queue a completed frame and arm the next RX buffer for the decoder.
 *
 * Called from the UART IRQ, on the octet that completed @p rx_buf. Swaps the
 * slab buffer so decoding can continue while the RX thread calls
 * ludlc_receive().
 */
static void ludlc_serial_on_packet(struct ludlc_serial_connection *sconn,
				   struct rx_packet_item *rx_buf, size_t size)
{
	struct rx_packet_item *next_buf = NULL;

	rx_buf->size = (ludlc_payload_size_t)size;

	if (!k_mem_slab_alloc(&sconn->rx_slab, (void **)&next_buf, K_NO_WAIT))
		ludlc_serial_rx_arm(sconn, next_buf->payload);

	atomic_ptr_set(&sconn->curr_rx_buf, next_buf);
	k_fifo_put(&sconn->rx_fifo, rx_buf);
	k_event_post(&sconn->conn.pconn.rx_event, RX_READY_EVT);

	if (!next_buf) {
		LUDLC_INC_STATS(&sconn->conn, overrun);
		LUDLC_LOG_TRACE("RX-IRQ slab empty, disable RX IRQ");
		uart_irq_rx_disable(sconn->uart_dev);
	}
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
	uint8_t buf[16];
	struct rx_packet_item *rx_buf;
	int n;

	uart_irq_update(dev);

	if (uart_irq_tx_ready(dev)) {
		uint8_t *data_ptr = NULL;
		uint32_t len = ring_buf_get_claim(&sconn->tx_rb, &data_ptr,
						  sizeof(sconn->tx_rb_buffer));

		if (len > 0) {
			int written = uart_fifo_fill(dev, data_ptr, len);

			if (written > 0) {
				ring_buf_get_finish(&sconn->tx_rb, written);
				k_event_post(&sconn->conn.pconn.tx_events,
					     TX_SPACE_AVAIL_EVT);

			} else {
				ring_buf_get_finish(&sconn->tx_rb, 0);
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
			ring_buf_get_finish(&sconn->tx_rb, len);
			uart_irq_tx_disable(dev);
			k_event_post(&sconn->conn.pconn.tx_events,
				     TX_EMPTY_EVT | TX_SPACE_AVAIL_EVT);
		}
	}

	rx_buf = atomic_ptr_get(&sconn->curr_rx_buf);
	if (!uart_irq_rx_ready(dev) || !rx_buf) {
		return;
	}

	while ((n = uart_fifo_read(dev, buf, sizeof(buf))) > 0) {
		int i = 0;

		while (i < n) {
			bool done;

			i += (int)ludlc_dec_feed(sconn->fs, sconn->csum,
						 &sconn->conn, &buf[i],
						 (size_t)(n - i), &done);
			if (done)
				ludlc_serial_on_packet(sconn, rx_buf,
						ludlc_dec_size(sconn->fs));

			/* A completed frame swaps the buffer, and the slab may
			 * now be empty, leaving nowhere for the rest. Those
			 * octets are lost, as is everything else until a
			 * buffer frees; see the overrun counter in
			 * ludlc_serial_on_packet().
			 */
			rx_buf = atomic_ptr_get(&sconn->curr_rx_buf);
			if (!rx_buf)
				return;
		}
	}
}

/**
 * @brief RX serial thread entry point.
 *
 * Waits for packets on `rx_fifo`, passes them to `ludlc_receive()` and
 * re-enables UART RX once a buffer is free again.
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
		struct rx_packet_item *rx_buf;
		ludlc_timestamp_t ts;

		rx_buf = k_fifo_get(&sconn->rx_fifo, K_NO_WAIT);
		if (!rx_buf) {
			uint32_t evt = k_event_wait_safe(
						&sconn->conn.pconn.rx_event,
						RX_READY_EVT | RX_EXIT_EVT |
						LUDLC_CONN_RX_TOUT_EVT,
						false, K_FOREVER);

			if (evt & RX_EXIT_EVT) {
				break;
			}

			if (evt & LUDLC_CONN_RX_TOUT_EVT) {
				ludlc_handle_disconnect(&sconn->conn);
			}

			continue;
		}

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
			ludlc_serial_rx_arm(sconn, rx_buf->payload);

			atomic_ptr_set(&sconn->curr_rx_buf, rx_buf);
			uart_irq_rx_enable(sconn->uart_dev);
		} else {
			k_mem_slab_free(&sconn->rx_slab, rx_buf);
		}
	}

	LUDLC_LOG_DEBUG("Exit from the serial RX thread");
}

/**
 * @brief Initiates serial transmission.
 *
 * Starts or continues sending from the TX ring buffer, using the asynchronous
 * or the interrupt-driven UART API.
 * @param sconn Pointer to the `ludlc_serial_connection` structure.
 */
static void ludlc_serial_start_tx(struct ludlc_serial_connection *sconn)
{
	if (IS_ENABLED(CONFIG_UART_ASYNC_API) &&
	    ludlc_platform_test_bit(USE_ASYNC_API, &sconn->flags)) {
		uint8_t *data_ptr;
		uint32_t len;

		if (ludlc_platform_test_and_set_bit(TX_ASYNC_BUSY,
						    &sconn->flags)) {
			return;
		}

		len = ring_buf_get_claim(&sconn->tx_rb, &data_ptr,
					 sizeof(sconn->tx_rb_buffer));

		if (len > 0) {
			int ret = uart_tx(sconn->uart_dev, data_ptr, len,
					  SYS_FOREVER_US);

			if (ret < 0) {
				ring_buf_get_finish(&sconn->tx_rb, 0);
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

/**
 * @brief Asynchronous UART callback.
 *
 * Handles `UART_TX_DONE` and `UART_TX_ABORTED`: releases the sent bytes from
 * the TX ring buffer and wakes the TX thread.
 * @param dev Pointer to the UART device.
 * @param evt Pointer to the UART event structure.
 * @param user_data Pointer to the `ludlc_serial_connection` structure.
 */
static void ludlc_serial_async_cb(const struct device *dev,
				  struct uart_event *evt, void *user_data)
{
	struct ludlc_serial_connection *sconn = user_data;

	if (evt->type == UART_TX_DONE) {

		ring_buf_get_finish(&sconn->tx_rb, evt->data.tx.len);

		ludlc_platform_clear_bit(TX_ASYNC_BUSY, &sconn->flags);

		k_event_post(&sconn->conn.pconn.tx_events, TX_SPACE_AVAIL_EVT);

		ludlc_serial_start_tx(sconn);
	} else if (evt->type == UART_TX_ABORTED) {
		ring_buf_get_finish(&sconn->tx_rb, evt->data.tx.len);
		ludlc_platform_clear_bit(TX_ASYNC_BUSY, &sconn->flags);
		ludlc_platform_request_tx(&sconn->conn);
		k_event_post(&sconn->conn.pconn.tx_events, TX_SPACE_AVAIL_EVT);
		ludlc_serial_start_tx(sconn);
	}
}

/**
 * @brief Load the next core packet into the framing encoder, if it is idle.
 */
static void ludlc_serial_enc_load(struct ludlc_serial_connection *sconn)
{
	const void *hdr, *payload;
	ludlc_payload_size_t hdr_sz, pay_sz;
	int pending;

	if (ludlc_enc_busy(sconn->fs))
		return;

	pending = ludlc_get_packet_to_send(&sconn->conn, &hdr, &hdr_sz,
					       &payload, &pay_sz);
	if (pending >= 0) {
		/* Close the run with an EOF only when nothing
		 * follows this frame.
		 */
		ludlc_enc_prep(sconn->fs, hdr, hdr_sz, payload, pay_sz,
			       pending != 0);
		if (!pending) {
			ludlc_platform_request_tx(&sconn->conn);
		}
	}
}

/**
 * @brief TX serial thread entry point.
 *
 * Pumps the framing encoder into the TX ring and starts the UART transmitter.
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
			wait_mask = LUDLC_CONN_FORCE_TX_F |
				    LUDLC_CONN_TX_EXIT_EVT;
		} else if (ring_buf_space_get(&sconn->tx_rb) == 0U) {
			wait_mask = LUDLC_CONN_TX_EXIT_EVT | TX_SPACE_AVAIL_EVT;
		}

		if (wait_mask != 0U) {
			evt = k_event_wait_safe(&sconn->conn.pconn.tx_events,
					wait_mask, false, K_FOREVER);

			if (evt & LUDLC_CONN_TX_EXIT_EVT) {
				break;
			}

			if ((wait_mask & TX_SPACE_AVAIL_EVT) &&
				(evt & TX_SPACE_AVAIL_EVT) == 0U) {
				continue;
			}
		}

		ludlc_serial_enc_load(sconn);

		/* A full ring leaves the rest of the frame for the next pass,
		 * woken by TX_SPACE_AVAIL_EVT. The encoder writes into the
		 * ring's own storage, so the frame is never copied.
		 */
		while (ludlc_enc_busy(sconn->fs)) {
			uint8_t *dst;
			uint32_t room;

			room = ring_buf_put_claim(&sconn->tx_rb, &dst,
						  UINT32_MAX);
			if (!room)
				break;

			ring_buf_put_finish(&sconn->tx_rb,
				(uint32_t)ludlc_enc_fill(sconn->fs,
							 sconn->csum, dst,
							 room));

			if (!ludlc_enc_busy(sconn->fs))
				LUDLC_INC_STATS(&sconn->conn, tx_packet);
		}

		packet_done = !ludlc_enc_busy(sconn->fs);

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

	k_event_post(&conn->pconn.rx_event, RX_EXIT_EVT);
	k_event_post(&conn->pconn.tx_events, LUDLC_CONN_TX_EXIT_EVT);

	uart_irq_rx_disable(sconn->uart_dev);

	if (sconn->tx_thread) {
		k_thread_join(sconn->tx_thread, K_FOREVER);
	}

	if (sconn->rx_thread) {
		k_thread_join(sconn->rx_thread, K_FOREVER);
	}

	ludlc_connection_cleanup(&sconn->conn);
	ludlc_framing_destroy(sconn->fs);
	free_serial_connection(sconn);
}


/**
 * @brief Creates and initializes a new LuDLC serial connection.
 *
 * Allocates the connection, sets up the UART and starts the RX and TX
 * threads.
 * @param arg Platform arguments: the UART device, the checksum and the cfg to
 * open the link with.
 * @param conn Output pointer to the newly created core `ludlc_connection`.
 * @param cb Callback structure for the LuDLC connection.
 * @return 0 on success, or a negative errno value on failure.
 */
int ludlc_serial_connection_create(const ludlc_platform_args_t *arg,
		   struct ludlc_connection **conn,
		   const struct ludlc_conn_cb *cb)
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

	atomic_ptr_set(&sconn->curr_rx_buf, rx_buf);

	ring_buf_init(&sconn->tx_rb, sizeof(sconn->tx_rb_buffer),
			sconn->tx_rb_buffer);

	ret = -ENOTSUP;
	if (IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN)) {
		ret = uart_irq_callback_user_data_set(sconn->uart_dev,
				ludlc_serial_irq_cb,
				(void *)sconn);
	}

	if (IS_ENABLED(CONFIG_UART_ASYNC_API) && ret) {
		ret = uart_callback_set(sconn->uart_dev, ludlc_serial_async_cb,
				(void *)sconn);
		if (!ret) {
			ludlc_platform_set_bit(USE_ASYNC_API, &sconn->flags);
		}
	}

	if (ret) {
		goto err;
	}

	ret = ludlc_connection_init(&sconn->conn, &cfg, cb, &sconn->conn);
	if (ret) {
		LUDLC_LOG_ERROR("Can't open a link of %u payload octets: %d",
				(unsigned int)ludlc_cfg_max_payload(&cfg), ret);
		goto err;
	}

	/*
	 * A byte stream carries no frame length, so the framing finds the end
	 * itself; the transport only provides the buffer to gather into.
	 */
	ludlc_serial_rx_arm(sconn, rx_buf->payload);

	sconn->rx_thread = k_thread_create(&sconn->rx_thread_data,
			sconn->rx_thread_stack,
			sizeof(sconn->rx_thread_stack),
			ludlc_serial_rx_thread,
			sconn, NULL, NULL,
			RX_THREAD_PRIO, 0, K_NO_WAIT);
	if (!sconn->rx_thread) {
		ret = -EFAULT;
		goto err;
	}

	k_thread_name_set(sconn->rx_thread, "LuDLC RX thread");

	sconn->tx_thread = k_thread_create(&sconn->tx_thread_data,
			sconn->tx_thread_stack,
			sizeof(sconn->tx_thread_stack),
			ludlc_serial_tx_thread,
			sconn, NULL, NULL,
			TX_THREAD_PRIO, 0, K_NO_WAIT);
	if (!sconn->tx_thread) {
		ret = -EFAULT;
		goto err;
	}

	k_thread_name_set(sconn->tx_thread, "LuDLC TX thread");

	if (IS_ENABLED(CONFIG_UART_ASYNC_API) &&
			ludlc_platform_test_bit(USE_ASYNC_API, &sconn->flags)) {
		uart_rx_enable(sconn->uart_dev, rx_buf->payload,
			       sizeof(rx_buf->payload),
			       0);
	} else {
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

/*
 * CRC-16/KERMIT, which the other LuDLC platforms send and a Linux host runs by
 * default under this name.
 */
static ludlc_csum_t ludlc_crc16_ccitt_pr0_byte(ludlc_csum_t crc, uint8_t data)
{
	return crc16_ccitt((uint16_t)crc, &data, 1);
}

static const struct ludlc_csum ludlc_crc16_ccitt_pr0 = {
	.name = "crc16-ccitt-pr0",
	.csum = ludlc_crc16_ccitt_pr0_byte,
	.octets = 2,
};

/** @brief The board's uart0, and this build's default framing and checksum. */
void ludlc_default_serial_platform_args(ludlc_platform_args_t *args)
{
	static const ludlc_platform_args_t def_args = {
		.dev = DEVICE_DT_GET(DT_NODELABEL(uart0)),
		.csum = &ludlc_crc16_ccitt_pr0,
	};

	if (args) {
		*args = def_args;
	}
}

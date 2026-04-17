// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file ludlc_zephyr_serial.c
 *
 * @brief This file provides the Zephyr-specific implementation for the LuDLC
 * serial transport layer.
 *
 * It handles UART communication, including interrupt-driven and asynchronous
 * modes, manages RX/TX threads, and integrates with Zephyr's kernel primitives
 * like events, FIFOs, and memory slabs.
 *
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * This file is licensed under either the Apache License, Version 2.0
 * or the GNU General Public License, version 2 or (at your option)
 * any later version.
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
#include <ludlc_platform.h>
#include <ludlc_private.h>
#include <ludlc_serial.h>
#include <ludlc_serial_enc_impl.h>
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

#define TX_BUF_SIZE	ROUND_UP(2*LUDLC_MAX_PACKET_SIZE, sizeof(void *))

#define TX_ASYNC_BUSY	0
#define USE_ASYNC_API	1

struct rx_packet_item {
	void *fifo_reserved;
	ludlc_payload_size_t size;
	uint8_t payload[LUDLC_MAX_PACKET_SIZE];
};

#define RX_BUF_ALIGNED_SIZE	\
	ROUND_UP(sizeof(struct rx_packet_item), sizeof(void *))

/* Zephyr-specific connection structure */
struct ludlc_serial_connection {
	K_KERNEL_STACK_MEMBER(rx_thread_stack, RX_THREAD_STACK_SIZE);
	K_KERNEL_STACK_MEMBER(tx_thread_stack, TX_THREAD_STACK_SIZE);

	uint8_t rx_slab_bufs[RX_BUF_ALIGNED_SIZE * NUM_RX_BUFFERS];
	uint8_t	tx_rb_buffer[TX_BUF_SIZE];

	/*!< Internal state flags. */
	ludlc_platform_atomic_t flags;
	const struct device *uart_dev;	/* Zephyr UART device */

	k_tid_t rx_thread;
	k_tid_t tx_thread;
	struct k_thread rx_thread_data;
	struct k_thread tx_thread_data;

	struct k_fifo		rx_fifo;
	struct k_mem_slab	rx_slab;

	atomic_ptr_t		curr_rx_buf;
	struct ludlc_sdec_state	dec_state;

	struct ring_buf		tx_rb;

	struct ludlc_connection	conn;  /* Core LuDLC connection */
};

#ifdef CONFIG_LUDLC_STATIC_SERIAL_CONN
/** @brief Static pool of serial connections. */
K_MEM_SLAB_DEFINE_STATIC(g_conn_slab,
		sizeof(struct ludlc_serial_connection),
		CONFIG_LUDLC_STATIC_SERIAL_CONN_NUM, sizeof(void*));
/**
 * @brief Allocates a serial connection from the static pool.
 * @return Pointer to a free connection, or NULL if the pool is full.
 */
static inline struct ludlc_serial_connection *alloc_serial_connection(void)
{
	void *conn = NULL;

	k_mem_slab_alloc(&g_conn_slab, &conn, K_NO_WAIT);

	return conn;
}

/**
 * @brief Returns a serial connection to the static pool.
 * @param conn The connection to free.
 */
static inline void free_serial_connection(struct ludlc_serial_connection *conn)
{
	k_mem_slab_free(&g_conn_slab, conn);
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
#endif /* CONFIG_LUDLC_STATIC_SERIAL_CONN */

/* --- UART Callbacks --- */

/**
 * @brief UART Interrupt Request (IRQ) callback handler.
 *
 * This function is invoked by the Zephyr UART driver when an interrupt occurs.
 * It handles both transmit-ready and receive-ready events. For reception,
 * it reads bytes from the UART hardware and puts them into the `rx_fifo`
 * for the `ludlc_rx_serial_thread` to process.
 */
static void uart_irq_cb(const struct device *dev, void *user_data)
{
	struct ludlc_serial_connection *sconn = user_data;
	uint8_t c;
	struct rx_packet_item *rx_buf;

	if (!uart_irq_update(dev)) {
		return;
	}

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

	/* Read all available bytes */
	while (uart_fifo_read(dev, &c, 1) == 1) {
		struct rx_packet_item *next_buf = NULL;

		if (!ludlc_serial_decode(&sconn->conn,
						&sconn->dec_state, c)) {
			continue;
		}

		rx_buf->size = sconn->dec_state.size;
		/*
		 * Publish the next RX buffer before queuing the completed one.
		 */
		if (!k_mem_slab_alloc(&sconn->rx_slab, (void **)&next_buf,
				      K_NO_WAIT)) {
			ludlc_serial_decoder_prep(&sconn->conn,
						  &sconn->dec_state,
						  next_buf->payload,
						  sizeof(next_buf->payload));
		}

		atomic_ptr_set(&sconn->curr_rx_buf, next_buf);
		k_fifo_put(&sconn->rx_fifo, rx_buf);
		k_event_post(&sconn->conn.pconn.rx_event, RX_READY_EVT);

		rx_buf = next_buf;
		if (!next_buf) {
			LUDLC_INC_STATS(&sconn->conn, overrun);
			LUDLC_LOG_TRACE("RX-IRQ slab empty, disable RX IRQ");
			uart_irq_rx_disable(sconn->uart_dev);
			break;
		}
	}
}

/**
 * @brief RX serial thread entry point.
 *
 * This thread continuously waits for incoming serial packets from the `rx_fifo`.
 * Once a packet is received, it processes it using `ludlc_receive` and
 * frees the associated memory. It also handles enabling/disabling UART RX
 * based on buffer availability.
 * @param p1 Pointer to the `ludlc_serial_connection` structure.
 * @param p2 Unused.
 * @param p3 Unused.
 */
static void rx_serial_thread(void *p1, void *p2, void *p3)
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

		if (sconn->conn.proto->get_timestamp(&ts) == 0) {
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
			ludlc_serial_decoder_prep(
				&sconn->conn, &sconn->dec_state,
				rx_buf->payload,
				sizeof(rx_buf->payload));

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
 * This function attempts to start or continue transmitting data from the
 * transmit ring buffer using either asynchronous or interrupt-driven UART APIs.
 * @param sconn Pointer to the `ludlc_serial_connection` structure.
 */
static void serial_start_tx(struct ludlc_serial_connection *sconn)
{
	if (IS_ENABLED(CONFIG_UART_ASYNC_API) &&
	    ludlc_platform_test_bit(USE_ASYNC_API, &sconn->flags)) {
		uint8_t *data_ptr;
		uint32_t len;

		/* Atomic check on THIS connection instance */
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
 * @brief Asynchronous UART callback handler.
 *
 * This function is invoked by the Zephyr UART driver for asynchronous events.
 * It primarily handles `UART_TX_DONE` and `UART_TX_ABORTED` events to manage
 * the transmit ring buffer and signal the TX thread.
 * @param dev Pointer to the UART device.
 * @param evt Pointer to the UART event structure.
 * @param user_data Pointer to the `ludlc_serial_connection` structure.
 */
static void uart_async_callback(const struct device *dev,
		struct uart_event *evt, void *user_data)
{
	struct ludlc_serial_connection *sconn = user_data;

	if (evt->type == UART_TX_DONE) {

		/* Free memory in THIS connection's ring buffer */
		ring_buf_get_finish(&sconn->tx_rb, evt->data.tx.len);

		/* Unlock Busy Flag */
		ludlc_platform_clear_bit(TX_ASYNC_BUSY, &sconn->flags);

		/* Signal thread */
		k_event_post(&sconn->conn.pconn.tx_events, TX_SPACE_AVAIL_EVT);

		/* Re-trigger */
		serial_start_tx(sconn);
	} else if (evt->type == UART_TX_ABORTED) {
		ring_buf_get_finish(&sconn->tx_rb, evt->data.tx.len);
		ludlc_platform_clear_bit(TX_ASYNC_BUSY, &sconn->flags);
		/* Signal thread */
		ludlc_platform_request_tx(&sconn->conn);
		k_event_post(&sconn->conn.pconn.tx_events, TX_SPACE_AVAIL_EVT);
		serial_start_tx(sconn);
	}
}

/**
 * @brief TX serial thread entry point.
 *
 * This thread is responsible for encoding LuDLC packets and queuing them
 * for transmission via the UART. It waits for force-tx/exit events and does
 * not require UART/ring-buffer drain before local disconnect transition.
 * @param p1 Pointer to the `ludlc_serial_connection` structure.
 * @param p2 Unused.
 * @param p3 Unused.
 */
static void tx_serial_thread(void *p1, void *p2, void *p3)
{
	struct ludlc_serial_connection *sconn = p1;
	struct ludlc_senc_state enc_state;
	bool disconnect_after_tx = false;
	bool packet_done = true;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	LUDLC_LOG_DEBUG("Start the serial TX thread");

	ludlc_serial_encoder_init(&enc_state);

	while (sconn->conn.conn_state != LUDLC_STATE_TERMINATE) {
		/* Wait until:
		 * 1) We need to send a new packet (packet_done is true)
		 * 2) We are forced to send (e.g., for an ACK/PING)
		 * 3) The FIFO is not full
		 * 4) We are told to terminate
		 */
		uint32_t evt;
		uint32_t wait_mask = 0U;

		if (packet_done) {
			wait_mask = LUDLC_CONN_FORCE_TX_F |
				    LUDLC_CONN_TX_EXIT_EVT;
		}

		if (ring_buf_space_get(&sconn->tx_rb) == 0U) {
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

		if (ludlc_serial_encoder_idle(&enc_state)) {
			/* Encoder is ready for a new packet. Get one. */
			int is_tx_empty = ludlc_get_packet_to_send(
				&sconn->conn,
				&enc_state.hdr,
				&enc_state.hdr_size,
				&enc_state.payload,
				&enc_state.payload_size);

			if (is_tx_empty < 0) {
				ludlc_serial_encoder_init(&enc_state);
				continue;
			}

			disconnect_after_tx =
				ludlc_is_disconnect_after_tx_packet(
					&sconn->conn,
					enc_state.hdr, enc_state.hdr_size,
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
			packet_done = false;
		}

		/* Generate one octet from the encoder and queue it */
		if (ring_buf_space_get(&sconn->tx_rb)) {
			uint8_t byte = ludlc_serial_encode(&sconn->conn,
							   &enc_state);
			ring_buf_put(&sconn->tx_rb, &byte, 1);

			if (ludlc_serial_encoder_idle(&enc_state)) {
				/* The full packet has been encoded */
				LUDLC_INC_STATS(&sconn->conn, tx_packet);
				packet_done = true;

				/*
				 * Encoder has finished the disconnect packet so
				 * no more LuDLC bytes remain to be generated.
				 * Do not wait for UART HW/DMA drain.
				 * Disconnect now.
				 */
				if (disconnect_after_tx) {
					ludlc_handle_disconnect(&sconn->conn);
					ludlc_platform_clear_bit(
						LUDLC_CONN_DISC_AFTER_TX_F,
						&sconn->conn.flags);
					disconnect_after_tx = false;
				}
			}
		}

		serial_start_tx(sconn);
	}

	LUDLC_LOG_DEBUG("Exit from the serial TX thread");
}

/* --- Connection Management --- */
/**
 * @brief Destroys a LuDLC serial connection.
 *
 * This function cleans up all resources associated with a serial connection,
 * including stopping threads, disabling UART IRQs, and freeing memory.
 * @param conn Pointer to the core `ludlc_connection` structure.
 */
void ludlc_serial_connection_destroy(struct ludlc_connection *conn)
{
	struct ludlc_serial_connection *sconn =
		CONTAINER_OF(conn, struct ludlc_serial_connection, conn);

	conn->conn_state = LUDLC_STATE_TERMINATE;

	k_event_post(&conn->pconn.rx_event, RX_EXIT_EVT);
	k_event_post(&conn->pconn.tx_events, LUDLC_CONN_TX_EXIT_EVT);

	/* Disable UART IRQ */
	uart_irq_rx_disable(sconn->uart_dev);

	/* Unblock and join TX thread */
	if (sconn->tx_thread) {
		k_thread_join(sconn->tx_thread, K_FOREVER);
	}

	/* Unblock and join RX thread */
	if (sconn->rx_thread) {
		k_thread_join(sconn->rx_thread, K_FOREVER);
	}

	ludlc_connection_cleanup(&sconn->conn);
	free_serial_connection(sconn);
}

/**
 * @brief Creates and initializes a new LuDLC serial connection.
 *
 * This function allocates and configures a `ludlc_serial_connection` structure,
 * initializes Zephyr kernel objects, creates RX/TX threads, and sets up the UART.
 * @param arg Platform-specific arguments, including the UART device.
 * @param conn Output pointer to the newly created core `ludlc_connection`.
 * @param proto Pointer to protocol hooks (checksum and timestamp).
 * @param cb Callback structure for the LuDLC connection.
 * @return 0 on success, or a negative errno value on failure.
 */
int ludlc_serial_connection_create(const ludlc_platform_args_t *arg,
		   struct ludlc_connection **conn,
		   const struct ludlc_proto_cb *proto,
		   const struct ludlc_conn_cb *cb)
{
	struct ludlc_serial_connection *sconn = NULL;
	struct rx_packet_item *rx_buf = NULL;
	int ret = 0;

	if (!arg || !device_is_ready(arg->dev)) {
		LUDLC_LOG_ERROR("UART device not ready");
		return -ENODEV;
	}

	sconn = alloc_serial_connection();
	if (!sconn) {
		return -ENOMEM;
	}
	memset(sconn, 0, sizeof(*sconn));

	sconn->uart_dev = arg->dev;

	k_fifo_init(&sconn->rx_fifo);
	k_mem_slab_init(&sconn->rx_slab,
			sconn->rx_slab_bufs,
			RX_BUF_ALIGNED_SIZE,
			NUM_RX_BUFFERS);

	ludlc_serial_decoder_init(&sconn->dec_state);

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
				uart_irq_cb,
				(void *)sconn);
	}

	if (IS_ENABLED(CONFIG_UART_ASYNC_API) && ret) {
		ret = uart_callback_set(sconn->uart_dev, uart_async_callback,
				(void *)sconn);
		if (!ret) {
			ludlc_platform_set_bit(USE_ASYNC_API, &sconn->flags);
		}
	}

	if (ret) {
		goto err;
	}

	/* Initialize the core connection, passing sconn->conn as the user_arg */
	ret = ludlc_connection_init(&sconn->conn, proto, cb, &sconn->conn);
	if (ret) {
		goto err;
	}

	/*
	 * Seed decoder state only after connection init so checksum seed/verify
	 * values are valid (conn->csum_init_value is set by init path).
	 */
	ludlc_serial_decoder_prep(&sconn->conn, &sconn->dec_state,
				  rx_buf->payload, sizeof(rx_buf->payload));

	/* Create RX Thread */
	sconn->rx_thread = k_thread_create(&sconn->rx_thread_data,
			sconn->rx_thread_stack,
			sizeof(sconn->rx_thread_stack),
			rx_serial_thread,
			sconn, NULL, NULL,
			RX_THREAD_PRIO, 0, K_NO_WAIT);
	if (!sconn->rx_thread) {
		ret = -EFAULT;
		goto err;
	}

	k_thread_name_set(sconn->rx_thread, "LuDLC RX thread");

	/* Create TX Thread */
	sconn->tx_thread = k_thread_create(&sconn->tx_thread_data,
			sconn->tx_thread_stack,
			sizeof(sconn->tx_thread_stack),
			tx_serial_thread,
			sconn, NULL, NULL,
			TX_THREAD_PRIO, 0, K_NO_WAIT);
	if (!sconn->tx_thread) {
		ret = -EFAULT;
		goto err;
	}

	k_thread_name_set(sconn->tx_thread, "LuDLC TX thread");

	/* Setup and enable UART rx */
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

/**
 * @brief Provides default platform arguments for a serial connection.
 *
 * This function populates a `ludlc_platform_args_t` structure with default
 * values, typically pointing to `uart0`.
 * @param args Pointer to the `ludlc_platform_args_t` structure to populate.
 */
void ludlc_default_serial_platform_args(ludlc_platform_args_t *args)
{
	static const ludlc_platform_args_t def_args = {
		.dev = DEVICE_DT_GET(DT_NODELABEL(uart0)),
	};

	if (args) {
		*args = def_args;
	}
}

// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file ludlc_serial.c
 *
 * @brief LuDLC generic Multi-Threaded serial transport implementation.
 *
 * This file provides the RX and TX thread implementations for running
 * the LuDLC protocol over a serial (byte-stream) transport. It uses
 * the serial encoder/decoder from `ludlc_serial_enc_impl.h` to handle
 * framing and escaping.
 *
 * Copyright (C) 2025 Andrey VOLKOV <andrey@volkov.fr> and LuDLC Contributors
 *
 * This file is licensed under either the Apache License, Version 2.0,
 * or the GNU General Public License, version 2 or (at your option)
 * any later version.
 */

#include <errno.h>

#include <ludlc.h>
#include <ludlc_private.h>
#include <ludlc_serial.h>
#include <ludlc_serial_enc_impl.h>

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
void *ludlc_rx_serial_thread(void *arg)
{
	struct ludlc_connection *conn = arg;
	uint32_t timestamp;
	int i;
	struct ludlc_sdec_state dec_state;

	LUDLC_LOG_DEBUG("Start serial RX thread");

	ludlc_serial_decoder_init(&dec_state);

	if (conn->proto->rx_start)
		conn->proto->rx_start(conn);

	while (conn->conn_state != LUDLC_STATE_TERMINATE) {
		int c;
		/* Read one byte at a time, blocking indefinitely (-1) */
		ssize_t ret = conn->proto->rx_read(conn, &c, 1, -1);
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

	if (conn->proto->rx_stop)
		conn->proto->rx_stop(conn);

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
static
int serial_start_tx(struct ludlc_connection *conn)
{
	while (kfifo_len(&conn->tx_fifo)) {
		uint8_t c;
		if (kfifo_get(&conn->tx_fifo, &c)) {
			ssize_t ret = conn->proto->tx_write(conn, &c, 1);
			if (ret < 0)
				return ret; /* Transport write error */
		}
	}
	return 0;
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
 */void *ludlc_tx_serial_thread(void *arg)
{
	struct ludlc_connection *conn = arg;
	struct ludlc_senc_state enc_state;
	bool packet_sent = true, is_dead = false;

	LUDLC_LOG_DEBUG("Start serial TX thread");

	ludlc_serial_encoder_init(&enc_state);

	if (conn->proto->tx_start)
		conn->proto->tx_start(conn);

	LUDLC_WQ_LOCK(conn->tx_wq);

	while(!is_dead) {
		/* Wait until:
		 * 1) We need to send a new packet (packet_sent is true)
		 * 2) We are forced to send (e.g., for an ACK/PING)
		 * 3) The FIFO is not full
		 * 4) We are told to terminate
		 */
		LUDLC_WQ_WAIT_EVENT(conn->tx_wq,
			((!packet_sent ||
				ludlc_platform_test_bit(LUDLC_CONN_FORCE_TX_F,
						&conn->flags)) &&
					!kfifo_is_full(&conn->tx_fifo)) ||
			conn->conn_state == LUDLC_STATE_TERMINATE);

		if(conn->conn_state == LUDLC_STATE_TERMINATE)
			break;

		if (ludlc_serial_encoder_idle(&enc_state)) {
			/* Encoder is ready for a new packet. Get one. */
			int is_tx_empty = ludlc_get_packet_to_send(conn,
				&enc_state.hdr,
				&enc_state.hdr_size,
				&enc_state.payload,
				&enc_state.payload_size);

			if(is_tx_empty < 0) {
				/* TODO: Error getting packet */
				is_dead  = true;
				continue;
			}
			/* Postpone the ping timer for one PING_TIME later */
			ludlc_platform_start_timer(&conn->ping_timer,
					CONFIG_LUDLC_PING_TIME,
					CONFIG_LUDLC_PING_TIME);
			if (is_tx_empty) {
				/* We are sending a PING or control packet */
				ludlc_platform_clear_bit(LUDLC_CONN_FORCE_TX_F,
						&conn->flags);

			} else {
				/* We are sending data, set flag to check
				 * for more */
				ludlc_platform_test_bit(LUDLC_CONN_FORCE_TX_F,
						&conn->flags);
			}

			/* Send EOF marker only if queue is empty */
			ludlc_serial_encoder_send_eof(&enc_state,
					is_tx_empty != 0);
			packet_sent = false;
		}

		/* Generate one octet from the encoder and queue it */
		kfifo_put(&conn->tx_fifo,
				ludlc_serial_encode(conn, &enc_state));
		if (ludlc_serial_encoder_idle(&enc_state)) {
			/* The full packet has been encoded */
			LUDLC_INC_STATS(conn, tx_packet);
			packet_sent = true;
		}

		/* Drain the FIFO to the hardware */
		serial_start_tx(conn);
	}

	LUDLC_WQ_UNLOCK(conn->tx_wq);

	if (conn->proto->tx_stop)
		conn->proto->tx_stop(conn);

	LUDLC_LOG_DEBUG("Exit form serial TX thread");

	return NULL;
}

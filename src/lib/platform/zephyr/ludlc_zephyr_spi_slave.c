// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file ludlc_zephyr_spi_slave.c
 *
 * @brief LuDLC SPI peripheral transport implementation for Zephyr.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * The wire format is in ludlc_spi_proto.h and the transport's contract in
 * ludlc_spi_slave.h. One thread does everything here, unlike the serial
 * transport's two: SPI has one wire and one clock, both driven by the
 * controller, and the two directions are the two halves of the same transfer,
 * so there is no second thing to wait on. That thread announces, transfers,
 * decodes and passes the packet to the core, and it blocks inside a transfer
 * instead of sleeping. The cost is that the core's callbacks run between
 * transfers, while this end is not armed: a slow callback is one the
 * controller may clock into nothing. See ludlc_spi_slave.h.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/time_units.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>

#include <ludlc.h>
#include <ludlc_platform.h>
#include <ludlc_private.h>
#include <ludlc_serial.h>
#include <ludlc_framing.h>
#include <ludlc_spi_proto.h>
#include <ludlc_spi_slave.h>
#include <ludlc_logger.h>

/*
 * Stack for the exchange thread: the core's callbacks run here, and TX/RX
 * staging is one small chunk rather than a whole encoded frame. What the
 * link may carry is @c max_payload on the connection, not this stack.
 */
#define XFER_THREAD_STACK_SIZE	1536
#define XFER_THREAD_PRIO	7

/** @brief How long the shutdown waits for the DISCONNECT to go out, in ms. */
#define LUDLC_DISCONNECT_WAIT_MS	50U

/**
 * @def LUDLC_XFER_JOIN_MS
 * @brief How long the shutdown waits for the exchange thread to exit.
 *
 * The thread exits when its current transfer completes, which requires the
 * controller to clock it. Long enough for a controller that is still running,
 * short enough not to block the caller when it is not. See
 * ludlc_spi_slave_connection_destroy() for what happens on expiry.
 */
#define LUDLC_XFER_JOIN_MS	500U

/** @brief Zephyr-specific SPI peripheral connection structure. */
struct ludlc_spi_connection {
	K_KERNEL_STACK_MEMBER(xfer_thread_stack, XFER_THREAD_STACK_SIZE);

	const struct device *spi_dev;
	struct spi_config cfg;

	/** @brief Line this end raises to ask for an exchange, if any. */
	struct gpio_dt_spec data_ready;

	k_tid_t xfer_thread;
	struct k_thread xfer_thread_data;

	/* The announcements of one exchange, LUDLC_SPI_HDR_LEN each way. */
	struct ludlc_spi_hdr	tx_hdr;
	struct ludlc_spi_hdr	rx_hdr;

	/* Pipelined encode/decode staging. */
	uint8_t			tx_chunk[LUDLC_TX_FIFO_SIZE / 2];
	uint8_t			rx_chunk[LUDLC_TX_FIFO_SIZE / 2];
	uint8_t			rx_pkt[LUDLC_FRAME_BUF_SIZE];

	struct ludlc_connection	conn;

	const struct ludlc_csum *csum;

	/** Framing state of this link, allocated at open. The exchange
	 * thread drives both state machines.
	 */
	struct ludlc_framing_state *fs;

	ludlc_frame_size_t	frame_cap;
};

#ifdef CONFIG_LUDLC_STATIC_SPI_CONN
static struct ludlc_spi_connection g_conn[CONFIG_LUDLC_STATIC_SPI_CONN_NUM];
static LUDLC_DECLARE_BITSET(g_conn_busy, CONFIG_LUDLC_STATIC_SPI_CONN_NUM);

/**
 * @brief Takes an SPI connection from the static pool.
 * @return Pointer to a free zeroed connection, or NULL if the pool is full.
 */
static inline struct ludlc_spi_connection *alloc_spi_connection(void)
{
	const int slot = ludlc_bitset_claim(g_conn_busy,
					    CONFIG_LUDLC_STATIC_SPI_CONN_NUM);

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
 * @brief Returns an SPI connection to the static pool.
 * @param conn The connection to release (must be from @ref g_conn).
 */
static inline void free_spi_connection(struct ludlc_spi_connection *conn)
{
	ludlc_platform_clear_bit(conn - g_conn, g_conn_busy);
}
#else
/**
 * @brief Allocates a new SPI connection on the heap.
 * @return Pointer to a zeroed connection, or NULL on failure.
 */
static inline struct ludlc_spi_connection *alloc_spi_connection(void)
{
	return ludlc_platform_alloc(sizeof(struct ludlc_spi_connection));
}

/**
 * @brief Frees an SPI connection using platform's free.
 * @param conn The connection structure to free.
 */
static inline void free_spi_connection(struct ludlc_spi_connection *conn)
{
	ludlc_platform_free(conn);
}
#endif /* CONFIG_LUDLC_STATIC_SPI_CONN */

/* --- The wire --- */

/**
 * @brief Drive the data-ready line, where the board has one.
 *
 * @param sconn The link.
 * @param on Whether the core has asked for a TX round that has not happened.
 */
static void ludlc_spi_data_ready(struct ludlc_spi_connection *sconn, bool on)
{
	if (sconn->data_ready.port) {
		gpio_pin_set_dt(&sconn->data_ready, on);
	}
}

/**
 * @brief Raise the data-ready line on the core's behalf.
 *
 * Registered as @c pconn.start_tx, so it runs in the context of whatever made
 * the request: a timer callback, an enqueue, a DISCONNECT on the way out. The
 * exchange thread cannot do this itself because it is blocked inside a
 * transfer, and waiting for it would mean waiting on the controller, which is
 * what this line asks for.
 *
 * @param conn The connection the core is asking about.
 */
static void ludlc_spi_start_tx(struct ludlc_connection *conn)
{
	struct ludlc_spi_connection *sconn =
		CONTAINER_OF(conn, struct ludlc_spi_connection, conn);

	ludlc_spi_data_ready(sconn, true);
}

/**
 * @brief Clock @p len octets each way and wait for the controller to do it.
 *
 * @param sconn The link.
 * @param tx What to send, @p len octets of it.
 * @param rx Where what arrives goes, @p len octets of room.
 * @param len How many octets the exchange agreed on.
 * @return 0 once the transfer has happened, or a negative errno.
 */
static int ludlc_spi_move(struct ludlc_spi_connection *sconn, const void *tx,
			  void *rx, size_t len)
{
	const struct spi_buf tx_buf = { .buf = (void *)tx, .len = len };
	const struct spi_buf rx_buf = { .buf = rx, .len = len };
	const struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1 };
	const struct spi_buf_set rx_set = { .buffers = &rx_buf, .count = 1 };
	int ret;

	/*
	 * Blocks until the controller clocks the transfer, for as long as that
	 * takes: giving up would leave the driver holding these buffers with
	 * no way to reclaim them. Zephyr's SPI API has no abort, only
	 * spi_release(), which drops the bus lock and not the transfer.
	 */
	ret = spi_transceive(sconn->spi_dev, &sconn->cfg, &tx_set, &rx_set);

	/*
	 * In peripheral mode the return value counts the frames received,
	 * which this end cannot act on: a transfer the controller cut short
	 * leaves a frame the framing or the checksum rejects, and an
	 * over-clocked one leaves padding. Only the sign matters.
	 */
	return ret < 0 ? ret : 0;
}

static void ludlc_spi_on_packet(void *ctx, void *pkt, size_t size)
{
	struct ludlc_spi_connection *sconn = ctx;
	const ludlc_ping_packet_t *pid;
	ludlc_timestamp_t ts;

	(void)pkt;

	if (sconn->conn.cfg.get_timestamp(&ts)) {
		LUDLC_LOG_ERROR("Can't get the timestamp, drop the packet");
		return;
	}

	pid = (const ludlc_ping_packet_t *)sconn->rx_pkt;
	LUDLC_LOG_TRACE("<< id=0x%02x ack=0x%02x len=%u",
			(unsigned int)pid->tx_id, (unsigned int)pid->ack_id,
			(unsigned int)size);
	ludlc_receive(&sconn->conn, (ludlc_packet_t *)sconn->rx_pkt,
		      (ludlc_payload_size_t)size, ts);
}

static ssize_t ludlc_spi_load_tx(struct ludlc_spi_connection *sconn)
{
	const void *hdr, *payload;
	ludlc_payload_size_t hdr_sz, pay_sz;
	ssize_t len;
	int pending;

	ludlc_spi_data_ready(sconn, false);

	if (!k_event_wait_safe(&sconn->conn.pconn.tx_events,
			       LUDLC_CONN_FORCE_TX_F, false, K_NO_WAIT)) {
		return 0;
	}

	pending = ludlc_get_packet_to_send(&sconn->conn, &hdr, &hdr_sz,
					  &payload, &pay_sz);
	if (pending < 0) {
		return 0;
	}

	/*
	 * The length is announced before the frame is encoded, so it is
	 * computed from the packet rather than from the encoder.
	 */
	len = LUDLC_FS_FRAMING(sconn->fs)->wire_len(sconn->csum, hdr, hdr_sz,
						    payload, pay_sz);
	if (len < 0 || (ludlc_frame_size_t)len > sconn->frame_cap) {
		LUDLC_LOG_ERROR("Cannot announce a packet of %u+%u octets",
				(unsigned int)hdr_sz, (unsigned int)pay_sz);
		LUDLC_INC_STATS(&sconn->conn, dropped);
		return 0;
	}

	ludlc_enc_prep(sconn->fs, hdr, hdr_sz, payload, pay_sz, false);

	ludlc_spi_data_ready(sconn, true);

	if (!pending) {
		ludlc_platform_request_tx(&sconn->conn);
	}

	return len;
}

/**
 * @brief Fill the chunk this exchange is about to clock out.
 *
 * A frame longer than the chunk continues into the next one. Past its end the
 * chunk is padded, the length having gone out in the header so that the
 * controller knows where to stop.
 */
static void ludlc_spi_fill_tx(struct ludlc_spi_connection *sconn, size_t want)
{
	size_t n = ludlc_enc_fill(sconn->fs, sconn->csum, sconn->tx_chunk,
				  want);

	if (n < want) {
		memset(sconn->tx_chunk + n, LUDLC_SPI_PAD, want - n);
	}
}

static int ludlc_spi_exchange(struct ludlc_spi_connection *sconn,
			      ssize_t *tx_len)
{
	ssize_t rx_len;
	size_t len, sent, recvd;
	int ret;

	ludlc_spi_hdr_put(&sconn->tx_hdr, (uint32_t)*tx_len);
	memset(&sconn->rx_hdr, 0, sizeof(sconn->rx_hdr));

	ret = ludlc_spi_move(sconn, &sconn->tx_hdr, &sconn->rx_hdr,
			     LUDLC_SPI_HDR_LEN);
	if (ret) {
		return ret;
	}

	rx_len = ludlc_spi_hdr_get(&sconn->rx_hdr, sconn->frame_cap);
	if (rx_len < 0) {
		LUDLC_INC_STATS(&sconn->conn, dropped);
		rx_len = 0;
	}

	if (!*tx_len && !rx_len) {
		return 0;
	}

	len = MAX((size_t)*tx_len, (size_t)rx_len);
	/*
	 * What the controller announced delimits a raw frame, and was checked
	 * against frame_cap above. COBS may announce more than it decodes to,
	 * hence the clamp.
	 */
	ludlc_dec_prep(sconn->fs, sconn->csum, sconn->rx_pkt,
		       MIN(ludlc_conn_frame_sz(&sconn->conn, sconn->csum),
			   (ludlc_frame_size_t)rx_len));

	for (sent = 0, recvd = 0; sent < len; ) {
		size_t chunk = MIN(len - sent, (size_t)(LUDLC_TX_FIFO_SIZE / 2));
		size_t rx_feed;

		ludlc_spi_fill_tx(sconn, chunk);
		ret = ludlc_spi_move(sconn, sconn->tx_chunk, sconn->rx_chunk,
				     chunk);
		if (ret) {
			ludlc_enc_init(sconn->fs);
			return ret;
		}

		if (recvd < (size_t)rx_len) {
			size_t i = 0;

			rx_feed = MIN(chunk, (size_t)rx_len - recvd);
			while (i < rx_feed) {
				bool done;

				i += ludlc_dec_feed(sconn->fs, sconn->csum,
						    &sconn->conn,
						    &sconn->rx_chunk[i],
						    rx_feed - i, &done);
				if (done)
					ludlc_spi_on_packet(
						sconn, sconn->rx_pkt,
						ludlc_dec_size(sconn->fs));
			}
			recvd += rx_feed;
		}
		sent += chunk;
	}

	if (*tx_len) {
		*tx_len = 0;
		LUDLC_INC_STATS(&sconn->conn, tx_packet);
	}

	return 0;
}

/**
 * @brief Exchange thread entry point: the whole of this transport.
 *
 * Blocks inside a transfer nearly all of the time, because that is what being
 * able to receive means here. Between transfers it loads whatever the core
 * has queued and delivers whatever arrived.
 *
 * @param p1 Pointer to the `ludlc_spi_connection` structure.
 * @param p2 Unused.
 * @param p3 Unused.
 */
static void ludlc_spi_xfer_thread(void *p1, void *p2, void *p3)
{
	struct ludlc_spi_connection *sconn = p1;
	ssize_t tx_len = 0;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	LUDLC_LOG_DEBUG("Start the SPI exchange thread");

	while (sconn->conn.conn_state != LUDLC_STATE_TERMINATE) {
		int ret;

		if (!tx_len) {
			tx_len = ludlc_spi_load_tx(sconn);
		}

		ret = ludlc_spi_exchange(sconn, &tx_len);
		if (ret) {
			/*
			 * The SPI driver failed, not the controller. Nothing
			 * can be sent or received over a bus in that state, so
			 * end the session rather than keep a link that only
			 * looks like one.
			 */
			LUDLC_LOG_ERROR("SPI exchange failed: %d", ret);
			ludlc_handle_disconnect(&sconn->conn);
			break;
		}

		/*
		 * The core timed the link out. Handled here rather than in the
		 * timer callback because this is the only point where the
		 * transport is between transfers, which also means it is only
		 * noticed once the controller clocks something.
		 */
		if (k_event_wait_safe(&sconn->conn.pconn.rx_event,
				      LUDLC_CONN_RX_TOUT_EVT, false,
				      K_NO_WAIT)) {
			ludlc_handle_disconnect(&sconn->conn);
		}
	}

	ludlc_spi_data_ready(sconn, false);

	LUDLC_LOG_DEBUG("Exit from the SPI exchange thread");
}

/* --- Connection Management --- */

void ludlc_spi_slave_connection_destroy(struct ludlc_connection *conn)
{
	struct ludlc_spi_connection *sconn =
		CONTAINER_OF(conn, struct ludlc_spi_connection, conn);

	/*
	 * Send a DISCONNECT before going, so that the controller need not wait
	 * out its watchdog. Queueing it raises the data-ready line, which is
	 * what makes the controller clock the exchange that carries it, and
	 * that same exchange is what releases the thread from its transfer.
	 * The core takes the link down once the frame has drained, which is
	 * what the bounded wait below is for.
	 */
	if (ludlc_send_disconnect(conn) == 0) {
		(void)WAIT_FOR(conn->conn_state != LUDLC_STATE_CONNECTED,
			       LUDLC_DISCONNECT_WAIT_MS * USEC_PER_MSEC,
			       k_msleep(1));
	}

	conn->conn_state = LUDLC_STATE_TERMINATE;

	/*
	 * The join waits on the controller, which may never clock again. The
	 * pending transfer is armed over buffers inside sconn and the SPI
	 * driver writes to them whenever the bus next moves, so freeing sconn
	 * here would corrupt whatever the allocator returns next. The thread
	 * cannot free sconn on its way out either, its stack being part of it.
	 * Leaking is the least bad option.
	 */
	if (sconn->xfer_thread &&
	    k_thread_join(sconn->xfer_thread, K_MSEC(LUDLC_XFER_JOIN_MS))) {
		LUDLC_LOG_ERROR("The controller stopped clocking mid-exchange: "
				"still armed after %u ms, leaking the link",
				LUDLC_XFER_JOIN_MS);

		return;
	}

	ludlc_connection_cleanup(&sconn->conn);
	ludlc_framing_destroy(sconn->fs);
	free_spi_connection(sconn);
}

int ludlc_spi_slave_connection_create(const ludlc_platform_args_t *args,
				      struct ludlc_connection **conn,
				      const struct ludlc_conn_cb *cb)
{
	struct ludlc_spi_connection *sconn = NULL;
	struct ludlc_conn_cfg cfg;
	int ret;

	if (!args || !device_is_ready(args->dev)) {
		LUDLC_LOG_ERROR("SPI device not ready");
		return -ENODEV;
	}

	cfg = *ludlc_args_conn_cfg(args);

	/*
	 * A link may run no checksum, but one it runs must be complete and fit
	 * the register it is computed in.
	 */
	if (args->csum && (!args->csum->csum || !args->csum->octets ||
			   args->csum->octets > LUDLC_MAX_CSUM_SIZE)) {
		LUDLC_LOG_ERROR("Unusable checksum for the link");
		return -EINVAL;
	}

	/*
	 * The exchange announces a frame before encoding it, so a framing
	 * whose encoded length is only known after escaping cannot be carried
	 * here. Any other framing can: the announcement delimits the frame.
	 */
	if (args->framing && !args->framing->wire_len) {
		LUDLC_LOG_ERROR("%s framing cannot be carried over SPI: its "
				"length is not known before it is sent",
				args->framing->name);
		return -EINVAL;
	}

	sconn = alloc_spi_connection();
	if (!sconn) {
		return -ENOMEM;
	}

	sconn->spi_dev = args->dev;
	sconn->csum = args->csum;
	sconn->data_ready = args->spi.data_ready;

	/*
	 * Mode 0, eight bits, most significant first: the default at both ends
	 * of this bus, and what the Linux controller uses unless its
	 * devicetree overrides it. The format does not depend on the choice,
	 * but both ends must make the same one.
	 */
	sconn->cfg.operation = SPI_OP_MODE_SLAVE | SPI_WORD_SET(8) |
			       SPI_TRANSFER_MSB;
	sconn->cfg.slave = args->spi.slave;

	if (sconn->data_ready.port) {
		ret = gpio_pin_configure_dt(&sconn->data_ready,
					    GPIO_OUTPUT_INACTIVE);
		if (ret) {
			LUDLC_LOG_ERROR("Can't drive the data-ready line: %d",
					ret);
			goto err_free;
		}
	}

	ret = ludlc_connection_init(&sconn->conn, &cfg, cb, &sconn->conn);
	if (ret) {
		LUDLC_LOG_ERROR("Can't open a link of %u payload octets: %d",
				(unsigned int)ludlc_cfg_max_payload(&cfg), ret);
		goto err_free;
	}

	/*
	 * Not ludlc_framing_or_default(): a link over a byte stream falls back
	 * on a delimiting framing because it has no other way to end a frame.
	 * Every exchange here announces its frame length, so the frame goes
	 * out unescaped.
	 */
	sconn->fs = ludlc_framing_create(args->framing ? args->framing :
							 &ludlc_framing_raw);
	if (!sconn->fs) {
		ret = -ENOMEM;
		goto err_destroy;
	}

	sconn->frame_cap =
		LUDLC_FS_FRAMING(sconn->fs)->encoded_max(&sconn->conn,
							 sconn->csum);

	ludlc_enc_init(sconn->fs);
	ludlc_dec_init(sconn->fs);
	/* Nothing is expected until an exchange announces a frame. */
	ludlc_dec_prep(sconn->fs, sconn->csum, sconn->rx_pkt, 0);

	/* Set before anything can ask for a TX round, handshake included. */
	sconn->conn.pconn.start_tx = ludlc_spi_start_tx;

	/*
	 * Started last, once everything it touches is in place: it goes onto
	 * the wire as soon as it runs, and from then on the link can only be
	 * taken down with the controller's help.
	 */
	sconn->xfer_thread = k_thread_create(&sconn->xfer_thread_data,
			sconn->xfer_thread_stack,
			sizeof(sconn->xfer_thread_stack),
			ludlc_spi_xfer_thread, sconn, NULL, NULL,
			XFER_THREAD_PRIO, 0, K_NO_WAIT);
	if (!sconn->xfer_thread) {
		ret = -EFAULT;
		goto err_destroy;
	}

	k_thread_name_set(sconn->xfer_thread, "LuDLC SPI thread");

	*conn = &sconn->conn;

	/*
	 * Queues the handshake and raises the data-ready line for it. The
	 * controller has to clock the exchange that carries it, so the link
	 * comes up only when the controller asks.
	 */
	ret = ludlc_connect(&sconn->conn);
	if (!ret) {
		return 0;
	}

err_destroy:
	ludlc_spi_slave_connection_destroy(&sconn->conn);
	*conn = NULL;

	return ret;

err_free:
	free_spi_connection(sconn);

	return ret;
}

/*
 * CRC-16/KERMIT, which the other LuDLC platforms send and a Linux controller
 * runs by default under this name.
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

void ludlc_default_spi_slave_platform_args(ludlc_platform_args_t *args)
{
	static const ludlc_platform_args_t def_args = {
		.dev = DEVICE_DT_GET(DT_NODELABEL(spi1)),
		.csum = &ludlc_crc16_ccitt_pr0,
	};

	if (args) {
		*args = def_args;
	}
}

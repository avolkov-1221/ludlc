// SPDX-License-Identifier: GPL-2.0-only
/**
 * @file ludlc_spi_master_drv.c
 *
 * @brief LuDLC transport over an SPI bus this system drives.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * One link per peripheral: the timings and the checksum come from firmware,
 * the framing and the core connection are kept here, and the peripheral
 * represents the local end of the link on the LuDLC bus.
 *
 * The wire format - what an exchange looks like and what firmware on the other
 * side has to implement - is written down in ludlc_spi_master_drv.h.
 */

#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/minmax.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/pm.h>
#include <linux/property.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/workqueue.h>

#include "ludlc_bus.h"
#include "ludlc_csum.h"
#include "ludlc_framing.h"
#include "ludlc_spi_master_drv.h"

/*
 * What a peer is given to point its controller at a buffer for the second
 * transfer, when firmware does not say. Generous for an interrupt handler on a
 * small microcontroller, and paid only once per exchange.
 */
#define LUDLC_SPI_DFLT_TURNAROUND_US	50

/*
 * How often to ask a peer that has no way of asking us, when firmware does not
 * say. Only links without a data-ready line pay this; it is a compromise
 * between the latency of a frame the peer cannot announce and a bus that is
 * busy for no reason.
 */
#define LUDLC_SPI_DFLT_POLL_MS		10

/*
 * One framing for the whole network is the normal case, and this overrides the
 * one a link would otherwise have, for every link that does not name its own.
 * An unknown name fails the module load instead of leaving every link talking
 * past its peer.
 */
static char *ludlc_framing_param;
module_param_named(framing, ludlc_framing_param, charp, 0444);
MODULE_PARM_DESC(framing,
	"frame delimiting every link uses unless firmware or "
	"configfs says otherwise: " LUDLC_FRAMING_NAMES_ANNOUNCED);

/*
 * Every exchange says how long its frame is, so a link that names no framing
 * needs nothing in the frame to mark its end. A peer that speaks a framing
 * which delimits must be named, not guessed at.
 */
static
const struct ludlc_framing *ludlc_spi_framing_dflt = &ludlc_framing_raw;

/*
 * As for the framing: one payload size for the whole network is the normal
 * case. Zero leaves every link on what the build allows.
 */
static unsigned int ludlc_payload_param;
module_param_named(max_payload, ludlc_payload_param, uint, 0444);
MODULE_PARM_DESC(max_payload,
	"octets of payload every link carries unless "
	"firmware or configfs says otherwise");

/*
 * Exchanges one run of the worker will do before yielding. Traffic arrives in
 * bursts and draining one is cheap, but a peer announcing frames it then fails
 * to send would otherwise hold the worker indefinitely. What is left is picked
 * up by the requeue.
 */
#define LUDLC_SPI_BUDGET		8

/**
 * @brief Is talking to the peripheral forbidden right now?
 *
 * Either the link is going away, in which case the core timers are gone, there
 * is nothing left to send and nothing may arm a timer that outlives the link,
 * or the system is suspended and the bus is not ours until it resumes. Unlike
 * a UART, nothing arrives unasked here, so this covers both directions: an
 * exchange that is not started is an exchange that does not happen.
 */
static inline bool ludlc_spi_quiesced(struct ludlc_spi_connection *lsc)
{
	return test_bit(LUDLC_CONN_SUSPENDED_F, &lsc->conn.pconn.flags) ||
	       READ_ONCE(lsc->conn.conn_state) == LUDLC_STATE_TERMINATE;
}

/**
 * @brief Run an exchange where it may block.
 *
 * The ping timer and the core both call in from contexts that must not wait on
 * the bus. The one caller that could do the work itself is the exchange loop
 * asking for another round, and it needs no help: it re-checks the request bit
 * before it gives up.
 */
static void ludlc_spi_schedule_xfer(struct ludlc_connection *conn)
{
	struct ludlc_spi_connection *lsc = to_spi_conn(conn);

	if (current_work() != &lsc->xfer_work.work)
		mod_delayed_work(system_wq, &lsc->xfer_work, 0);
}

/**
 * @brief Whether the caller is in a context this link runs in.
 *
 * What ludlc_in_callback() asks the transport: the exchange work, or the
 * thread of the data-ready handler once it has run.
 */
static bool ludlc_spi_in_callback(struct ludlc_connection *conn)
{
	struct ludlc_spi_connection *lsc = to_spi_conn(conn);
	struct work_struct *work = current_work();

	if (work)
		return work == &lsc->xfer_work.work;

	return current == READ_ONCE(lsc->irq_task);
}

static void ludlc_spi_on_packet(void *ctx, void *pkt, size_t size)
{
	struct ludlc_spi_connection *lsc = ctx;
	ludlc_timestamp_t ts;

	(void)pkt;

	if (lsc->conn.cfg.get_timestamp(&ts)) {
		LUDLC_LOG_ERROR("Can't get the timestamp, drop the packet");
		return;
	}

	ludlc_receive(&lsc->conn, (ludlc_packet_t *)lsc->rx_pkt,
		      (ludlc_payload_size_t)size, ts);
}

/**
 * @brief Take the next core packet, ready for the framing to write out.
 *
 * @return Exact wire length, to be announced, or 0 with nothing to send.
 */
static ssize_t ludlc_spi_load_tx(struct ludlc_spi_connection *lsc)
{
	const void *hdr, *payload;
	ludlc_payload_size_t hdr_sz, pay_sz;
	ssize_t len;
	int pending;

	/*
	 * Only if the core asked for a TX round. Unlike a byte stream there is
	 * no idle traffic to fall back on: with nothing to send, the exchange
	 * either carries the peer's frame or does not happen.
	 */
	if (!test_and_clear_bit(LUDLC_CONN_FORCE_TX_F, &lsc->conn.pconn.flags))
		return 0;

	pending = ludlc_get_packet_to_send(&lsc->conn, &hdr, &hdr_sz,
					  &payload, &pay_sz);
	if (pending < 0)
		return 0;

	ludlc_enc_prep(lsc->fs, hdr, hdr_sz, payload, pay_sz, false);

	/*
	 * The length goes out before the frame does, so it is computed from the
	 * packet rather than from an encoder part way through it.
	 */
	len = LUDLC_FS_FRAMING(lsc->fs)->wire_len(lsc->csum, hdr, hdr_sz,
						  payload, pay_sz);
	if (len < 0 || (ludlc_frame_size_t)len > lsc->frame_cap) {
		dev_err_ratelimited(&lsc->spi->dev,
				    "cannot announce a packet of %u+%u octets\n",
				    (unsigned int)hdr_sz, (unsigned int)pay_sz);
		LUDLC_INC_STATS(&lsc->conn, dropped);
		return 0;
	}

	if (!pending)
		ludlc_platform_request_tx(&lsc->conn);

	return len;
}

/** @brief Clock @p len octets each way, one chip select. */
static int ludlc_spi_move(struct ludlc_spi_connection *lsc,
			  const void *tx, void *rx, size_t len)
{
	struct spi_transfer xfer = {
		.tx_buf = tx,
		.rx_buf = rx,
		.len = len,
	};

	return spi_sync_transfer(lsc->spi, &xfer, 1);
}

/**
 * @brief Fill the TX chunk with up to @p want encoded octets, padding the
 * rest.
 *
 * A frame longer than one chunk continues into the next. Past its end the
 * chunk is padded, the length having gone out ahead of the frame so that the
 * peer knows where to stop.
 */
static void ludlc_spi_fill_tx(struct ludlc_spi_connection *lsc,
			      const struct ludlc_csum *csum, size_t want)
{
	size_t n = ludlc_enc_fill(lsc->fs, csum, lsc->tx_chunk, want);

	if (n < want)
		memset(lsc->tx_chunk + n, LUDLC_SPI_PAD, want - n);
}

/**
 * @brief One exchange: announce, then pipeline the frame in half-fifo chunks.
 */
static bool ludlc_spi_exchange(struct ludlc_spi_connection *lsc)
{
	const struct ludlc_csum *csum = lsc->csum;
	ssize_t tx_len, rx_len;
	size_t len, sent, recvd;
	int ret;

	tx_len = ludlc_spi_load_tx(lsc);

	ludlc_spi_hdr_put(&lsc->tx_hdr, (uint32_t)tx_len);

	ret = ludlc_spi_move(lsc, &lsc->tx_hdr, &lsc->rx_hdr,
			     LUDLC_SPI_HDR_LEN);
	if (ret) {
		dev_err_ratelimited(&lsc->spi->dev,
				    "announcement failed: %d\n", ret);
		ludlc_handle_disconnect(&lsc->conn);
		ludlc_enc_init(lsc->fs);

		return false;
	}

	rx_len = ludlc_spi_hdr_get(&lsc->rx_hdr, lsc->frame_cap);
	if (rx_len < 0) {
		LUDLC_INC_STATS(&lsc->conn, dropped);
		rx_len = 0;
	}

	if (!tx_len && !rx_len)
		return false;

	len = max_t(size_t, tx_len, rx_len);
	/*
	 * What the peer announced delimits a raw frame, and was checked against
	 * frame_cap above. COBS may announce more than it decodes to, hence the
	 * clamp.
	 */
	ludlc_dec_prep(lsc->fs, csum, lsc->rx_pkt,
		       min_t(ludlc_frame_size_t,
			     ludlc_conn_frame_sz(&lsc->conn, csum), rx_len));

	fsleep(lsc->turnaround_us);

	for (sent = 0, recvd = 0; sent < len; ) {
		size_t chunk = min(len - sent, (size_t)LUDLC_SPI_CHUNK);
		size_t rx_feed;

		ludlc_spi_fill_tx(lsc, csum, chunk);

		ret = ludlc_spi_move(lsc, lsc->tx_chunk, lsc->rx_chunk, chunk);
		if (ret) {
			dev_err_ratelimited(&lsc->spi->dev,
					    "frame transfer failed: %d\n", ret);
			ludlc_handle_disconnect(&lsc->conn);
			ludlc_enc_init(lsc->fs);

			return false;
		}

		if (recvd < (size_t)rx_len) {
			size_t i = 0;

			rx_feed = min(chunk, (size_t)rx_len - recvd);
			while (i < rx_feed) {
				bool done;

				i += ludlc_dec_feed(lsc->fs, csum, &lsc->conn,
						    &lsc->rx_chunk[i],
						    rx_feed - i, &done);
				if (!done)
					continue;

				ludlc_spi_on_packet(lsc, lsc->rx_pkt,
						    ludlc_dec_size(lsc->fs));
			}
			recvd += rx_feed;
		}

		sent += chunk;
	}

	if (tx_len)
		LUDLC_INC_STATS(&lsc->conn, tx_packet);

	return true;
}

/**
 * @brief Exchange until neither end has anything left, or the budget runs out.
 *
 * @param lsc The link. The caller holds @c xfer_lock.
 * @return true if it stopped on the budget rather than on silence, and should
 * be called again.
 */
static bool ludlc_spi_drain(struct ludlc_spi_connection *lsc)
{
	unsigned int budget = LUDLC_SPI_BUDGET;

	while (budget--) {
		if (ludlc_spi_quiesced(lsc))
			return false;

		if (!ludlc_spi_exchange(lsc))
			return false;

		/*
		 * Nothing above sleeps on an idle bus, and a controller that
		 * keeps up would otherwise hold this CPU for the whole burst.
		 * Only @c xfer_lock is held, so sleeping here is safe.
		 */
		cond_resched();
	}

	return true;
}

/** @brief One end has something to send: exchange until neither has. */
static void ludlc_spi_xfer_workfn(struct work_struct *work)
{
	struct ludlc_spi_connection *lsc =
			container_of(to_delayed_work(work),
				     struct ludlc_spi_connection,
				     xfer_work);
	bool more;

	scoped_guard(mutex, &lsc->xfer_lock)
		more = ludlc_spi_drain(lsc);

	if (ludlc_spi_quiesced(lsc))
		return;

	/*
	 * Straight back in if the budget cut a burst short, otherwise at the
	 * poll interval - and not at all where the peer can raise a line to
	 * signal that it has something.
	 */
	if (more)
		mod_delayed_work(system_wq, &lsc->xfer_work, 0);
	else if (lsc->poll_period)
		mod_delayed_work(system_wq, &lsc->xfer_work, lsc->poll_period);
}

/**
 * @brief The peer has something to send.
 *
 * Threaded, and the line stays masked until this returns: a peer that holds it
 * asserted until its queue has drained is served in one call, so the trigger
 * firmware describes can be an edge or a level without either being a special
 * case here.
 */
static irqreturn_t ludlc_spi_ready_isr(int irq, void *data)
{
	struct ludlc_spi_connection *lsc = data;
	bool more;

	if (ludlc_spi_quiesced(lsc))
		return IRQ_HANDLED;

	/* This thread runs exchanges too; see ludlc_spi_in_callback(). */
	WRITE_ONCE(lsc->irq_task, current);

	scoped_guard(mutex, &lsc->xfer_lock)
		more = ludlc_spi_drain(lsc);

	/* Left over from a burst the budget cut short. */
	if (more && !ludlc_spi_quiesced(lsc))
		mod_delayed_work(system_wq, &lsc->xfer_work, 0);

	return IRQ_HANDLED;
}

/* --- Connection Management --- */

/**
 * @brief Take the link down and free it.
 *
 * Marks the link terminated so both paths stop, removes the bus devices it
 * carried together with the core state, then stops the exchanges themselves.
 *
 * @param conn The link, as returned by ludlc_spi_master_connection_create().
 */
void ludlc_spi_master_connection_destroy(struct ludlc_connection *conn)
{
	struct ludlc_spi_connection *lsc;

	if (!conn)
		return;

	lsc = to_spi_conn(conn);

	WRITE_ONCE(conn->conn_state, LUDLC_STATE_TERMINATE);

	/* Releases the devices on this link along with the connection. */
	ludlc_connection_cleanup(conn);

	/*
	 * Only now, with the timers destroyed and the core done with the link,
	 * can nothing ask for another exchange. Both sources are stopped here:
	 * the interrupt first, since its handler queues the work for what a
	 * burst left over.
	 */
	if (lsc->irq)
		free_irq(lsc->irq, lsc);

	cancel_delayed_work_sync(&lsc->xfer_work);

	/*
	 * Nothing will call the checksum again, so its provider can be
	 * released. Last access to anything in @lsc.
	 */
	ludlc_link_csum_drop(&lsc->csum);

	ludlc_framing_destroy(lsc->fs);

	mutex_destroy(&lsc->xfer_lock);

	spi_set_drvdata(lsc->spi, NULL);
	put_device(&lsc->spi->dev);

	ludlc_platform_free(lsc);
}
EXPORT_SYMBOL_GPL(ludlc_spi_master_connection_destroy);

/**
 * @brief Ask for the data-ready line, if firmware described one.
 *
 * @return 0 whether or not there is one - a link without it polls instead -
 * or a negative errno if there is one and it could not be taken.
 */
static int ludlc_spi_request_irq(struct ludlc_spi_connection *lsc)
{
	int ret;

	if (lsc->spi->irq <= 0) {
		dev_info(&lsc->spi->dev,
			 "no data-ready line, polling every %ums\n",
			 jiffies_to_msecs(lsc->poll_period));

		return 0;
	}

	/*
	 * Whatever trigger firmware asked for: the handler runs in a thread
	 * with the line masked, so a level suits it as well as an edge.
	 */
	ret = request_threaded_irq(lsc->spi->irq, NULL, ludlc_spi_ready_isr,
				   IRQF_ONESHOT, dev_name(&lsc->spi->dev),
				   lsc);
	if (ret) {
		dev_err(&lsc->spi->dev, "cannot take the data-ready line: %d\n",
			ret);

		return ret;
	}

	lsc->irq = lsc->spi->irq;

	/* Nothing to poll for: the peer says when it has something. */
	lsc->poll_period = 0;

	return 0;
}

/**
 * @brief Open a LuDLC link on an SPI peripheral.
 *
 * Builds the framing around the checksum @p args names and starts the
 * handshake. The bus devices firmware describes below the peripheral are
 * registered once the peer answers, and removed again when it goes away.
 *
 * @param args The SPI device, the checksum to run, the timings the peer needs
 * and the cfg to open the link with, clock included.
 * @param[out] conn The link, on success.
 * @param cb Must be NULL: on Linux the events belong to the bus, which passes
 * them to the driver bound to the channel they concern.
 * @param user_arg Passed to the event callbacks as is. They are the bus's
 * here, and do not use it.
 * @return 0, or a negative errno with nothing left open.
 */
int ludlc_spi_master_connection_create(const ludlc_platform_args_t *args,
				       struct ludlc_connection **conn,
				       const struct ludlc_conn_cb *cb,
				       void *user_arg)
{
	struct ludlc_spi_connection *lsc;
	const struct ludlc_csum *csum;
	struct ludlc_conn_cfg cfg;
	int ret;

	if (!args || !args->spi.dev || !conn || cb)
		return -EINVAL;

	/*
	 * The exchange announces a frame before encoding it, so a framing
	 * whose wire form is only known once escaped cannot be carried here.
	 * The rest may: what the announcement says is what delimits the frame.
	 */
	if (args->framing && !ludlc_spi_framing_ok(args->framing)) {
		dev_err(&args->spi.dev->dev,
			"%s framing cannot be carried over SPI: its length is not known before it is sent\n",
			ludlc_framing_name(args->framing));

		return -EINVAL;
	}

	lsc = ludlc_platform_alloc(sizeof(*lsc));
	if (!lsc)
		return -ENOMEM;

	lsc->spi = to_spi_device(get_device(&args->spi.dev->dev));
	lsc->turnaround_us = args->spi.turnaround_us;
	lsc->poll_period = msecs_to_jiffies(args->spi.poll_interval_ms);

	mutex_init(&lsc->xfer_lock);
	INIT_DELAYED_WORK(&lsc->xfer_work, ludlc_spi_xfer_workfn);

	cfg = *ludlc_args_conn_cfg(args);

	ret = ludlc_connection_init(&lsc->conn, &cfg,
				    &ludlc_bus_conn_cb, user_arg);
	if (ret) {
		dev_err(&args->spi.dev->dev,
			"cannot open a link of %u payload octets: %d\n",
			(unsigned int)ludlc_cfg_max_payload(&cfg), ret);
		goto fail_conn_init;
	}

	/*
	 * Held only once the link is open, as ludlc_connection_init() clears
	 * everything it is given.
	 */
	ret = ludlc_link_csum_set(&lsc->csum, &lsc->spi->dev, args->csum);
	if (ret)
		goto fail_csum;

	csum = lsc->csum;

	/*
	 * Not ludlc_framing_or_default(): a link over a stream falls back on a
	 * framing that delimits, having no other way to end a frame. Every
	 * exchange here says how long its frame is, which is all delimiting
	 * provides, so the frame goes out unescaped.
	 */
	lsc->fs = ludlc_framing_create(args->framing ? : &ludlc_framing_raw);
	if (!lsc->fs) {
		ret = -ENOMEM;
		goto fail_csum;
	}

	lsc->frame_cap = LUDLC_FS_FRAMING(lsc->fs)->encoded_max(&lsc->conn,
								csum);

	ludlc_enc_init(lsc->fs);
	ludlc_dec_init(lsc->fs);
	/* Nothing is expected until an exchange announces a frame. */
	ludlc_dec_prep(lsc->fs, csum, lsc->rx_pkt, 0);

	lsc->conn.pconn.start_tx = ludlc_spi_schedule_xfer;
	lsc->conn.pconn.in_callback = ludlc_spi_in_callback;

	/* The peripheral represents the local end of the link on the bus. */
	ret = ludlc_conn_set_device(&lsc->conn, &lsc->spi->dev);
	if (ret)
		goto fail_set_dev;

	/*
	 * An exchange reaches the framing and the core through drvdata, and
	 * the interrupt below can start one straight away, so publish first.
	 */
	spi_set_drvdata(lsc->spi, lsc);

	ret = ludlc_spi_request_irq(lsc);
	if (ret)
		goto fail_irq;

	ret = ludlc_connect(&lsc->conn); /* Start handshake */
	if (ret)
		goto fail_connect;

	/*
	 * A link that polls has nothing to wait for otherwise, and one with a
	 * data-ready line still has its own handshake to send.
	 */
	mod_delayed_work(system_wq, &lsc->xfer_work, 0);

	*conn = &lsc->conn;

	return 0;

fail_connect:
	/* Live by now, so unwind the whole connection. */
	ludlc_spi_master_connection_destroy(&lsc->conn);

	return ret;

fail_irq:
	spi_set_drvdata(lsc->spi, NULL);
fail_set_dev:
	ludlc_link_csum_drop(&lsc->csum);
	ludlc_framing_destroy(lsc->fs);
fail_csum:
	ludlc_connection_cleanup(&lsc->conn);
fail_conn_init:
	mutex_destroy(&lsc->xfer_lock);
	put_device(&lsc->spi->dev);
	ludlc_platform_free(lsc);

	return ret;
}
EXPORT_SYMBOL_GPL(ludlc_spi_master_connection_create);

/**
 * @brief The checksum firmware asks this link to use, if it names one.
 *
 * No standard property describes one - a bus descriptor has no room for what
 * runs over it - so this is a property of our own naming a checksum the core
 * knows: ludlc,csum = "crc16-pr0", or "none" for no checksum. An unknown name
 * leaves the link on the default rather than failing the probe.
 */
static void ludlc_spi_fw_csum(struct spi_device *spi,
			      ludlc_platform_args_t *args)
{
	const struct ludlc_csum *csum;
	const char *name;

	if (device_property_read_string(&spi->dev, "ludlc,csum", &name))
		return;

	if (ludlc_crc_by_name(name, &csum)) {
		dev_warn(&spi->dev, "unknown checksum \"%s\", keeping %s\n",
			 name, args->csum ? args->csum->name : LUDLC_CSUM_NONE);
		return;
	}

	ludlc_csum_put(args->csum);
	args->csum = csum;
}

/**
 * @brief The framing firmware asks this link to use, if it names one.
 *
 * ludlc,framing = "raw" for the frame as it stands, the announcement being
 * what delimits it, or "cobs" for the stuffed form a UART link can also run.
 * The escaping is not available here; see
 * ludlc_spi_master_connection_create(). As with the checksum, an unknown name
 * leaves the link on the default rather than failing the probe.
 */
static void ludlc_spi_fw_framing(struct spi_device *spi,
				 ludlc_platform_args_t *args)
{
	const struct ludlc_framing *framing;
	const char *name;

	if (device_property_read_string(&spi->dev, "ludlc,framing", &name))
		return;

	framing = ludlc_framing_lookup(name);
	if (!framing || !ludlc_spi_framing_ok(framing)) {
		dev_warn(&spi->dev, "unusable framing \"%s\", keeping %s\n",
			 name, args->framing ? args->framing->name :
					       ludlc_spi_framing_dflt->name);
		return;
	}

	args->framing = framing;
}

/**
 * @brief The driver defaults, with what firmware says applied over them.
 *
 * A reference to the checksum is taken - see ludlc_args_csum_put().
 */
void ludlc_spi_default_args(struct device *dev, ludlc_platform_args_t *args)
{
	struct spi_device *spi = to_spi_device(dev);
	u32 size;

	*args = (ludlc_platform_args_t){
		.spi.dev = spi,
		.spi.turnaround_us = LUDLC_SPI_DFLT_TURNAROUND_US,
		.spi.poll_interval_ms = LUDLC_SPI_DFLT_POLL_MS,
		.framing = ludlc_spi_framing_dflt,
		.cfg = LUDLC_CONN_CFG_DEFAULT,
	};

	args->cfg.max_payload = (ludlc_payload_size_t)ludlc_payload_param;

	args->csum = ludlc_csum_get(ludlc_csum_default());

	ludlc_spi_fw_csum(spi, args);
	ludlc_spi_fw_framing(spi, args);
	ludlc_fw_chan_tag_bits(&spi->dev, &args->cfg);

	device_property_read_u32(&spi->dev, "ludlc,turnaround-us",
				 &args->spi.turnaround_us);
	device_property_read_u32(&spi->dev, "ludlc,poll-interval-ms",
				 &args->spi.poll_interval_ms);

	if (device_property_read_u32(&spi->dev, "ludlc,max-payload", &size))
		return;

	/*
	 * A size this build cannot hold leaves the link on the default, as an
	 * unknown checksum name does: easier to diagnose a link carrying less
	 * than the peer expects than one that never comes up.
	 */
	if (size > CONFIG_LUDLC_MAX_PAYLOAD_SIZE) {
		dev_warn(&spi->dev, "payload of %u octets is out of range\n",
			 size);
		return;
	}

	args->cfg.max_payload = (ludlc_payload_size_t)size;
}

/** @brief Bind to an SPI peripheral and open its link. */
static int ludlc_spi_probe(struct spi_device *spi)
{
	struct ludlc_connection *conn = NULL;
	ludlc_platform_args_t args;
	int ret;

	/*
	 * Mode and speed belong to the peripheral and firmware describes them
	 * the ordinary way; only the word size is forced here, the framing
	 * being a stream of octets.
	 */
	spi->bits_per_word = 8;

	ret = spi_setup(spi);
	if (ret) {
		dev_err(&spi->dev, "cannot set the bus up: %d\n", ret);
		return ret;
	}

	/*
	 * Firmware (DT/ACPI) and configfs are both pure configuration: start
	 * from what firmware says, let a configfs port describing this device
	 * override it attribute by attribute, then bring the connection up.
	 */
	ludlc_spi_default_args(&spi->dev, &args);
	ludlc_spi_cfg_apply(&spi->dev, &args);

	ret = ludlc_spi_link_up(&spi->dev, &args, &conn);
	ludlc_args_csum_put(&args);
	if (ret)
		return ret;

	/* The link is the bus, so tell configfs which one its nodes join. */
	ludlc_spi_cfg_bind(&spi->dev, conn);

	dev_info(&spi->dev, "LuDLC SPI transport ready at %u Hz\n",
		 spi->max_speed_hz);

	return 0;
}

/** @brief Release the peripheral, taking its link and its devices with it. */
static void ludlc_spi_remove(struct spi_device *spi)
{
	/*
	 * Remove the configfs-created devices while their link is still alive,
	 * and stop configfs from bringing the peripheral up behind our back.
	 * The link itself is only looked at afterwards, since a "live" store
	 * racing with us may have just created it.
	 */
	ludlc_spi_cfg_unbind(&spi->dev);

	ludlc_spi_link_down(&spi->dev);
}

/*
 * A suspend lasts longer than the watchdog, so the session is lost in any
 * case. The link is therefore taken down deliberately rather than left to time
 * out, and the peer is told on the way out so that it can start handshaking
 * its way back. The four phases hold the bus still, send that disconnect and
 * stop the clock, then start the link again as the system comes back - see
 * ludlc_conn_prepare() and friends for why each of them is where it is.
 */

/** @brief Hold the bus still for the transition; the link stays up. */
static int ludlc_spi_prepare(struct device *dev)
{
	struct ludlc_spi_connection *lsc = dev_get_drvdata(dev);

	if (lsc)
		ludlc_conn_prepare(&lsc->conn);

	return 0;
}

/** @brief Send the disconnect, get it out, then stop the link's clock. */
static int ludlc_spi_suspend(struct device *dev)
{
	struct ludlc_spi_connection *lsc = dev_get_drvdata(dev);

	if (!lsc)
		return 0;

	/*
	 * The devices on the bus suspended before this one, each with the link
	 * still under it, so this is the last thing to be sent. Being
	 * unnumbered it goes out ahead of anything they left queued, and an
	 * exchange either carries it or does not return: unlike a UART there
	 * is no queue in the hardware to drain afterwards.
	 */
	if (ludlc_conn_send_disconnect(&lsc->conn))
		flush_delayed_work(&lsc->xfer_work);

	/*
	 * This stops both paths first, so the exchange that may still be in
	 * flight cannot leave a timer armed behind it.
	 */
	ludlc_conn_suspend(&lsc->conn);

	/*
	 * Taking the lock waits for the exchange in progress, no matter which
	 * of the two started it - cancelling the work would miss one the
	 * interrupt thread is running. The SUSPENDED bit above has already
	 * stopped both from starting another, so the lock is dropped again
	 * immediately: the work cannot be waited for while holding what it is
	 * waiting for.
	 */
	mutex_lock(&lsc->xfer_lock);
	mutex_unlock(&lsc->xfer_lock);

	cancel_delayed_work_sync(&lsc->xfer_work);

	/*
	 * The bit still being set means the disconnect packet never went out.
	 * The session ends here all the same: finish it locally, so that the
	 * queued packets are failed, the peer is left to its watchdog and
	 * resume opens a new link rather than trusting a session the far end
	 * has already given up on.
	 */
	if (test_bit(LUDLC_CONN_DISC_AFTER_TX_F, &lsc->conn.flags))
		ludlc_handle_disconnect(&lsc->conn);

	return 0;
}

/** @brief The bus can be used again: restart the link. */
static int ludlc_spi_resume(struct device *dev)
{
	struct ludlc_spi_connection *lsc = dev_get_drvdata(dev);

	if (!lsc)
		return 0;

	ludlc_conn_resume(&lsc->conn);

	/*
	 * Nothing arrives here unasked, so a peer that spent the suspend
	 * getting ready to talk is only heard once something asks it. The
	 * handshake resume queues does that, and a link that polls needs its
	 * timer started again in any case.
	 */
	mod_delayed_work(system_wq, &lsc->xfer_work, 0);

	return 0;
}

/** @brief Every device is awake: let the bus follow the link again. */
static void ludlc_spi_complete(struct device *dev)
{
	struct ludlc_spi_connection *lsc = dev_get_drvdata(dev);

	if (lsc)
		ludlc_conn_complete(&lsc->conn);
}

static const struct dev_pm_ops ludlc_spi_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(ludlc_spi_suspend, ludlc_spi_resume)
	.prepare	= pm_sleep_ptr(ludlc_spi_prepare),
	.complete	= pm_sleep_ptr(ludlc_spi_complete),
};

static const struct of_device_id ludlc_spi_of_match[] = {
	{ .compatible = "ludlc,spi" },
	{ }
};
MODULE_DEVICE_TABLE(of, ludlc_spi_of_match);

static const struct spi_device_id ludlc_spi_id[] = {
	{ "ludlc-spi" },
	{ }
};
MODULE_DEVICE_TABLE(spi, ludlc_spi_id);

static struct spi_driver ludlc_spi_driver = {
	.driver = {
		.name = "ludlc-spi",
		.of_match_table = ludlc_spi_of_match,
		.pm = pm_sleep_ptr(&ludlc_spi_pm_ops),
	},
	.id_table = ludlc_spi_id,
	.probe = ludlc_spi_probe,
	.remove = ludlc_spi_remove,
};

/**
 * @brief The peripheral named @p name among those this driver is bound to.
 *
 * @return The device with a reference held, or NULL. Release it with
 * put_device().
 */
struct device *ludlc_spi_dev_find(const char *name)
{
	return driver_find_device_by_name(&ludlc_spi_driver.driver, name);
}

/**
 * @brief Settle the default framing, publish configfs, then bind the
 * peripherals.
 *
 * In that order: a peripheral can probe as soon as the driver registers, and
 * it has to find both of those ready. The checksums were settled when the core
 * loaded.
 */
static int __init ludlc_spi_init(void)
{
	int ret;

	if (ludlc_framing_param) {
		const struct ludlc_framing *framing =
				ludlc_framing_lookup(ludlc_framing_param);

		if (!framing) {
			pr_err("ludlc_spi_master: no framing called \"%s\"\n",
			       ludlc_framing_param);

			return -EINVAL;
		}

		if (!ludlc_spi_framing_ok(framing)) {
			pr_err("ludlc_spi_master: %s framing cannot be carried over SPI: its length is not known before it is sent\n",
			       framing->name);

			return -EINVAL;
		}

		ludlc_spi_framing_dflt = framing;
	}

	/* A size no link could take is refused here rather than per link. */
	if (ludlc_payload_param > CONFIG_LUDLC_MAX_PAYLOAD_SIZE) {
		pr_err("ludlc_spi_master: max_payload=%u, this build allows %u\n",
		       ludlc_payload_param,
		       (unsigned int)CONFIG_LUDLC_MAX_PAYLOAD_SIZE);

		return -EINVAL;
	}

	ret = ludlc_spi_cfg_init();
	if (!ret) {
		ret = spi_register_driver(&ludlc_spi_driver);
		if (ret)
			ludlc_spi_cfg_deinit();
	}

	return ret;
}
module_init(ludlc_spi_init);

/** @brief Release the peripherals first, then the configfs describing them. */
static void __exit ludlc_spi_exit(void)
{
	spi_unregister_driver(&ludlc_spi_driver);
	ludlc_spi_cfg_deinit();
}
module_exit(ludlc_spi_exit);

MODULE_AUTHOR("Andrey Volkov <andrey@volkov.fr>");
MODULE_DESCRIPTION("LuDLC SPI master transport");
MODULE_LICENSE("GPL");

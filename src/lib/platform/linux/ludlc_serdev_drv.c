// SPDX-License-Identifier: GPL-2.0-only
/**
 * @file ludlc_serdev_drv.c
 *
 * @brief LuDLC serial transport over Linux serdev.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * One link per serdev port: the UART settings and the checksum come from
 * firmware or configfs, the framing and the core connection are kept here, and
 * the port represents the local end of the link on the LuDLC bus.
 */

#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/pm.h>
#include <linux/scatterlist.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/workqueue.h>
#include <linux/acpi.h>
#include "linux/dev_printk.h"
#include "linux/device.h"

#include "ludlc_bus.h"
#include "ludlc_csum.h"
#include "ludlc_serdev.h"

/* Used when neither firmware nor configfs describes the port. */
#define LUDLC_SERDEV_DEFAULT_BAUD	115200

/*
 * How long the port is given to send the last frame of a session before the
 * system suspends. A disconnect packet is a header and one octet, so this is
 * generous even for the slowest line this is likely to run over.
 */
#define LUDLC_SERDEV_DRAIN_TIME		msecs_to_jiffies(100)

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
	"configfs says otherwise: "
	LUDLC_FRAMING_NAMES_DELIMITED);

/*
 * A port hands over a stream of octets, so a link that names no framing runs
 * the one that delimits its own frames at a cost the data cannot change. A
 * peer that speaks the escaping must be named, not guessed at.
 */
static const struct ludlc_framing *ludlc_serdev_framing_dflt;

/*
 * As for the framing: one payload size for the whole network is the normal
 * case. Zero leaves every link on what the build allows.
 */
static unsigned int ludlc_payload_param;
module_param_named(max_payload, ludlc_payload_param, uint, 0444);
MODULE_PARM_DESC(max_payload,
"octets of payload every link carries unless firmware or configfs says otherwise");

/**
 * @brief Run the TX round the core is asking for where it may block.
 *
 * The ping timer and the tty write wakeup both call in from contexts that must
 * not block on the port. The one caller that could do the work itself is the
 * TX loop asking for another round, and it needs no help: it re-checks the
 * request bit before it gives up.
 */
static inline void ludlc_serdev_schedule_tx(struct ludlc_connection *conn)
{
	struct ludlc_serial_connection *lsc = to_serial_conn(conn);

	if (current_work() != &lsc->tx_work)
		schedule_work(&lsc->tx_work);
}

/**
 * @brief Is writing to the port forbidden right now?
 *
 * Either the link is going away, in which case the core timers are gone, there
 * is nothing left to send and nothing may arm a timer that outlives the link,
 * or the system is suspended and the port is not ours until it resumes.
 * Writing only: received data is always accepted when the tty hands it over,
 * which around a suspend can happen on either side of the port's own resume,
 * because dropping it could lose the packet a wakeup device is waiting for.
 * Anything the core then needs to send waits here.
 */
static inline bool ludlc_serdev_tx_quiesced(struct ludlc_serial_connection *lsc)
{
	return test_bit(LUDLC_CONN_SUSPENDED_F, &lsc->conn.pconn.flags) ||
	       READ_ONCE(lsc->conn.conn_state) == LUDLC_STATE_TERMINATE;
}

/**
 * @brief Send queued packets until the queue is empty or the port is full.
 *
 * Each pass loads the next packet once the encoder is idle, encodes into one
 * linear free span of the fifo, and hands one linear span to the port. A
 * short write means the port is full: the run ends, and write_wakeup queues
 * the next one once the port drains.
 */
static void ludlc_serdev_tx_workfn(struct work_struct *work)
{
	struct ludlc_serial_connection *lsc =
			container_of(work,
				     struct ludlc_serial_connection,
				     tx_work);
	struct scatterlist sg;

	sg_init_table(&sg, 1);

	while (!ludlc_serdev_tx_quiesced(lsc)) {
		unsigned int len;
		u8 *tail;
		int ret;

		/*
		 * Load the next packet as soon as the encoder is idle, so
		 * frames go back to back. A pending of 0 means another packet
		 * is queued: no EOF after this one, and the request stays set.
		 */
		if (!ludlc_enc_busy(lsc->fs) &&
		    test_and_clear_bit(LUDLC_CONN_FORCE_TX_F,
				       &lsc->conn.pconn.flags)) {
			const void *hdr, *payload;
			ludlc_payload_size_t hdr_sz, pay_sz;
			int pending;

			pending = ludlc_get_packet_to_send(&lsc->conn, &hdr,
							   &hdr_sz, &payload,
							   &pay_sz);
			if (pending >= 0) {
				ludlc_enc_prep(lsc->fs, hdr, hdr_sz, payload,
					       pay_sz, pending != 0);
				if (!pending)
					set_bit(LUDLC_CONN_FORCE_TX_F,
						&lsc->conn.pconn.flags);
			}
		}

		if (ludlc_enc_busy(lsc->fs) &&
		    kfifo_dma_in_prepare(&lsc->tx_fifo, &sg, 1,
					 kfifo_avail(&lsc->tx_fifo))) {
			size_t n = ludlc_enc_fill(lsc->fs, lsc->csum,
						  sg_virt(&sg), sg.length);

			kfifo_dma_in_finish(&lsc->tx_fifo, n);
			if (!ludlc_enc_busy(lsc->fs))
				LUDLC_INC_STATS(&lsc->conn, tx_packet);
		}

		len = kfifo_out_linear_ptr(&lsc->tx_fifo, &tail,
					   LUDLC_SERDEV_TX_BUF_SZ);

		/*
		 * Nothing left to send. A request made from inside this run
		 * does not queue the work again, so look for one here.
		 */
		if (!len) {
			if (!test_bit(LUDLC_CONN_FORCE_TX_F,
				      &lsc->conn.pconn.flags))
				break;
			continue;
		}

		ret = serdev_device_write_buf(lsc->serdev, tail, len);
		if (ret < 0) {
			ludlc_handle_disconnect(&lsc->conn);
			break;
		}

		kfifo_skip_count(&lsc->tx_fifo, ret);

		/*
		 * A short write means the port is full. write_wakeup queues
		 * the next run once it drains.
		 */
		if (ret < len)
			break;

		cond_resched();
	}
}

/**
 * @brief Feed what the tty handed over to the decoder.
 */
static size_t ludlc_serdev_receive_buf(struct serdev_device *serdev,
					const u8 *data, size_t count)
{
	struct ludlc_serial_connection *lsc = serdev_device_get_drvdata(serdev);
	const struct ludlc_csum *csum = lsc->csum;
	size_t i;

	if (READ_ONCE(lsc->conn.conn_state) == LUDLC_STATE_TERMINATE)
		return count;

	for (i = 0; i < count;) {
		bool done;
		ludlc_timestamp_t ts;

		i += ludlc_dec_feed(lsc->fs, csum, &lsc->conn, &data[i],
				    count - i, &done);
		if (!done)
			continue;

		if (lsc->conn.cfg.get_timestamp(&ts)) {
			LUDLC_LOG_ERROR("Can't get the timestamp, "
					"drop the packet");
		} else {
			ludlc_receive(&lsc->conn,
				(ludlc_packet_t *)lsc->rx_buf,
				ludlc_dec_size(lsc->fs), ts);
		}

		ludlc_dec_prep(lsc->fs, csum, lsc->rx_buf,
			       ludlc_conn_frame_sz(&lsc->conn, csum));
	}

	return count;
}

/** @brief The port has room again: continue where the TX loop stopped. */
static void ludlc_serdev_write_wakeup(struct serdev_device *serdev)
{
	struct ludlc_serial_connection *lsc = serdev_device_get_drvdata(serdev);

	/*
	 * Room on a port with nothing to send is of no use, and queueing the
	 * work again after teardown cancelled it would leave it running on a
	 * connection that is being freed.
	 */
	if (ludlc_serdev_tx_quiesced(lsc))
		return;

	schedule_work(&lsc->tx_work);
}

static const struct serdev_device_ops ludlc_serdev_ops = {
	.receive_buf = ludlc_serdev_receive_buf,
	.write_wakeup = ludlc_serdev_write_wakeup,
};

/* --- Connection Management --- */

/**
 * @brief Take the link down and free it.
 *
 * Marks the link terminated so the RX and TX paths stop, removes the bus
 * devices it carried together with the core state, then closes the port.
 *
 * @param conn The link, as returned by ludlc_serial_connection_create().
 */
void ludlc_serial_connection_destroy(struct ludlc_connection *conn)
{
	struct ludlc_serial_connection *lsc;

	if (!conn)
		return;

	lsc = to_serial_conn(conn);

	WRITE_ONCE(conn->conn_state, LUDLC_STATE_TERMINATE);

	/* Releases the devices on this link along with the connection. */
	ludlc_connection_cleanup(conn);

	/*
	 * Only now, with the timers destroyed and the core done with the link,
	 * can nothing ask for another TX round. A write wakeup arriving from
	 * here on finds the link terminated and does nothing, so the work is
	 * never queued again.
	 */
	cancel_work_sync(&lsc->tx_work);

	/*
	 * Closing the port ends the client ops: it waits for the ones in flight
	 * and lets no new ones start. Only then can the state they reach
	 * through drvdata be cleared and freed.
	 */
	if (lsc->serdev) {
		serdev_device_close(lsc->serdev);
		serdev_device_set_drvdata(lsc->serdev, NULL);

		put_device(&lsc->serdev->dev);
	}

	/*
	 * Nothing will call the checksum again, so its provider can be
	 * released. Last access to anything in @lsc.
	 */
	ludlc_link_csum_drop(&lsc->csum);

	ludlc_framing_destroy(lsc->fs);
	ludlc_platform_free(lsc);
}
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(ludlc_serial_connection_destroy);
#endif

/**
 * @brief Open a LuDLC link on a serdev port.
 *
 * Configures the port as @p args describes it, builds the framing around the
 * checksum it names, and starts the handshake. The bus devices firmware
 * describes below the port are registered once the peer answers, and removed
 * again when it goes away.
 *
 * @param args The serdev device, its UART settings, the checksum to use and
 * the cfg to open the link with, clock included.
 * @param[out] conn The link, on success.
 * @param cb Must be NULL: on Linux the events belong to the bus, which passes
 * them to the driver bound to the channel they concern.
 * @param user_arg Passed to the event callbacks as is. They are the bus's
 * here, and do not use it.
 * @return 0, or a negative errno with nothing left open.
 */
int ludlc_serial_connection_create(
		   const ludlc_platform_args_t *args,
		   struct ludlc_connection **conn,
		   const struct ludlc_conn_cb *cb,
		   void *user_arg)
{
	struct ludlc_serial_connection *lsc;
	const struct ludlc_csum *csum;
	struct ludlc_conn_cfg cfg;
	unsigned int baudrate;
	int ret;

	if (!args || !args->serdev.dev || !conn || cb)
		return -EINVAL;

	dev_dbg(&args->serdev.dev->dev,
		"Creating the serdev conn on dev %s",
		dev_name(args->serdev.dev->ctrl->host));

	/*
	 * A port hands over octets and nothing else, so a framing that expects
	 * to be told where a frame ends would never complete one here.
	 */
	if (args->framing && args->framing->needs_wire_len) {
		dev_err(&args->serdev.dev->dev,
			"%s framing needs a transport that "
				"says where a frame ends\n",
			args->framing->name);

		return -EINVAL;
	}

	lsc = ludlc_platform_alloc(sizeof(*lsc));
	if (!lsc)
		return -ENOMEM;

	lsc->serdev = to_serdev_device(get_device(&args->serdev.dev->dev));

	lsc->fs = ludlc_framing_create(args->framing);
	if (!lsc->fs) {
		ret = -ENOMEM;
		goto fail_framing;
	}

	INIT_KFIFO(lsc->tx_fifo);
	INIT_WORK(&lsc->tx_work, ludlc_serdev_tx_workfn);

	cfg = *ludlc_args_conn_cfg(args);

	ret = ludlc_connection_init(&lsc->conn, &cfg,
				    &ludlc_bus_conn_cb, user_arg);
	if (ret) {
		dev_err(&args->serdev.dev->dev,
			"cannot open a link of %u payload octets: %d\n",
			(unsigned int)ludlc_cfg_max_payload(&cfg), ret);
		goto fail_conn_init;
	}

	/*
	 * Held only once the link is open, as ludlc_connection_init() clears
	 * everything it is given. The framing is built around it next.
	 */
	ret = ludlc_link_csum_set(&lsc->csum, &lsc->serdev->dev, args->csum);
	if (ret)
		goto fail_csum;

	csum = lsc->csum;

	ludlc_enc_init(lsc->fs);
	ludlc_dec_init(lsc->fs);

	/*
	 * A stream of octets says nothing about where a frame ends, so the
	 * framing finds that itself; all the link provides is somewhere to
	 * gather one.
	 */
	ludlc_dec_prep(lsc->fs, csum, lsc->rx_buf,
		       ludlc_conn_frame_sz(&lsc->conn, csum));

	lsc->conn.pconn.start_tx = ludlc_serdev_schedule_tx;

	/* The port represents the local end of the link on the LuDLC bus. */
	ret = ludlc_conn_set_device(&lsc->conn, &lsc->serdev->dev);
	if (ret)
		goto fail_set_dev;

	/*
	 * The port starts delivering data as soon as it is opened, and the
	 * client ops reach the decoder and the core through drvdata, so both
	 * have to be ready and published first.
	 */
	serdev_device_set_drvdata(lsc->serdev, lsc);
	serdev_device_set_client_ops(lsc->serdev, &ludlc_serdev_ops);

	ret = serdev_device_open(lsc->serdev);
	if (ret) {
		serdev_device_set_drvdata(lsc->serdev, NULL);
		goto fail_set_dev;
	}

	/*
	 * The port reports the speed it actually set, which is not always the
	 * one asked for. Nothing would ever be received on a link running at
	 * the wrong rate, so fail rather than leave one silently deaf.
	 */
	baudrate = serdev_device_set_baudrate(lsc->serdev,
						args->serdev.baudrate);
	if (baudrate != args->serdev.baudrate) {
		dev_err(&lsc->serdev->dev,
			"cannot run at %u baud, port set %u instead\n",
			args->serdev.baudrate, baudrate);
		ret = -EINVAL;
		goto fail_serdev_conn;
	}

	serdev_device_set_flow_control(lsc->serdev, args->serdev.flow_control);

	ret = serdev_device_set_parity(lsc->serdev, args->serdev.parity);
	if (ret) {
		dev_err(&lsc->serdev->dev, "cannot set parity: %d\n", ret);
		goto fail_serdev_conn;
	}

	dev_info(&lsc->serdev->dev,
		"Serdev conn has been created: baud = %u", baudrate);

	ret = ludlc_connect(&lsc->conn); /* Start handshake */
	if (!ret) {
		*conn = &lsc->conn;
		return 0;
	}

fail_serdev_conn:
	/* The port is open and live by now, so unwind the whole connection. */
	ludlc_serial_connection_destroy(&lsc->conn);

	return ret;

fail_set_dev:
	ludlc_link_csum_drop(&lsc->csum);
fail_csum:
	ludlc_connection_cleanup(&lsc->conn);
fail_conn_init:
	ludlc_framing_destroy(lsc->fs);
fail_framing:
	put_device(&lsc->serdev->dev);
	ludlc_platform_free(lsc);

	dev_dbg(&args->serdev.dev->dev,
		"ludlc_serial_connection_create failed with %d", ret);

	return ret;
}
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(ludlc_serial_connection_create);
#endif

#ifdef CONFIG_ACPI
struct ludlc_serdev_acpi_walk {
	struct serdev_device *serdev;
	ludlc_platform_args_t *args;
	bool found;
};

/**
 * @brief Take the UART settings from a UARTSerialBus descriptor in _CRS.
 *
 * Speed, parity and flow control, as far as serdev can honour them.
 */
static acpi_status ludlc_serdev_acpi_uart(struct acpi_resource *rsc, void *ctx)
{
	struct acpi_resource_uart_serialbus *uart;
	struct ludlc_serdev_acpi_walk *walk = ctx;
	struct serdev_device *serdev = walk->serdev;

	if (!serdev_acpi_get_uart_resource(rsc, &uart))
		return AE_OK;

	walk->args->serdev.baudrate = uart->default_baud_rate;

	/* serdev supports RTS/CTS only, so any other scheme is ignored. */
	if (uart->flow_control & ~(u8)ACPI_UART_FLOW_CONTROL_HW)
		dev_warn(&serdev->dev, "unsupported flow control %#04x\n",
			 uart->flow_control);

	walk->args->serdev.flow_control = uart->flow_control &
					   ACPI_UART_FLOW_CONTROL_HW;

	switch (uart->parity) {
	case ACPI_UART_PARITY_NONE:
		walk->args->serdev.parity = SERDEV_PARITY_NONE;
		break;
	case ACPI_UART_PARITY_EVEN:
		walk->args->serdev.parity = SERDEV_PARITY_EVEN;
		break;
	case ACPI_UART_PARITY_ODD:
		walk->args->serdev.parity = SERDEV_PARITY_ODD;
		break;
	default:
		dev_warn(&serdev->dev, "unsupported parity %#04x\n",
			 uart->parity);
		walk->args->serdev.parity = SERDEV_PARITY_NONE;
		break;
	}

	walk->found = true;

	dev_dbg(&serdev->dev,
		"ACPI: baudrate = %u, "
		"flow_control = %d, "
		"parity = %d",
		walk->args->serdev.baudrate,
		walk->args->serdev.flow_control,
		walk->args->serdev.parity);

	/* A serdev device hangs off exactly one UART, so stop here. */
	return AE_CTRL_TERMINATE;
}

/**
 * @brief The line settings ACPI describes for @p serdev, if any.
 *
 * @return Whether _CRS contained a UART descriptor to take them from.
 */
static bool ludlc_serdev_acpi_args(struct serdev_device *serdev,
				   ludlc_platform_args_t *args)
{
	struct ludlc_serdev_acpi_walk walk = {
		.serdev = serdev,
		.args = args,
	};
	acpi_handle handle = ACPI_HANDLE(&serdev->dev);
	acpi_status status;

	if (!handle)
		return false;

	status = acpi_walk_resources(handle, METHOD_NAME__CRS,
				     ludlc_serdev_acpi_uart, &walk);
	if (ACPI_FAILURE(status)) {
		dev_warn(&serdev->dev, "cannot read _CRS: %s\n",
			 acpi_format_exception(status));
		return false;
	}

	return walk.found;
}
#else
static inline bool ludlc_serdev_acpi_args(struct serdev_device *serdev,
					  ludlc_platform_args_t *args)
{
	return false;
}
#endif

#ifdef CONFIG_ACPI
/** @brief Does _STA report @p dev as enabled, and not merely present? */
static inline bool ludlc_acpi_enabled(struct device *dev)
{
	struct acpi_device *adev = ACPI_COMPANION(dev);

	return !adev || adev->status.enabled;
}
#else
static inline bool ludlc_acpi_enabled(struct device *dev)
{
	return true;
}
#endif

/**
 * @brief Does firmware ask for this port to be brought up on its own?
 *
 * A DT node always does: the serdev core only registers available children, so
 * a disabled one never reaches probe. An ACPI node need not, because the core
 * registers everything _STA reports as present, enabled or not, and present
 * but not enabled means "described, but leave it alone". A port with no
 * firmware node behind it must not be brought up either.
 */
static inline bool ludlc_serdev_fw_enabled(struct serdev_device *serdev)
{
	if (!fwnode_device_is_available(dev_fwnode(&serdev->dev)))
		return false;

	return ludlc_acpi_enabled(&serdev->dev);
}

static struct serdev_device_driver ludlc_serdev_driver;

/**
 * @brief The port named @p name among those this driver is bound to.
 *
 * @return The device with a reference held, or NULL. Release it with
 * put_device().
 */
struct device *ludlc_serdev_dev_find(const char *name)
{
	return driver_find_device_by_name(&ludlc_serdev_driver.driver, name);
}

/**
 * @brief The checksum firmware asks this port to use, if it names one.
 *
 * No standard property describes one - a UART descriptor has no room for what
 * runs over it - so this is a property of our own naming a checksum this
 * driver knows: ludlc,csum = "crc16-pr0", or "none" for no checksum. An
 * unknown name leaves the port on the default rather than failing the probe.
 */
static void ludlc_serdev_fw_crc(struct serdev_device *serdev,
				ludlc_platform_args_t *args)
{
	const struct ludlc_csum *csum;
	const char *name;

	if (device_property_read_string(&serdev->dev, "ludlc,csum", &name))
		return;

	if (ludlc_crc_by_name(name, &csum)) {
		dev_warn(&serdev->dev,
			 "unknown checksum \"%s\", keeping %s\n", name,
			 args->csum ? args->csum->name : LUDLC_CSUM_NONE);
		return;
	}

	ludlc_csum_put(args->csum);
	args->csum = csum;
}

/**
 * @brief The framing firmware asks this port to use, if it names one.
 *
 * ludlc,framing = "cobs", the alternative being "hdlc". As with the checksum,
 * an unknown name leaves the port on the default: a link that comes up on the
 * wrong framing is no worse than one that does not come up at all, and is
 * easier to diagnose from the far end.
 */
static void ludlc_serdev_fw_framing(struct serdev_device *serdev,
				    ludlc_platform_args_t *args)
{
	const struct ludlc_framing *framing;
	const char *name;

	if (device_property_read_string(&serdev->dev, "ludlc,framing", &name))
		return;

	framing = ludlc_framing_lookup(name);
	if (!framing || !ludlc_serdev_framing_ok(framing)) {
		dev_warn(&serdev->dev, "unusable framing \"%s\", keeping %s\n",
			 name, ludlc_framing_name(args->framing));
		return;
	}

	args->framing = framing;
}

/**
 * @brief The frame size firmware asks this port to carry, if it names one.
 *
 * ludlc,max-payload = <n>. As with the checksum and the framing, a size this
 * build cannot hold leaves the port on the default rather than failing the
 * probe: a link that comes up carrying less than the peer expects is easier to
 * diagnose than one that does not come up.
 */
static void ludlc_serdev_fw_payload(struct serdev_device *serdev,
				    ludlc_platform_args_t *args)
{
	u32 size;

	if (device_property_read_u32(&serdev->dev, "ludlc,max-payload", &size))
		return;

	if (size > CONFIG_LUDLC_MAX_PAYLOAD_SIZE) {
		dev_warn(&serdev->dev,
			 "payload of %u octets is out of range, keeping %u\n",
			 size, (unsigned int)args->cfg.max_payload);
		return;
	}

	args->cfg.max_payload = (ludlc_payload_size_t)size;
}

/**
 * @brief The driver defaults, with what firmware says applied over them.
 *
 * ACPI keeps speed, parity and flow control together in the UARTSerialBus
 * descriptor of _CRS; DT has a binding for the speed only, so the rest keeps
 * its default there unless configfs says otherwise. A reference to the
 * checksum is taken - see ludlc_args_csum_put().
 */
void ludlc_serdev_default_args(struct device *dev,
			       ludlc_platform_args_t *args)
{
	struct serdev_device *serdev = to_serdev_device(dev);

	*args = (ludlc_platform_args_t){
		.serdev.dev = serdev,
		.serdev.baudrate = LUDLC_SERDEV_DEFAULT_BAUD,
		.serdev.parity = SERDEV_PARITY_NONE,
		.framing = ludlc_serdev_framing_dflt,
		.cfg = {
			.max_payload =
				(ludlc_payload_size_t)ludlc_payload_param,
			.window = CONFIG_LUDLC_WINDOW,
			.ping_time = CONFIG_LUDLC_PING_TIME,
			.default_ttl = LUDLC_DEFAULT_TTL,
		},
	};

	args->csum = ludlc_csum_get(ludlc_csum_default());

	ludlc_serdev_fw_crc(serdev, args);
	ludlc_serdev_fw_framing(serdev, args);
	ludlc_serdev_fw_payload(serdev, args);
	ludlc_fw_chan_tag_bits(&serdev->dev, &args->cfg);

	if (ludlc_serdev_acpi_args(serdev, args))
		return;

	device_property_read_u32(&serdev->dev, "current-speed",
				 &args->serdev.baudrate);
}

/**
 * @brief Bind to a serdev port, and create its link if firmware asks for one.
 *
 * @return 0, -ENODEV for a port neither firmware nor configfs can open, or a
 * negative errno from opening the link.
 */
static int ludlc_serdev_probe(struct serdev_device *serdev)
{
	ludlc_platform_args_t args;
	struct ludlc_connection *conn = NULL;
	int ret;

	/*
	 * A port firmware describes but does not enable is meant to be left
	 * alone until user space asks for it. Only configfs can ask, so
	 * without it there is nothing this driver can do with the port.
	 */
	if (!ludlc_serdev_fw_enabled(serdev)) {
		if (!IS_ENABLED(CONFIG_CONFIGFS_FS))
			return -ENODEV;

		ludlc_serdev_cfg_bind(&serdev->dev, NULL);
		dev_info(&serdev->dev,
			 "LuDLC port held down, awaiting configfs\n");

		return 0;
	}

	/*
	 * Firmware (DT/ACPI) and configfs are both pure configuration: start
	 * from what firmware says, let a configfs port describing this device
	 * override it attribute by attribute, then bring the connection up.
	 */
	ludlc_serdev_default_args(&serdev->dev, &args);
	ludlc_serdev_cfg_apply(&serdev->dev, &args);

	ret = ludlc_serdev_link_up(&serdev->dev, &args, &conn);
	ludlc_args_csum_put(&args);
	if (ret)
		return ret;

	/* The link is the bus, so tell configfs which one its nodes join. */
	ludlc_serdev_cfg_bind(&serdev->dev, conn);

	dev_info(&serdev->dev, "LuDLC serdev transport ready\n");

	return 0;
}

/** @brief Release the port, taking its link and its devices with it. */
static void ludlc_serdev_remove(struct serdev_device *serdev)
{
	/*
	 * Remove the configfs-created devices while their link is still alive,
	 * and stop configfs from bringing the port up behind our back. The
	 * link itself is only looked at afterwards, since a "live" store
	 * racing with us may have just created it.
	 */
	ludlc_serdev_cfg_unbind(&serdev->dev);

	ludlc_serdev_link_down(&serdev->dev);

	dev_info(&serdev->dev, "LuDLC serdev transport has been removed\n");
}

/*
 * A suspend lasts longer than the watchdog, so the session is lost in any
 * case. The link is therefore taken down deliberately rather than left to time
 * out, and the peer is told on the way out so that it can start handshaking
 * its way back. The four phases hold the bus still, send that disconnect and
 * stop the clock, then start the link again as the system comes back - see
 * ludlc_conn_prepare() and friends for why each of them is where it is. User
 * space is frozen throughout, so configfs cannot bring a port up or down
 * behind any of this.
 */

/** @brief Hold the bus still for the transition; the link stays up. */
static int ludlc_serdev_prepare(struct device *dev)
{
	struct ludlc_serial_connection *lsc = dev_get_drvdata(dev);

	/* A port held down for configfs has no link. */
	if (lsc)
		ludlc_conn_prepare(&lsc->conn);

	return 0;
}

/** @brief Send the disconnect, get it out, then stop the link's clock. */
static int ludlc_serdev_suspend(struct device *dev)
{
	struct ludlc_serial_connection *lsc = dev_get_drvdata(dev);

	if (!lsc)
		return 0;

	/*
	 * The devices on the bus suspended before this one, each with the link
	 * still under it, so this is the last thing to be sent. Being
	 * unnumbered it goes out ahead of anything they left queued, and it
	 * only helps if it leaves before the port does: run the TX round, then
	 * wait for the port to finish with it. A port too congested to send it
	 * in time leaves the peer its watchdog, which is all it would have had
	 * anyway.
	 */
	if (ludlc_conn_send_disconnect(&lsc->conn)) {
		flush_work(&lsc->tx_work);
		serdev_device_wait_until_sent(lsc->serdev,
					      LUDLC_SERDEV_DRAIN_TIME);
	}

	/*
	 * This stops the RX and TX paths first, so the TX round that may still
	 * be in flight cannot leave a timer armed behind it.
	 */
	ludlc_conn_suspend(&lsc->conn);
	cancel_work_sync(&lsc->tx_work);

	/*
	 * Nothing half-sent survives the suspend: the link starts over on
	 * resume, and the tail of a frame from the old session has no place in
	 * it, least of all a disconnect that did not fit, which would arrive
	 * on the far side and take the new session down as it is set up.
	 * Resetting here is safe because the work is cancelled just above, and
	 * anything the RX path may queue turns back at the quiesce check.
	 */
	ludlc_enc_init(lsc->fs);
	kfifo_reset(&lsc->tx_fifo);

	/*
	 * The bit still being set means the port never took the whole
	 * disconnect packet, and the octets just dropped were the rest of it.
	 * The session ends here all the same: finish it locally, so that the
	 * queued packets are failed, the peer is left to its watchdog and
	 * resume opens a new link rather than trusting a session the far end
	 * has already given up on.
	 */
	if (test_bit(LUDLC_CONN_DISC_AFTER_TX_F, &lsc->conn.flags))
		ludlc_handle_disconnect(&lsc->conn);

	return 0;
}

/** @brief The port can be used again: restart the link. */
static int ludlc_serdev_resume(struct device *dev)
{
	struct ludlc_serial_connection *lsc = dev_get_drvdata(dev);

	if (lsc)
		ludlc_conn_resume(&lsc->conn);

	return 0;
}

/** @brief Every device is awake: let the bus follow the link again. */
static void ludlc_serdev_complete(struct device *dev)
{
	struct ludlc_serial_connection *lsc = dev_get_drvdata(dev);

	if (lsc)
		ludlc_conn_complete(&lsc->conn);
}

static const struct dev_pm_ops ludlc_serdev_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(ludlc_serdev_suspend, ludlc_serdev_resume)
	.prepare	= pm_sleep_ptr(ludlc_serdev_prepare),
	.complete	= pm_sleep_ptr(ludlc_serdev_complete),
};

/*
 * LuDLC has no assigned ACPI vendor ID, so ACPI systems describe the transport
 * with the generic PRP0001 device and this same compatible in _DSD. serdev
 * matching tries the ACPI companion first and reaches this table in both
 * cases.
 */
static const struct of_device_id ludlc_serdev_of_match[] = {
	{ .compatible = "ludlc,serdev" },
	{ }
};
MODULE_DEVICE_TABLE(of, ludlc_serdev_of_match);

static struct serdev_device_driver ludlc_serdev_driver = {
	.driver = {
		.name = "ludlc-serdev",
		.of_match_table = ludlc_serdev_of_match,
		.pm = pm_sleep_ptr(&ludlc_serdev_pm_ops),
	},
	.probe = ludlc_serdev_probe,
	.remove = ludlc_serdev_remove,
};

/**
 * @brief Settle the default framing, publish configfs, then bind the ports.
 *
 * In that order: a port can probe as soon as the driver registers, and it has
 * to find both of those ready. The checksums were settled when the core
 * loaded.
 */
static int __init ludlc_serdev_init(void)
{
	int ret;

	if (ludlc_framing_param) {
		const struct ludlc_framing *framing =
				ludlc_framing_lookup(ludlc_framing_param);

		if (!framing) {
			pr_err("ludlc_serdev: no framing called \"%s\"\n",
			       ludlc_framing_param);

			return -EINVAL;
		}

		if (!ludlc_serdev_framing_ok(framing)) {
			pr_err("ludlc_serdev: %s framing needs a transport "
				"that says where a frame ends\n",
			        framing->name);

			return -EINVAL;
		}

		ludlc_serdev_framing_dflt = framing;
	} else {
		ludlc_serdev_framing_dflt = ludlc_framing_default();
	}

	/* A size no link could take is refused here rather than per port. */
	if (ludlc_payload_param > CONFIG_LUDLC_MAX_PAYLOAD_SIZE) {
		pr_err("ludlc_serdev: max_payload=%u, this build allows %u\n",
		       ludlc_payload_param,
		       (unsigned int)CONFIG_LUDLC_MAX_PAYLOAD_SIZE);

		return -EINVAL;
	}

	ret = ludlc_serdev_cfg_init();
	if (!ret) {
		ret = serdev_device_driver_register(&ludlc_serdev_driver);
		if (ret)
			ludlc_serdev_cfg_deinit();
	}

	return ret;
}
module_init(ludlc_serdev_init);

/** @brief Release the ports first, then the configfs describing them. */
static void __exit ludlc_serdev_exit(void)
{
	serdev_device_driver_unregister(&ludlc_serdev_driver);
	ludlc_serdev_cfg_deinit();
}
module_exit(ludlc_serdev_exit);

MODULE_AUTHOR("Andrey Volkov <andrey@volkov.fr>");
MODULE_DESCRIPTION("LuDLC serdev transport");
MODULE_LICENSE("GPL");

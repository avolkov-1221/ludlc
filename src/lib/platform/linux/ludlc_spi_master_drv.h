/* SPDX-License-Identifier: GPL-2.0-only */
/**
 * @file ludlc_spi_master_drv.h
 *
 * @brief A LuDLC link over an SPI bus this system drives: the driver's side.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * The calls that open and close a link are in ludlc_spi_master.h, which every
 * port shares. This file adds what the Linux driver and its optional configfs
 * front-end need.
 *
 * The wire format is not this driver's to define: it is shared with whatever
 * runs on the peripheral, and lives in ludlc_spi_proto.h, which describes the
 * exchange, the announcement and the frame. What is here is the controller's
 * half. The checksum belongs to the link rather than to this transport: both
 * ends must agree on it, and they agree the same way as on any other wire. See
 * ludlc_csum.h.
 *
 * @section spi_attention Being heard
 *
 * An exchange happens when the local end has a frame to send, when the peer
 * raises the data-ready line firmware described for it, or - where there is no
 * such line - every @c ludlc,poll-interval-ms. The line may be described with
 * either an edge or a level trigger: it is handled in a threaded interrupt and
 * masked for as long as the exchange takes, so a peer that holds it asserted
 * until its queue has drained is served in one call rather than flooding the
 * handler. How long the peer needs between the two transfers of an exchange is
 * a property of its firmware, which states it as @c ludlc,turnaround-us.
 */

#ifndef __LUDLC_SPI_MASTER_DRV_H__
#define __LUDLC_SPI_MASTER_DRV_H__

#include <linux/cache.h>
#include <linux/mutex.h>
#include <linux/spi/spi.h>
#include <linux/workqueue.h>

#include <ludlc.h>
#include <ludlc_framing.h>
#include <ludlc_private.h>
#include <ludlc_spi_master.h>
#include <ludlc_spi_proto.h>

#include "ludlc_csum.h"
#include "ludlc_framing.h"

/** Half of the staging fifo: one SPI chunk while the other half is prepared. */
#define LUDLC_SPI_CHUNK		(LUDLC_TX_FIFO_SIZE / 2)

/**
 * @struct ludlc_spi_connection
 * @brief One LuDLC link on one SPI peripheral.
 */
struct ludlc_spi_connection {
	struct spi_device *spi;

	struct ludlc_connection conn;

	struct mutex xfer_lock;
	struct delayed_work xfer_work;

	int irq;
	u32 turnaround_us;
	unsigned long poll_period;

	/** Longest frame this link may announce or accept, once framed. */
	ludlc_frame_size_t frame_cap;

	/** Framing state of this link, allocated at open. Holds the packet
	 * going out and the frame coming in.
	 */
	struct ludlc_framing_state *fs;

	/**
	 * @brief Checksum this link runs, held for as long as it runs it.
	 *
	 * Taken with ludlc_link_csum_set() at open and released with
	 * ludlc_link_csum_drop() once the framing has stopped, since the code
	 * computing it may belong to another module.
	 */
	const struct ludlc_csum *csum;

	struct ludlc_spi_hdr tx_hdr ____cacheline_aligned;
	struct ludlc_spi_hdr rx_hdr;

	/** Pipelined encode/decode staging: one chunk each way. */
	u8 tx_chunk[LUDLC_SPI_CHUNK] ____cacheline_aligned;
	u8 rx_chunk[LUDLC_SPI_CHUNK];

	/** Decoded packet: a packet and its checksum. */
	u8 rx_pkt[LUDLC_FRAME_BUF_SIZE];
};

/** @brief The transport state of @p conn, which must belong to this driver. */
static inline
struct ludlc_spi_connection *to_spi_conn(struct ludlc_connection *conn)
{
	return container_of(conn, struct ludlc_spi_connection, conn);
}

/*
 * The peripheral as the configfs layer sees it: a plain device, since a port
 * directory is the same on every wire. See ludlc_cfg_port_ops.
 */

/** @brief The peripheral called @p name among those this driver is bound to. */
struct device *ludlc_spi_dev_find(const char *name);

/** @brief The link running on @p dev, or NULL if the peripheral is down. */
static inline struct ludlc_connection *ludlc_spi_get_conn(struct device *dev)
{
	struct ludlc_spi_connection *lsc = dev_get_drvdata(dev);

	return lsc ? &lsc->conn : NULL;
}

void ludlc_spi_default_args(struct device *dev, ludlc_platform_args_t *args);

static inline int ludlc_spi_link_up(struct device *dev,
				    const ludlc_platform_args_t *args,
				    struct ludlc_connection **conn)
{
	if (ludlc_spi_get_conn(dev))
		return -EBUSY;

	return ludlc_spi_master_connection_create(args, conn, NULL, NULL);
}

static inline void ludlc_spi_link_down(struct device *dev)
{
	struct ludlc_connection *conn = ludlc_spi_get_conn(dev);

	if (conn)
		ludlc_spi_master_connection_destroy(conn);
}

/**
 * @brief Can a link over an exchange carry @p framing?
 *
 * The exchange announces a frame before encoding it, so a framing whose wire
 * form is only known once escaped cannot be carried here. The rest may: what
 * the announcement says is what delimits the frame.
 */
static inline bool ludlc_spi_framing_ok(const struct ludlc_framing *framing)
{
	return framing->wire_len != NULL;
}

#if IS_ENABLED(CONFIG_CONFIGFS_FS)
bool ludlc_spi_cfg_apply(struct device *dev, ludlc_platform_args_t *args);
void ludlc_spi_cfg_bind(struct device *dev, struct ludlc_connection *conn);
void ludlc_spi_cfg_unbind(struct device *dev);
int ludlc_spi_cfg_init(void);
void ludlc_spi_cfg_deinit(void);
#else
static inline bool ludlc_spi_cfg_apply(struct device *dev,
				       ludlc_platform_args_t *args)
{
	return false;
}

static inline void ludlc_spi_cfg_bind(struct device *dev,
				      struct ludlc_connection *conn)
{ }

static inline void ludlc_spi_cfg_unbind(struct device *dev)
{ }

static inline int ludlc_spi_cfg_init(void)
{
	return 0;
}

static inline void ludlc_spi_cfg_deinit(void)
{ }
#endif

#endif /* __LUDLC_SPI_MASTER_DRV_H__ */

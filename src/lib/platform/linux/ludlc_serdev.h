/* SPDX-License-Identifier: GPL-2.0-only */
/**
 * @file ludlc_serdev.h
 *
 * @brief Interface between the LuDLC serdev transport driver and its optional
 * configfs front-end.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * A link is created either by the transport probe, when firmware describes the
 * port as available, or by configfs when it does not. The driver interface
 * below is what the configfs front-end needs for the second case. The checksum
 * a link runs is the core's business rather than this driver's; see
 * ludlc_csum.h.
 */

#ifndef __LUDLC_SERDEV_H__
#define __LUDLC_SERDEV_H__

#include <linux/kfifo.h>
#include <linux/serdev.h>

#include <ludlc.h>
#include <ludlc_framing.h>
#include <ludlc_private.h>
#include <ludlc_serial.h>

#include "ludlc_csum.h"
#include "ludlc_framing.h"
#include "ludlc_platform_args.h"

/** @brief Lower bound on the TX fifo size. */
#define LUDLC_SERDEV_TX_MIN_SZ	64

/**
 * @brief TX fifo size, in encoded octets. A power of two, as kfifo requires.
 *
 * Independent of the frame size: the link sets how large a payload may be
 * (@c max_payload), and the framing writes encoded octets into this fifo as
 * it produces them. COBS's stuffing group (at most 255 octets) is not the
 * frame; what the fifo has no room for waits in the encoder until it does.
 */
#define LUDLC_SERDEV_TX_BUF_SZ						\
	roundup_pow_of_two(LUDLC_TX_FIFO_SIZE < LUDLC_SERDEV_TX_MIN_SZ ?	\
			   LUDLC_SERDEV_TX_MIN_SZ : LUDLC_TX_FIFO_SIZE)

static_assert((LUDLC_SERDEV_TX_BUF_SZ & (LUDLC_SERDEV_TX_BUF_SZ - 1)) == 0,
	      "kfifo masks with the size, so it must be a power of two");
static_assert(LUDLC_SERDEV_TX_BUF_SZ >= LUDLC_SERDEV_TX_MIN_SZ,
	      "the TX fifo is smaller than the floor this port asks for");

/**
 * @struct ludlc_serial_connection
 * @brief One LuDLC link on one serdev port.
 */
struct ludlc_serial_connection {
	struct serdev_device *serdev;

	struct ludlc_connection conn;

	/**
	 * Framing state of this link, allocated at open. TX and RX drive it
	 * an octet at a time. The encoder belongs to @c tx_work: call
	 * cancel_work_sync() before touching it from elsewhere.
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

	struct work_struct tx_work;
	DECLARE_KFIFO(tx_fifo, u8, LUDLC_SERDEV_TX_BUF_SZ);

	/** Decoded packet: a packet and its checksum. */
	u8 rx_buf[LUDLC_FRAME_BUF_SIZE];
};

static inline
struct ludlc_serial_connection *to_serial_conn(struct ludlc_connection *conn)
{
	return container_of(conn, struct ludlc_serial_connection, conn);
}

/*
 * The port as the configfs layer sees it: a plain device, since a port
 * directory is the same on every wire. See ludlc_cfg_port_ops.
 */

/** @brief The port called @p name among those this driver is bound to. */
struct device *ludlc_serdev_dev_find(const char *name);

/** @brief The link running on the port @p dev, or NULL while it is down. */
static inline
struct ludlc_connection *ludlc_serdev_get_conn(struct device *dev)
{
	struct ludlc_serial_connection *lsc = dev_get_drvdata(dev);

	return lsc ? &lsc->conn : NULL;
}

void ludlc_serdev_default_args(struct device *dev,
			       ludlc_platform_args_t *args);

static inline int ludlc_serdev_link_up(struct device *dev,
				       const ludlc_platform_args_t *args,
				       struct ludlc_connection **conn)
{
	if (ludlc_serdev_get_conn(dev))
		return -EBUSY;

	return ludlc_serial_connection_create(args, conn, NULL);
}

static inline void ludlc_serdev_link_down(struct device *dev)
{
	struct ludlc_connection *conn = ludlc_serdev_get_conn(dev);

	if (conn)
		ludlc_serial_connection_destroy(conn);
}

/**
 * @brief Can a link over a port carry @p framing?
 *
 * A port hands over a stream of octets, so a framing that has to be told
 * where a frame ends would never complete one here.
 */
static inline bool ludlc_serdev_framing_ok(const struct ludlc_framing *framing)
{
	return !framing->needs_wire_len;
}

#if IS_ENABLED(CONFIG_CONFIGFS_FS)
bool ludlc_serdev_cfg_apply(struct device *dev, ludlc_platform_args_t *args);
void ludlc_serdev_cfg_bind(struct device *dev, struct ludlc_connection *conn);
void ludlc_serdev_cfg_unbind(struct device *dev);
int ludlc_serdev_cfg_init(void);
void ludlc_serdev_cfg_deinit(void);
#else
static inline bool ludlc_serdev_cfg_apply(struct device *dev,
					  ludlc_platform_args_t *args)
{
	return false;
}

static inline void ludlc_serdev_cfg_bind(struct device *dev,
					 struct ludlc_connection *conn)
{ }

static inline void ludlc_serdev_cfg_unbind(struct device *dev)
{ }

static inline int ludlc_serdev_cfg_init(void)
{
	return 0;
}

static inline void ludlc_serdev_cfg_deinit(void)
{ }
#endif

#endif /* __LUDLC_SERDEV_H__ */

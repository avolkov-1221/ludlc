// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file ludlc_spi_slave.h
 *
 * @brief A LuDLC link over an SPI bus driven by another controller.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * The wire format is shared with the controller on the other end of the bus
 * and is described in ludlc_spi_proto.h; read that first. This file covers
 * the peripheral half, written against the ludlc_spi_master driver. A
 * peripheral cannot start an exchange, only be ready for one, so the single
 * thread this transport runs stays inside a transfer nearly all of the time.
 * When it has something to send it raises the data-ready line - see
 * @c ludlc_platform_args_t - and waits for the controller to clock it.
 *
 * @section spi_cb What runs where
 *
 * One thread does everything, so @c on_recv and @c on_confirm run on it,
 * between one transfer and another. While they run this end is not armed and
 * an exchange the controller clocks meanwhile is lost: the framing recovers
 * within an exchange or two, but the round is wasted. Callbacks must
 * therefore queue work rather than do it inline. Sending from a callback is
 * cheap: it raises the data-ready line and returns, and the frame goes out in
 * a later exchange. Both ends must agree on the frame size, which is what an
 * announcement is checked against, and on the checksum.
 *
 * @section spi_slave_size How large a frame
 *
 * The link's @c max_payload plus a header and a checksum, capped by
 * @ref LUDLC_MAX_FRAME_SIZE, which sizes the two frame buffers here. This end
 * usually has the less memory of the two, so it limits what the link can
 * carry: a controller announcing more is refused.
 */

#ifndef __LUDLC_SPI_SLAVE_H__
#define __LUDLC_SPI_SLAVE_H__

#include <ludlc.h>

/**
 * @brief Bring up a LuDLC link on an SPI bus this system does not drive.
 *
 * Queues the handshake before returning; the controller has to clock the
 * exchanges that carry it.
 *
 * @param args The SPI peripheral, its chip select and mode, the data-ready line
 * if the board has one, the checksum the link runs, NULL for none, the cfg to
 * open it with, and the priority of the link's thread. See
 * @c ludlc_platform_args_t, and
 * LUDLC_SPI_PERIPHERAL_ARGS_DT() in ludlc_dt.h to take them from the
 * devicetree.
 * @param conn Where the new connection goes. Set to NULL on failure.
 * @param cb Callbacks the link reports to. Must outlive the connection.
 * @param user_arg User data passed to the event callbacks as is.
 * @return 0, or a negative errno.
 */
int ludlc_spi_slave_connection_create(const ludlc_platform_args_t *args,
				      struct ludlc_connection **conn,
				      const struct ludlc_conn_cb *cb,
				      void *user_arg);

/**
 * @brief Take a LuDLC SPI link down and free what it held.
 *
 * Sends a DISCONNECT first, so that the controller need not wait out its
 * watchdog, and waits a bounded time for the exchange that carries it. This
 * requires the controller to keep clocking: a peripheral cannot end a
 * transfer it did not start, so if the bus goes quiet mid-exchange the
 * transfer stays armed over buffers the SPI driver owns, and the connection
 * is leaked rather than freed underneath the driver. An error is logged when
 * that happens.
 *
 * @param conn The connection. Not to be used afterwards.
 */
void ludlc_spi_slave_connection_destroy(struct ludlc_connection *conn);

/**
 * @brief Fill in @p args for the link a "ludlc,spi-peripheral" node
 * describes, or else for spi1, with no data-ready line and CRC-16/KERMIT.
 *
 * The checksum is the one a Linux controller runs by default. Both ends must
 * run the same one, so change it to match the controller.
 *
 * @param args Where to write. Ignored if NULL.
 */
void ludlc_default_spi_slave_platform_args(ludlc_platform_args_t *args);

#endif /* __LUDLC_SPI_SLAVE_H__ */

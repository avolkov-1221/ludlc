// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only)
/**
 * @file ludlc_spi_master.h
 *
 * @brief Public API definitions for the LuDLC SPI controller transport.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * Declares the calls that create and destroy a LuDLC connection over an SPI
 * bus this system drives. The wire format is shared with the peripheral on
 * the other end of the bus and is described in ludlc_spi_proto.h; the calls
 * of the peripheral's half are in ludlc_spi_slave.h.
 */

#ifndef __LUDLC_SPI_MASTER_H__
#define __LUDLC_SPI_MASTER_H__

#include <ludlc.h>

/**
 * @brief Open a LuDLC link on an SPI peripheral.
 *
 * @param args Platform arguments: the SPI device, the checksum to run, the
 * timings the peer needs and the cfg to open the link with, clock included.
 * @param[out] conn The link, on success.
 * @param cb Pointer to the user-facing event callbacks. Must be NULL on
 * Linux: there the events belong to the bus, which passes them to the driver
 * bound to the channel they concern.
 * @return 0, or a negative errno with nothing left open.
 */
int ludlc_spi_master_connection_create(const ludlc_platform_args_t *args,
				       struct ludlc_connection **conn,
				       const struct ludlc_conn_cb *cb);

/**
 * @brief Take the link down and free it.
 *
 * @param conn The link, as returned by ludlc_spi_master_connection_create().
 */
void ludlc_spi_master_connection_destroy(struct ludlc_connection *conn);

#endif /* __LUDLC_SPI_MASTER_H__ */

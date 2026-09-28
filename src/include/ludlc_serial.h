// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only)
/**
 * @file ludlc_serial.h
 *
 * @brief Public API definitions for the LuDLC serial transport.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 */

#ifndef __LUDLC_SERIAL_H__
#define __LUDLC_SERIAL_H__

#include <ludlc.h>

/**
 * @brief Creates and initializes a new LuDLC serial connection.
 *
 * This function allocates resources, initializes the connection state,
 * and typically spawns the TX and RX threads for a serial transport.
 *
 * @param args Platform arguments: the port, its line settings, the
 * @ref ludlc_csum to run or NULL for none, and the @ref ludlc_conn_cfg to
 * open the link with, clock included.
 * @param[out] conn A pointer to a `struct ludlc_connection*` that will be
 * updated to point to the newly created connection.
 * @param cb Pointer to the user-facing event callbacks.
 * @return 0 on success, or a negative error code on failure.
 */
int ludlc_serial_connection_create(
		const ludlc_platform_args_t *args,
		struct ludlc_connection **conn,
		const struct ludlc_conn_cb *cb
		);

/**
 * @brief Fill @p args with what this port opens a serial link with.
 *
 * A starting point for an application with no settings of its own: overwrite
 * what it does not want and pass it to ludlc_serial_connection_create(). Not
 * every port provides one; Linux takes its settings from the device tree or
 * from configfs instead.
 */
void ludlc_default_serial_platform_args(ludlc_platform_args_t *args);

/**
 * @brief Destroys and cleans up a LuDLC serial connection.
 *
 * This function signals the TX and RX threads to terminate, waits for
 * them to join, and frees all resources associated with the connection.
 *
 * @param conn Pointer to the connection to be destroyed.
 */
void ludlc_serial_connection_destroy(struct ludlc_connection *conn);

#endif /* __LUDLC_SERIAL_H__ */

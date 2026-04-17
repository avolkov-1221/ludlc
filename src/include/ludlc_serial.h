// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file ludlc_serial.h
 *
 * @brief Public API definitions for the LuDLC serial transport.
 *
 * This file declares the functions related to creating, destroying,
 * and managing a LuDLC connection specifically over a serial (byte-stream)
 * transport. It includes the entry points for the TX/RX threads.
 *
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * This file is licensed under either the Apache License, Version 2.0,
 * or the GNU General Public License, version 2 or (at your option)
 * any later version.
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
 * @param args Pointer to the platform-specific arguments (e.g., device name,
 * baud rate).
 * @param[out] conn A pointer to a `struct ludlc_connection*` that will be
 * updated to point to the newly created connection.
 * @param proto Pointer to protocol services (checksum and timestamp hooks).
 * @param cb Pointer to the user-facing event callbacks.
 * @return 0 on success, or a negative error code on failure.
 */
int ludlc_serial_connection_create(const ludlc_platform_args_t *args,
		struct ludlc_connection **conn,
		const struct ludlc_proto_cb *proto,
		const struct ludlc_conn_cb *cb
		);

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

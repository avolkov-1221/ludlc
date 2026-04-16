// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file ludlc_echo.c
 *
 * @brief Simple Zephyr demo for LuDLC: send "Hello" and wait for echo.
 *
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * This file is licensed under either the Apache License, Version 2.0,
 * or the GNU General Public License, version 2 or (at your option)
 * any later version.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>

#include <string.h>

#include <ludlc.h>
#include <ludlc_serial.h>

LOG_MODULE_REGISTER(ludlc_demo, CONFIG_LOG_DEFAULT_LEVEL);

/* Semaphore to signal that the echo packet was received */
K_SEM_DEFINE(packet_received_sem, 0, 1);

/* Define the channel for our echo test */
#define ECHO_CHANNEL (CONFIG_LUDLC_CONTROL_CHANNEL + 1)

static const char TEST_PACKET[] = "Hello";
static const size_t TEST_PACKET_LEN = sizeof(TEST_PACKET);

/* --- LuDLC Callbacks --- */

static void on_disconnect(struct ludlc_connection *conn, void *user_arg)
{
	LOG_INF("Connection lost (%p)", conn);
}

static void on_connect(struct ludlc_connection *conn, void *user_arg)
{
	LOG_INF("Connection established (%p)", conn);

	/* Connection is up, send our test packet */
	int ret = ludlc_enqueue_data(conn, ECHO_CHANNEL, TEST_PACKET,
			TEST_PACKET_LEN,
			false /* not oneshot */);

	if (ret) {
		LOG_ERR("Failed to enqueue packet: %d", ret);
	} else {
		LOG_INF("Test packet enqueued!");
	}
}

static void on_recv(struct ludlc_connection *conn, ludlc_channel_t chan,
		const void *data, ludlc_payload_size_t data_size,
		ludlc_timestamp_t tstamp, void *user_arg)
{
	LOG_INF("Packet received! chan=%u, size=%u", chan, data_size);

	/* Check if it's the echo we were waiting for */
	if (chan == ECHO_CHANNEL && data_size == TEST_PACKET_LEN &&
			memcmp(data, TEST_PACKET, TEST_PACKET_LEN) == 0) {
		LOG_INF("Received our echo packet!");
		k_sem_give(&packet_received_sem);
	}
}

static void on_confirm(struct ludlc_connection *conn, int error,
		ludlc_channel_t chan, const void *data,
		ludlc_payload_size_t data_size, void *user_arg)
{
	if (error) {
		LOG_ERR("Packet failed to confirm: %d", error);
	} else {
		LOG_INF("Packet confirmed (chan=%u, size=%u)", chan, data_size);
	}
}

static struct ludlc_conn_cb demo_callbacks = {
	.on_disconnect = on_disconnect,
	.on_connect = on_connect,
	.on_recv = on_recv,
	.on_confirm = on_confirm,
};

/* --- Main --- */
int main(void)
{
	struct ludlc_connection *conn = NULL;
	int ret;
	ludlc_platform_args_t ser_arg;

	LOG_INF("Starting LuDLC Zephyr Demo...");

	/* Get default serial port (zephyr,console) */
	/* This will be defined in src/lib/platform/zephyr/ludlc_zephyr_serial.c */
	ludlc_default_serial_platform_args(&ser_arg);

	LOG_INF("Creating connection on %s...", ser_arg.dev->name);
	ret = ludlc_serial_connection_create(&ser_arg, &conn, &demo_callbacks);
	if (ret || !conn) {
		LOG_ERR("Failed to create LuDLC connection: %d", ret);
		return 0;
	}

	/*
	 * Wait for the on_connect callback to fire, send a packet,
	 * and the on_recv callback to get the echo.
	 * We'll wait up to 10 seconds.
	 */
	LOG_INF("Waiting for echo packet...");
	ret = k_sem_take(&packet_received_sem, K_SECONDS(10));

	if (ret == 0) {
		LOG_INF("--- TEST SUCCESS ---");
	} else {
		LOG_ERR("--- TEST FAILED (Timeout) ---");
	}

	/* Clean up and destroy the connection */
	LOG_INF("Destroying connection...");
	ludlc_serial_connection_destroy(conn);

	LOG_INF("Demo finished.");
	return 0;
}

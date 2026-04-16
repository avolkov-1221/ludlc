// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file ludlc_echo.c
 *
 * @brief Zephyr LuDLC echo counterpart sample.
 *
 * This sample mirrors the ECHO channel logic used by `ludlc_demo`:
 * - receives packets on the ECHO channel
 * - decrements payload[0] as a simple hop/TTL counter
 * - sends the packet back while payload[0] is non-zero
 *
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/crc.h>

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <ludlc.h>
#include <ludlc_serial.h>

#undef CONFIG_LUDLC_ECHO_PERIODIC_TX

LOG_MODULE_REGISTER(ludlc_echo, LOG_LEVEL_DBG);

#define ECHO_CHANNEL		1
#define ECHO_MAX_PAYLOAD	64U

#ifndef CONFIG_LUDLC_ECHO_PERIODIC_TX
#define CONFIG_LUDLC_ECHO_PERIODIC_TX 0
#endif

static uint8_t echo_tx_buf[ECHO_MAX_PAYLOAD];
static atomic_t tx_flags;

enum {
	ECHO_TX_BUSY_F = 0,
#if CONFIG_LUDLC_ECHO_PERIODIC_TX
	PERIODIC_TX_BUSY_F,
#endif
};

static ludlc_csum_t ludlc_csum_byte(ludlc_csum_t crc, uint8_t data)
{
	return crc16_ccitt(crc, &data, 1);
}

static struct ludlc_proto_cb proto = {
	.csum_byte = ludlc_csum_byte,
	.get_timestamp = ludlc_default_get_timestamp,
};

static int send_echo_resp(struct ludlc_connection *conn, const uint8_t *data,
			  ludlc_payload_size_t size)
{
	int ret;

	if (!conn || !data || size == 0U) {
		return -EINVAL;
	}

	if (size > sizeof(echo_tx_buf)) {
		LOG_WRN("Drop oversized echo payload (%u > %u)",
			(unsigned int)size, (unsigned int)sizeof(echo_tx_buf));
		return -EMSGSIZE;
	}

	memcpy(echo_tx_buf, data, size);
	if (echo_tx_buf[0] == 0U) {
		/* Same guard as ludlc_demo: stop ping-pong when TTL is zero. */
		return 0;
	}

	if (atomic_test_and_set_bit(&tx_flags, ECHO_TX_BUSY_F)) {
		LOG_WRN("Drop echo response: previous ECHO packet still pending");
		return -EBUSY;
	}

	echo_tx_buf[0]--;
	ret = ludlc_enqueue_data(conn, ECHO_CHANNEL, echo_tx_buf, size, 1);
	if (ret) {
		atomic_clear_bit(&tx_flags, ECHO_TX_BUSY_F);
		LOG_ERR("Failed to enqueue echo response: %d", ret);
	}

	return ret;
}

#if CONFIG_LUDLC_ECHO_PERIODIC_TX
#define PERIODIC_TX_INTERVAL K_SECONDS(1)

static uint8_t periodic_tx_buf[] = "\2Hello World!";

static int periodic_tx_tick(struct ludlc_connection *conn)
{
	int ret;

	if (!conn) {
		return -EINVAL;
	}

	if (atomic_test_and_set_bit(&tx_flags, PERIODIC_TX_BUSY_F)) {
		return -EBUSY;
	}

	ret = ludlc_enqueue_data(conn, ECHO_CHANNEL, periodic_tx_buf,
				 sizeof(periodic_tx_buf), 1);
	if (ret) {
		atomic_clear_bit(&tx_flags, PERIODIC_TX_BUSY_F);
		LOG_ERR("Failed to enqueue periodic packet: %d", ret);
		return ret;
	}

	LOG_INF("Periodic ECHO packet queued");
	return 0;
}

static inline void periodic_tx_on_confirm(ludlc_channel_t chan, const void *data)
{
	if (chan == ECHO_CHANNEL && data == periodic_tx_buf) {
		atomic_clear_bit(&tx_flags, PERIODIC_TX_BUSY_F);
	}
}
#else
#define PERIODIC_TX_INTERVAL K_SECONDS(1)

static inline int periodic_tx_tick(struct ludlc_connection *conn)
{
	ARG_UNUSED(conn);
	return 0;
}

static inline void periodic_tx_on_confirm(ludlc_channel_t chan, const void *data)
{
	ARG_UNUSED(chan);
	ARG_UNUSED(data);
}
#endif

static void on_disconnect(struct ludlc_connection *conn, void *user_arg)
{
	ARG_UNUSED(user_arg);
	LOG_INF("Connection lost (%p)", conn);
}

static void on_connect(struct ludlc_connection *conn, void *user_arg)
{
	ARG_UNUSED(user_arg);
	LOG_INF("Connection established (%p)", conn);
}

static void on_recv(struct ludlc_connection *conn, ludlc_channel_t chan,
		    const void *data, ludlc_payload_size_t data_size,
		    ludlc_timestamp_t tstamp, void *user_arg)
{
	int ret;

	ARG_UNUSED(user_arg);
	LOG_INF("RX packet: chan=%u size=%u ts=%llu",
		(unsigned int)chan, (unsigned int)data_size,
		(unsigned long long)tstamp);

	if (!data || data_size == 0U) {
		return;
	}

	if (chan != ECHO_CHANNEL) {
		return;
	}

	ret = send_echo_resp(conn, data, data_size);
	if (ret == 0) {
		LOG_INF("ECHO response queued");
	}
}

static void on_confirm(struct ludlc_connection *conn, int error,
		       ludlc_channel_t chan, const void *data,
		       ludlc_payload_size_t data_size, void *user_arg)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(user_arg);

	if (chan == ECHO_CHANNEL && data == echo_tx_buf) {
		atomic_clear_bit(&tx_flags, ECHO_TX_BUSY_F);
	}

	periodic_tx_on_confirm(chan, data);

	if (error) {
		LOG_ERR("TX confirm failed: chan=%u err=%d size=%u",
			(unsigned int)chan, error, (unsigned int)data_size);
		return;
	}

	LOG_INF("TX confirm: chan=%u size=%u",
		(unsigned int)chan, (unsigned int)data_size);
}

static struct ludlc_conn_cb demo_callbacks = {
	.on_disconnect = on_disconnect,
	.on_connect = on_connect,
	.on_recv = on_recv,
	.on_confirm = on_confirm,
};

int main(void)
{
	struct ludlc_connection *conn = NULL;
	int ret;
	static const ludlc_platform_args_t ser_arg = {
		.dev = DEVICE_DT_GET(DT_NODELABEL(uart0)),
	};

	LOG_INF("Starting Zephyr LuDLC echo");
	LOG_INF("Using UART device: %s", ser_arg.dev->name);

	ret = ludlc_serial_connection_create(&ser_arg, &conn, &proto,
					     &demo_callbacks);
	if (ret || !conn) {
		LOG_ERR("Failed to create LuDLC connection: %d", ret);
		return 0;
	}

	LOG_INF("Echo ready on channel %u", (unsigned int)ECHO_CHANNEL);

	while (1) {
		ret = periodic_tx_tick(conn);
		if (ret && ret != -EBUSY) {
			LOG_WRN("Periodic enqueue returned %d", ret);
		}

		k_sleep(PERIODIC_TX_INTERVAL);
	}

	return 0;
}

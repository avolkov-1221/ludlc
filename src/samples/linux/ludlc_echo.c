// SPDX-License-Identifier: GPL-2.0-only
/**
 * @file linux/ludlc_echo.c
 *
 * @brief Echo responder for one device on the LuDLC bus.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * A packet on a channel this device owns is copied and sent back with its
 * first octet decremented. A zero there ends the exchange.
 *
 * ludlc_demo and the Zephyr echo sample use that octet as a hop count and
 * put the exchange on the channel after the control channel, which is where
 * this node belongs. Match it with compatible "ludlc,echo". The copy is what
 * the core holds until the peer acknowledges the reply.
 *
 * Example:
 * port=/sys/kernel/config/ludlc-serdev/serial0-0
 * mkdir $port/ludlc/echo
 * echo "ludlc,echo" > $port/ludlc/echo/compatible
 * echo 1            > $port/ludlc/echo/reg
 * echo 1            > $port/ludlc/echo/live
 */

#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sprintf.h>

#include <ludlc.h>
#include <ludlc_bus.h>

/* Eight octets a line, same layout as ludlc_demo. */
static void ludlc_echo_dump(struct device *dev, ludlc_channel_t chan,
			    const void *data, ludlc_payload_size_t size,
			    ludlc_timestamp_t tstamp)
{
	const u8 *payload = data;
	char hex_part[3 * 8 + 1];
	char ascii_part[8 + 1];
	ludlc_payload_size_t i;

	dev_info(dev, "packet received @%llu on channel %u\n",
		 (unsigned long long)tstamp, chan);

	if (!size || !payload) {
		dev_info(dev, "payload(len=0): (empty)\n");
		return;
	}

	dev_info(dev, "payload(len=%zu):\n", (size_t)size);

	for (i = 0; i < size; i += 8) {
		size_t j, pos = 0;
		size_t chunk = (size - i) < 8 ? (size_t)(size - i) : 8;

		for (j = 0; j < 8; j++) {
			if (j < chunk) {
				u8 c = *payload++;

				pos += snprintf(hex_part + pos,
						sizeof(hex_part) - pos,
						"%02X ", c);
				ascii_part[j] = (c >= 32 && c <= 126) ? c : '.';
			} else {
				pos += snprintf(hex_part + pos,
						sizeof(hex_part) - pos, "   ");
				ascii_part[j] = ' ';
			}
		}
		ascii_part[8] = '\0';
		dev_info(dev, "  %04zu: %-24s |%s|\n", (size_t)i, hex_part,
			 ascii_part);
	}
}

static void ludlc_echo_receive(struct ludlc_device *ldev, ludlc_channel_t chan,
			       const void *data, ludlc_payload_size_t size,
			       ludlc_timestamp_t tstamp)
{
	u8 *buf;
	int ret;

	ludlc_echo_dump(&ldev->dev, chan, data, size, tstamp);

	if (!data || !size || !*(const u8 *)data)
		return;

	buf = kmemdup(data, size, GFP_KERNEL);
	if (!buf)
		return;

	buf[0]--;

	ret = ludlc_enqueue_data(ldev->conn, chan, buf, size, 1);
	if (ret) {
		kfree(buf);
		dev_warn_ratelimited(&ldev->dev, "cannot queue the echo: %d\n",
				     ret);
	}
}

static void ludlc_echo_confirm(struct ludlc_device *ldev, ludlc_channel_t chan,
			       int error, const void *data,
			       ludlc_payload_size_t size)
{
	(void)chan;
	(void)size;

	if (error)
		dev_warn_ratelimited(&ldev->dev,
				     "echo was not acknowledged: %d\n", error);

	kfree(data);
}

static int ludlc_echo_probe(struct ludlc_device *ldev)
{
	dev_info(&ldev->dev, "echo responder\n");
	return 0;
}

static const struct of_device_id ludlc_echo_of_match[] = {
	{ .compatible = "ludlc,echo", },
	{ }
};
MODULE_DEVICE_TABLE(of, ludlc_echo_of_match);

static struct ludlc_driver ludlc_echo_driver = {
	.probe		= ludlc_echo_probe,
	.receive	= ludlc_echo_receive,
	.confirm	= ludlc_echo_confirm,
	.drv = {
		.name		= "ludlc-echo",
		.of_match_table	= ludlc_echo_of_match,
	},
};
module_ludlc_driver(ludlc_echo_driver);

MODULE_AUTHOR("Andrey Volkov <andrey@volkov.fr>");
MODULE_DESCRIPTION("LuDLC echo responder");
MODULE_LICENSE("GPL");
MODULE_SOFTDEP("pre: ludlc_core");

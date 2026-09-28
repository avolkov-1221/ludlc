// SPDX-License-Identifier: GPL-2.0-only
/**
 * @file ludlc_spi_cfg.c
 *
 * @brief Optional configfs description of a LuDLC SPI peripheral, for a system
 * where neither DT nor ACPI describes the LuDLC link on it.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * The port directory itself, and everything a link is made of, is in
 * ludlc_cfg_port.c; what is here is the exchange:
 *
 * @verbatim
 *   /sys/kernel/config/ludlc-spi/
 *       <dev>/                one directory per peripheral this system drives
 *           turnaround_us     gap the peer needs between the two transfers
 *           poll_interval_ms  how often to ask a peer that cannot ask us
 *           csum, framing, max_payload, live, ludlc/    - see ludlc_cfg_port.c
 * @endverbatim
 *
 * `<dev>` must be named after the SPI device it describes (as shown by
 * dev_name(), e.g. "spi0.0"); the settings are applied when that device
 * probes.
 *
 * The peripheral itself is not created here: a system that describes one well
 * enough for the SPI core to register it has said where it sits on the bus,
 * how fast and in which mode. What it has no way of saying is that LuDLC runs
 * over it, and that is what this directory is for.
 */

#include <linux/bitops.h>
#include <linux/cleanup.h>
#include <linux/configfs.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>

#include "ludlc_bus.h"
#include "ludlc_spi_master.h"

/* The bits this port keeps beyond the ones every port has. */
#define LUDLC_SPI_CFG_F_TURNAROUND_SET	(LUDLC_CFG_PORT_F_BUS + 0)
#define LUDLC_SPI_CFG_F_POLL_SET	(LUDLC_CFG_PORT_F_BUS + 1)

static struct configfs_subsystem ludlc_spi_cfg_subsys;

/*
 * The timings of the exchange. Each reads back empty until written, and what
 * is written overrides firmware for this peripheral only. They take effect the
 * next time the link is brought up, either from here or from the transport
 * probe.
 */
static ssize_t ludlc_spi_port_turnaround_us_show(struct config_item *item,
						 char *page)
{
	struct ludlc_cfg_port *port = to_ludlc_cfg_port(item);

	guard(mutex)(&port->lock);

	if (!test_bit(LUDLC_SPI_CFG_F_TURNAROUND_SET, &port->flags))
		return sysfs_emit(page, "\n");

	return sysfs_emit(page, "%u\n", port->args.spi.turnaround_us);
}

/**
 * @brief How long the peer is given between the two transfers of an exchange.
 *
 * A property of the peer's firmware rather than of this system, so there is
 * nothing to check it against here beyond what the field can hold: too short
 * and the peer misses the second transfer, too long and every exchange pays
 * for it.
 */
static ssize_t ludlc_spi_port_turnaround_us_store(struct config_item *item,
						  const char *page,
						  size_t count)
{
	struct ludlc_cfg_port *port = to_ludlc_cfg_port(item);
	u32 turnaround_us;
	int ret;

	ret = kstrtou32(page, 0, &turnaround_us);
	if (ret)
		return ret;

	guard(mutex)(&port->lock);
	port->args.spi.turnaround_us = turnaround_us;
	__set_bit(LUDLC_SPI_CFG_F_TURNAROUND_SET, &port->flags);

	return count;
}
CONFIGFS_ATTR(ludlc_spi_port_, turnaround_us);

static ssize_t ludlc_spi_port_poll_interval_ms_show(struct config_item *item,
						    char *page)
{
	struct ludlc_cfg_port *port = to_ludlc_cfg_port(item);

	guard(mutex)(&port->lock);

	if (!test_bit(LUDLC_SPI_CFG_F_POLL_SET, &port->flags))
		return sysfs_emit(page, "\n");

	return sysfs_emit(page, "%u\n", port->args.spi.poll_interval_ms);
}

/**
 * @brief How often to ask a peer that has no way of asking us, or 0 for never.
 *
 * Only a link whose peer was described without a data-ready line pays this:
 * with one, an exchange happens when there is a reason for it.
 */
static ssize_t ludlc_spi_port_poll_interval_ms_store(struct config_item *item,
						     const char *page,
						     size_t count)
{
	struct ludlc_cfg_port *port = to_ludlc_cfg_port(item);
	u32 poll_interval_ms;
	int ret;

	ret = kstrtou32(page, 0, &poll_interval_ms);
	if (ret)
		return ret;

	guard(mutex)(&port->lock);
	port->args.spi.poll_interval_ms = poll_interval_ms;
	__set_bit(LUDLC_SPI_CFG_F_POLL_SET, &port->flags);

	return count;
}
CONFIGFS_ATTR(ludlc_spi_port_, poll_interval_ms);

/** @brief Apply the timings the user wrote to @p args. */
static void ludlc_spi_cfg_override(const struct ludlc_cfg_port *port,
				   ludlc_platform_args_t *args)
{
	if (test_bit(LUDLC_SPI_CFG_F_TURNAROUND_SET, &port->flags))
		args->spi.turnaround_us = port->args.spi.turnaround_us;

	if (test_bit(LUDLC_SPI_CFG_F_POLL_SET, &port->flags))
		args->spi.poll_interval_ms = port->args.spi.poll_interval_ms;
}

static const struct ludlc_cfg_port_ops ludlc_spi_cfg_port_ops = {
	.dev_find	= ludlc_spi_dev_find,
	.default_args	= ludlc_spi_default_args,
	.override	= ludlc_spi_cfg_override,
	.link_up	= ludlc_spi_link_up,
	.link_down	= ludlc_spi_link_down,
	.get_conn	= ludlc_spi_get_conn,
	.framing_ok	= ludlc_spi_framing_ok,
};

static struct configfs_attribute *ludlc_spi_port_attrs[] = {
	&ludlc_spi_port_attr_turnaround_us,
	&ludlc_spi_port_attr_poll_interval_ms,
	LUDLC_CFG_PORT_ATTRS,
	NULL,
};

static const struct config_item_type ludlc_spi_port_type = {
	.ct_item_ops	= &ludlc_cfg_port_item_ops,
	.ct_attrs	= ludlc_spi_port_attrs,
	.ct_owner	= THIS_MODULE,
};

/** @brief mkdir: a port describing the SPI device named @p name. */
static struct config_group *ludlc_spi_cfg_make_port(struct config_group *group,
						    const char *name)
{
	return ludlc_cfg_port_make(group, name, &ludlc_spi_cfg_port_ops,
				   &ludlc_spi_port_type);
}

static struct configfs_group_operations ludlc_spi_cfg_group_ops = {
	.make_group = ludlc_spi_cfg_make_port,
};

static const struct config_item_type ludlc_spi_cfg_subsys_type = {
	.ct_group_ops	= &ludlc_spi_cfg_group_ops,
	.ct_owner	= THIS_MODULE,
};

static struct configfs_subsystem ludlc_spi_cfg_subsys = {
	.su_group = {
		.cg_item = {
			.ci_namebuf = "ludlc-spi",
			.ci_type    = &ludlc_spi_cfg_subsys_type,
		},
	},
};

bool ludlc_spi_cfg_apply(struct device *dev, ludlc_platform_args_t *args)
{
	return ludlc_cfg_port_apply(&ludlc_spi_cfg_subsys, dev, args);
}

void ludlc_spi_cfg_bind(struct device *dev, struct ludlc_connection *conn)
{
	ludlc_cfg_port_bind(&ludlc_spi_cfg_subsys, dev, conn);
}

void ludlc_spi_cfg_unbind(struct device *dev)
{
	ludlc_cfg_port_unbind(&ludlc_spi_cfg_subsys, dev);
}

/** @brief Create /sys/kernel/config/ludlc-spi/. */
int ludlc_spi_cfg_init(void)
{
	return ludlc_cfg_port_subsys_register(&ludlc_spi_cfg_subsys);
}

/** @brief Remove the subsystem, ports and all. */
void ludlc_spi_cfg_deinit(void)
{
	ludlc_cfg_port_subsys_unregister(&ludlc_spi_cfg_subsys);
}

// SPDX-License-Identifier: GPL-2.0-only
/**
 * @file ludlc_serdev_cfg.c
 *
 * @brief Optional configfs description of a LuDLC serdev port, for a system
 * where neither DT nor ACPI describes the LuDLC link.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * The port directory itself, and everything a link is made of, is in
 * ludlc_cfg_port.c; what is here is the UART:
 *
 * @verbatim
 *   /sys/kernel/config/ludlc-serdev/
 *       <port>/               one directory per serdev port
 *           baudrate
 *           parity            none | odd | even
 *           flow_control
 *           csum, framing, max_payload, live, ludlc/    - see ludlc_cfg_port.c
 * @endverbatim
 *
 * `<port>` must be named after the serdev device it describes (as shown by
 * dev_name(), e.g. "serial0-0"); the settings are applied when that device
 * probes.
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
#include "ludlc_serdev.h"

/* The bits this port keeps beyond the ones every port has. */
#define LUDLC_SERDEV_CFG_F_BAUDRATE_SET	(LUDLC_CFG_PORT_F_BUS + 0)
#define LUDLC_SERDEV_CFG_F_PARITY_SET	(LUDLC_CFG_PORT_F_BUS + 1)
#define LUDLC_SERDEV_CFG_F_FLOW_SET	(LUDLC_CFG_PORT_F_BUS + 2)

static struct configfs_subsystem ludlc_serdev_cfg_subsys;

static const char * const ludlc_serdev_parity_names[] = {
	[SERDEV_PARITY_NONE] = "none",
	[SERDEV_PARITY_EVEN] = "even",
	[SERDEV_PARITY_ODD]  = "odd",
};

/*
 * The line settings. Each reads back empty until written, and what is written
 * overrides firmware for this port only. They take effect the next time the
 * link is brought up, either from here or from the transport probe.
 */
static ssize_t ludlc_serdev_port_baudrate_show(struct config_item *item,
					       char *page)
{
	struct ludlc_cfg_port *port = to_ludlc_cfg_port(item);

	guard(mutex)(&port->lock);

	if (!test_bit(LUDLC_SERDEV_CFG_F_BAUDRATE_SET, &port->flags))
		return sysfs_emit(page, "\n");

	return sysfs_emit(page, "%u\n", port->args.serdev.baudrate);
}

static ssize_t ludlc_serdev_port_baudrate_store(struct config_item *item,
						const char *page, size_t count)
{
	struct ludlc_cfg_port *port = to_ludlc_cfg_port(item);
	u32 baudrate;
	int ret;

	ret = kstrtou32(page, 0, &baudrate);
	if (ret)
		return ret;

	if (!baudrate)
		return -EINVAL;

	guard(mutex)(&port->lock);
	port->args.serdev.baudrate = baudrate;
	__set_bit(LUDLC_SERDEV_CFG_F_BAUDRATE_SET, &port->flags);

	return count;
}
CONFIGFS_ATTR(ludlc_serdev_port_, baudrate);

static ssize_t ludlc_serdev_port_parity_show(struct config_item *item,
					     char *page)
{
	struct ludlc_cfg_port *port = to_ludlc_cfg_port(item);

	guard(mutex)(&port->lock);

	if (!test_bit(LUDLC_SERDEV_CFG_F_PARITY_SET, &port->flags))
		return sysfs_emit(page, "\n");

	return sysfs_emit(page, "%s\n",
			  ludlc_serdev_parity_names[port->args.serdev.parity]);
}

static ssize_t ludlc_serdev_port_parity_store(struct config_item *item,
					      const char *page, size_t count)
{
	struct ludlc_cfg_port *port = to_ludlc_cfg_port(item);
	int parity;

	parity = sysfs_match_string(ludlc_serdev_parity_names, page);
	if (parity < 0)
		return parity;

	guard(mutex)(&port->lock);
	port->args.serdev.parity = parity;
	__set_bit(LUDLC_SERDEV_CFG_F_PARITY_SET, &port->flags);

	return count;
}
CONFIGFS_ATTR(ludlc_serdev_port_, parity);

static ssize_t ludlc_serdev_port_flow_control_show(struct config_item *item,
						   char *page)
{
	struct ludlc_cfg_port *port = to_ludlc_cfg_port(item);

	guard(mutex)(&port->lock);

	if (!test_bit(LUDLC_SERDEV_CFG_F_FLOW_SET, &port->flags))
		return sysfs_emit(page, "\n");

	return sysfs_emit(page, "%d\n", port->args.serdev.flow_control);
}

static ssize_t ludlc_serdev_port_flow_control_store(struct config_item *item,
						    const char *page,
						    size_t count)
{
	struct ludlc_cfg_port *port = to_ludlc_cfg_port(item);
	bool flow_control;
	int ret;

	ret = kstrtobool(page, &flow_control);
	if (ret)
		return ret;

	guard(mutex)(&port->lock);
	port->args.serdev.flow_control = flow_control;
	__set_bit(LUDLC_SERDEV_CFG_F_FLOW_SET, &port->flags);

	return count;
}
CONFIGFS_ATTR(ludlc_serdev_port_, flow_control);

/** @brief Apply the line settings the user wrote to @p args. */
static void ludlc_serdev_cfg_override(const struct ludlc_cfg_port *port,
				      ludlc_platform_args_t *args)
{
	if (test_bit(LUDLC_SERDEV_CFG_F_BAUDRATE_SET, &port->flags))
		args->serdev.baudrate = port->args.serdev.baudrate;

	if (test_bit(LUDLC_SERDEV_CFG_F_PARITY_SET, &port->flags))
		args->serdev.parity = port->args.serdev.parity;

	if (test_bit(LUDLC_SERDEV_CFG_F_FLOW_SET, &port->flags))
		args->serdev.flow_control = port->args.serdev.flow_control;
}

static const struct ludlc_cfg_port_ops ludlc_serdev_cfg_port_ops = {
	.dev_find	= ludlc_serdev_dev_find,
	.default_args	= ludlc_serdev_default_args,
	.override	= ludlc_serdev_cfg_override,
	.link_up	= ludlc_serdev_link_up,
	.link_down	= ludlc_serdev_link_down,
	.get_conn	= ludlc_serdev_get_conn,
	.framing_ok	= ludlc_serdev_framing_ok,
};

static struct configfs_attribute *ludlc_serdev_port_attrs[] = {
	&ludlc_serdev_port_attr_baudrate,
	&ludlc_serdev_port_attr_parity,
	&ludlc_serdev_port_attr_flow_control,
	LUDLC_CFG_PORT_ATTRS,
	NULL,
};

static const struct config_item_type ludlc_serdev_port_type = {
	.ct_item_ops	= &ludlc_cfg_port_item_ops,
	.ct_attrs	= ludlc_serdev_port_attrs,
	.ct_owner	= THIS_MODULE,
};

/** @brief mkdir: a port describing the serdev device named @p name. */
static struct config_group *
ludlc_serdev_cfg_make_port(struct config_group *group, const char *name)
{
	return ludlc_cfg_port_make(group, name, &ludlc_serdev_cfg_port_ops,
				   &ludlc_serdev_port_type);
}

static struct configfs_group_operations ludlc_serdev_cfg_group_ops = {
	.make_group = ludlc_serdev_cfg_make_port,
};

static const struct config_item_type ludlc_serdev_cfg_subsys_type = {
	.ct_group_ops	= &ludlc_serdev_cfg_group_ops,
	.ct_owner	= THIS_MODULE,
};

static struct configfs_subsystem ludlc_serdev_cfg_subsys = {
	.su_group = {
		.cg_item = {
			.ci_namebuf = "ludlc-serdev",
			.ci_type    = &ludlc_serdev_cfg_subsys_type,
		},
	},
};

bool ludlc_serdev_cfg_apply(struct device *dev, ludlc_platform_args_t *args)
{
	return ludlc_cfg_port_apply(&ludlc_serdev_cfg_subsys, dev, args);
}

void ludlc_serdev_cfg_bind(struct device *dev, struct ludlc_connection *conn)
{
	ludlc_cfg_port_bind(&ludlc_serdev_cfg_subsys, dev, conn);
}

void ludlc_serdev_cfg_unbind(struct device *dev)
{
	ludlc_cfg_port_unbind(&ludlc_serdev_cfg_subsys, dev);
}

/** @brief Create /sys/kernel/config/ludlc-serdev/. */
int ludlc_serdev_cfg_init(void)
{
	return ludlc_cfg_port_subsys_register(&ludlc_serdev_cfg_subsys);
}

/** @brief Remove the subsystem, ports and all. */
void ludlc_serdev_cfg_deinit(void)
{
	ludlc_cfg_port_subsys_unregister(&ludlc_serdev_cfg_subsys);
}

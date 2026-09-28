// SPDX-License-Identifier: GPL-2.0-only
/**
 * @file ludlc_cfg_port.c
 *
 * @brief Optional configfs description of a LuDLC port, for a system where
 * neither DT nor ACPI describes the link on it.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * Every transport gets the same directory, because a link is made of the same
 * things whatever carries it:
 *
 * @verbatim
 *   /sys/kernel/config/<transport>/
 *       <port>/               one directory per port the transport drives
 *           csum              the checksum both ends run
 *           framing           how frames are delimited
 *           max_payload       the largest payload the link carries
 *           live              is the link on this port up?
 *           ludlc/            LuDLC bus devices parented to <port>
 *               <dev>/        compatible, label, reg, live
 * @endverbatim
 *
 * What differs is the wire: its settings, which the transport adds as
 * attributes of its own, and how a link on it is opened - see
 * @ref ludlc_cfg_port_ops.
 *
 * `<port>` must be named after the device it describes, as dev_name() shows it
 * ("serial0-0", "spi0.0"); the settings are applied when that device probes.
 * A written attribute overrides what firmware said about the port; one that
 * was never written reads back empty and leaves the firmware value, or the
 * driver default, unchanged, so creating a port only to reach its ludlc/
 * subdirectory does not disturb a device firmware describes.
 *
 * A port firmware describes as available is brought up by the transport when
 * it probes, and this directory only configures and inspects it. A port
 * firmware leaves unavailable stays down until "live" is written here.
 */

#include <linux/bitops.h>
#include <linux/cleanup.h>
#include <linux/configfs.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/stringify.h>
#include <linux/sysfs.h>

#include <ludlc.h>
#include <ludlc_framing.h>

#include "ludlc_bus.h"
#include "ludlc_csum.h"

/*
 * As CONFIGFS_ATTR(), but the attribute is left for a transport to splice into
 * its own port type - see LUDLC_CFG_PORT_ATTRS. The show and store code is
 * here, so this module is the one an open file pins.
 */
#define LUDLC_CFG_PORT_ATTR(_name)					\
	struct configfs_attribute ludlc_cfg_port_attr_##_name = {	\
		.ca_name	= __stringify(_name),			\
		.ca_mode	= 0644,					\
		.ca_owner	= THIS_MODULE,				\
		.show		= ludlc_cfg_port_##_name##_show,	\
		.store		= ludlc_cfg_port_##_name##_store,	\
	};								\
	EXPORT_SYMBOL_GPL(ludlc_cfg_port_attr_##_name)

#define LUDLC_CFG_PORT_ATTR_RO(_name)					\
	struct configfs_attribute ludlc_cfg_port_attr_##_name = {	\
		.ca_name	= __stringify(_name),			\
		.ca_mode	= 0444,					\
		.ca_owner	= THIS_MODULE,				\
		.show		= ludlc_cfg_port_##_name##_show,	\
	};								\
	EXPORT_SYMBOL_GPL(ludlc_cfg_port_attr_##_name)

/*
 * Empty until written. A port set to "none" holds no descriptor, as one never
 * written does, hence the flag.
 */
static ssize_t ludlc_cfg_port_csum_show(struct config_item *item, char *page)
{
	struct ludlc_cfg_port *port = to_ludlc_cfg_port(item);

	guard(mutex)(&port->lock);

	if (!test_bit(LUDLC_CFG_PORT_F_CSUM_SET, &port->flags))
		return sysfs_emit(page, "\n");

	return sysfs_emit(page, "%s\n", port->args.csum ?
			  port->args.csum->name : LUDLC_CSUM_NONE);
}

/**
 * @brief Choose the checksum a link opened from here will use.
 *
 * Configfs can only pass a name, so it is turned into a descriptor here and a
 * typo is reported by the write rather than by a link that never comes up. The
 * port then holds a reference, which keeps the module providing the checksum
 * from being unloaded while a port is set to use it. "none" opens the link
 * with no checksum at all.
 *
 * @return @p count, or -EINVAL if no checksum is registered under that name.
 */
static ssize_t ludlc_cfg_port_csum_store(struct config_item *item,
					 const char *page, size_t count)
{
	struct ludlc_cfg_port *port = to_ludlc_cfg_port(item);
	const struct ludlc_csum *csum;
	int ret;

	ret = ludlc_crc_by_name(page, &csum);
	if (ret)
		return ret;

	guard(mutex)(&port->lock);
	ludlc_csum_put(port->args.csum);
	port->args.csum = csum;
	__set_bit(LUDLC_CFG_PORT_F_CSUM_SET, &port->flags);

	return count;
}
LUDLC_CFG_PORT_ATTR(csum);

/* "none" and the names this kernel knows, which are what "csum" accepts. */
static ssize_t ludlc_cfg_port_csum_available_show(struct config_item *item,
						  char *page)
{
	return ludlc_csum_list(page);
}
LUDLC_CFG_PORT_ATTR_RO(csum_available);

/*
 * Empty until written, as the checksum is: what a port would otherwise run
 * comes from its module parameter, its firmware or its driver, and which of
 * those wins is only settled when the link opens.
 */
static ssize_t ludlc_cfg_port_framing_show(struct config_item *item, char *page)
{
	struct ludlc_cfg_port *port = to_ludlc_cfg_port(item);

	guard(mutex)(&port->lock);

	if (!test_bit(LUDLC_CFG_PORT_F_FRAMING_SET, &port->flags))
		return sysfs_emit(page, "\n");

	return sysfs_emit(page, "%s\n", port->args.framing->name);
}

/**
 * @brief Choose how a link opened from here delimits its frames.
 *
 * Both ends must agree on this as they must on the checksum, and getting it
 * wrong shows up the same way: a link on which nothing ever arrives. An
 * unknown name, or one this wire cannot carry, is rejected here rather than
 * left to be discovered that way.
 *
 * @return @p count, or -EINVAL for a framing this port cannot run.
 */
static ssize_t ludlc_cfg_port_framing_store(struct config_item *item,
					    const char *page, size_t count)
{
	struct ludlc_cfg_port *port = to_ludlc_cfg_port(item);
	const struct ludlc_framing *framing;

	framing = ludlc_framing_lookup(page);
	if (!framing)
		return -EINVAL;

	if (port->ops->framing_ok && !port->ops->framing_ok(framing))
		return -EINVAL;

	scoped_guard(mutex, &port->lock) {
		port->args.framing = framing;
		__set_bit(LUDLC_CFG_PORT_F_FRAMING_SET, &port->flags);
	}

	return count;
}
LUDLC_CFG_PORT_ATTR(framing);

/* What this wire can carry, which is what "framing" accepts. */
static ssize_t ludlc_cfg_port_framing_available_show(struct config_item *item,
						     char *page)
{
	struct ludlc_cfg_port *port = to_ludlc_cfg_port(item);
	const struct ludlc_framing *framing;
	unsigned int i;
	int len = 0;

	for (i = 0; (framing = ludlc_framing_at(i)); i++) {
		if (port->ops->framing_ok && !port->ops->framing_ok(framing))
			continue;

		len += sysfs_emit_at(page, len, len ? " %s" : "%s",
				     framing->name);
	}

	return len + sysfs_emit_at(page, len, "\n");
}
LUDLC_CFG_PORT_ATTR_RO(framing_available);

static ssize_t ludlc_cfg_port_max_payload_show(struct config_item *item,
					       char *page)
{
	struct ludlc_cfg_port *port = to_ludlc_cfg_port(item);

	guard(mutex)(&port->lock);

	return sysfs_emit(page, "%u\n",
			  (unsigned int)port->args.cfg.max_payload);
}

/**
 * @brief Choose the largest payload a link opened from here carries.
 *
 * Both ends must agree on this as they must on the checksum. Zero restores
 * whatever the port would otherwise use. The transport works out the frame
 * around it, adding the header and the checksum.
 *
 * @return @p count, or -EINVAL for a size this build cannot hold.
 */
static ssize_t ludlc_cfg_port_max_payload_store(struct config_item *item,
						const char *page, size_t count)
{
	struct ludlc_cfg_port *port = to_ludlc_cfg_port(item);
	u32 size;
	int ret;

	ret = kstrtou32(page, 0, &size);
	if (ret)
		return ret;

	if (size > CONFIG_LUDLC_MAX_PAYLOAD_SIZE)
		return -EINVAL;

	scoped_guard(mutex, &port->lock) {
		port->args.cfg.max_payload = (ludlc_payload_size_t)size;
		if (size)
			__set_bit(LUDLC_CFG_PORT_F_PAYLOAD_SET, &port->flags);
		else
			__clear_bit(LUDLC_CFG_PORT_F_PAYLOAD_SET,
				    &port->flags);
	}

	return count;
}
LUDLC_CFG_PORT_ATTR(max_payload);

/* The most this build can hold, as "max_payload" accepts. */
static ssize_t
ludlc_cfg_port_max_payload_available_show(struct config_item *item, char *page)
{
	return sysfs_emit(page, "1-%u\n",
			  (unsigned int)CONFIG_LUDLC_MAX_PAYLOAD_SIZE);
}
LUDLC_CFG_PORT_ATTR_RO(max_payload_available);

/**
 * @brief Apply the attributes the user wrote here to @p args.
 *
 * The settings of this wire, which the transport keeps, then the ones every
 * link has. A reference to the checksum is taken - see ludlc_args_csum_put().
 */
static void ludlc_cfg_port_override(struct ludlc_cfg_port *port,
				    ludlc_platform_args_t *args)
{
	const struct ludlc_csum *csum;

	lockdep_assert_held(&port->lock);

	if (port->ops->override)
		port->ops->override(port, args);

	if (test_bit(LUDLC_CFG_PORT_F_FRAMING_SET, &port->flags))
		args->framing = port->args.framing;

	if (test_bit(LUDLC_CFG_PORT_F_PAYLOAD_SET, &port->flags))
		args->cfg.max_payload = port->args.cfg.max_payload;

	/*
	 * The args borrow the descriptor from the port, but only the port's
	 * reference keeps the provider loaded, so take one for the caller to
	 * drop with ludlc_args_csum_put(). "none" holds no descriptor and
	 * needs no reference.
	 */
	if (test_bit(LUDLC_CFG_PORT_F_CSUM_SET, &port->flags)) {
		csum = ludlc_csum_get(port->args.csum);
		if (csum || !port->args.csum) {
			ludlc_csum_put(args->csum);
			args->csum = csum;
		}
	}
}

/** @brief Is a link up on this port? */
static ssize_t ludlc_cfg_port_live_show(struct config_item *item, char *page)
{
	struct ludlc_cfg_port *port = to_ludlc_cfg_port(item);

	guard(mutex)(&port->lock);

	return sysfs_emit(page, "%d\n", !!port->devices.conn);
}

/**
 * @brief Bring the link on this port up, or take it down.
 *
 * This is the only way to open a port firmware leaves unavailable, and a way
 * to reopen one it does describe with different settings. The link is built
 * from the firmware defaults with this directory applied over them, as a
 * transport probing the device builds it.
 *
 * @return @p count, -ENODEV while no transport is bound to the port, or a
 * negative errno from opening the link.
 */
static ssize_t ludlc_cfg_port_live_store(struct config_item *item,
					 const char *page, size_t count)
{
	struct ludlc_cfg_port *port = to_ludlc_cfg_port(item);
	struct ludlc_connection *conn;
	ludlc_platform_args_t args;
	bool live;
	int ret;

	ret = kstrtobool(page, &live);
	if (ret)
		return ret;

	/* Attach and detach walk the device nodes below this port. */
	guard(mutex)(&port->subsys->su_mutex);
	guard(mutex)(&port->lock);

	/* There is nothing to open until a transport binds to the port. */
	if (!port->dev)
		return -ENODEV;

	/* Already in the requested state. */
	if (live == !!port->devices.conn)
		return count;

	if (!live) {
		ludlc_cfg_devices_group_detach(&port->devices);
		port->ops->link_down(port->dev);
		__clear_bit(LUDLC_CFG_PORT_F_LIVE, &port->flags);

		return count;
	}

	port->ops->default_args(port->dev, &args);
	ludlc_cfg_port_override(port, &args);

	ret = port->ops->link_up(port->dev, &args, &conn);
	ludlc_args_csum_put(&args);
	if (ret)
		return ret;

	ludlc_cfg_devices_group_attach(&port->devices, conn);
	__set_bit(LUDLC_CFG_PORT_F_LIVE, &port->flags);

	return count;
}
LUDLC_CFG_PORT_ATTR(live);

/** @brief rmdir: the port goes away, and with it any link it opened. */
static void ludlc_cfg_port_release(struct config_item *item)
{
	struct ludlc_cfg_port *port = to_ludlc_cfg_port(item);

	if (port->dev) {
		/* A link this directory opened cannot outlive it. */
		if (test_bit(LUDLC_CFG_PORT_F_LIVE, &port->flags))
			port->ops->link_down(port->dev);

		put_device(port->dev);
	}

	ludlc_csum_put(port->args.csum);

	kfree(port);
}

struct configfs_item_operations ludlc_cfg_port_item_ops = {
	.release = ludlc_cfg_port_release,
};
EXPORT_SYMBOL_GPL(ludlc_cfg_port_item_ops);

/**
 * @brief mkdir: a port of type @p type, describing the device named @p name.
 *
 * For the make_group() of a transport's subsystem, which is where the type
 * comes from: it carries the attributes of this wire and belongs to the module
 * implementing them. Nothing is written yet, so the port describes the device
 * without changing anything about it.
 *
 * @param group The subsystem's own group, as configfs passes it.
 */
struct config_group *ludlc_cfg_port_make(struct config_group *group,
					 const char *name,
					 const struct ludlc_cfg_port_ops *ops,
					 const struct config_item_type *type)
{
	struct ludlc_cfg_port *port;
	struct ludlc_connection *conn;

	port = kzalloc(sizeof(*port), GFP_KERNEL);
	if (!port)
		return ERR_PTR(-ENOMEM);

	mutex_init(&port->lock);
	port->ops = ops;
	port->subsys = to_configfs_subsystem(group);
	port->args.cfg = (struct ludlc_conn_cfg)LUDLC_CONN_CFG_DEFAULT;

	config_group_init_type_name(&port->group, name, type);

	ludlc_cfg_devices_group_init(&port->devices, "ludlc");
	configfs_add_default_group(&port->devices.group, &port->group);

	/*
	 * The transport may have probed long before this directory was
	 * created: take the device it drives, and the link as well if it is
	 * already up. Otherwise both arrive later through
	 * ludlc_cfg_port_bind().
	 */
	port->dev = ops->dev_find(name);
	if (port->dev) {
		conn = ops->get_conn(port->dev);
		if (conn)
			ludlc_cfg_devices_group_attach(&port->devices, conn);
	}

	return &port->group;
}
EXPORT_SYMBOL_GPL(ludlc_cfg_port_make);

/**
 * @brief The port directory named after @p dev, under the subsystem mutex.
 *
 * @return The port with a reference held, or NULL if it was never created.
 * Release it with ludlc_cfg_port_put().
 */
static struct ludlc_cfg_port *
ludlc_cfg_port_find(struct configfs_subsystem *subsys, struct device *dev)
{
	struct config_item *item;

	lockdep_assert_held(&subsys->su_mutex);

	item = config_group_find_item(&subsys->su_group, dev_name(dev));

	return item ? to_ludlc_cfg_port(item) : NULL;
}

/** @brief ludlc_cfg_port_find(), taking the subsystem mutex here. */
static struct ludlc_cfg_port *
ludlc_cfg_port_get(struct configfs_subsystem *subsys, struct device *dev)
{
	guard(mutex)(&subsys->su_mutex);

	return ludlc_cfg_port_find(subsys, dev);
}

static inline void ludlc_cfg_port_put(struct ludlc_cfg_port *port)
{
	config_item_put(&port->group.cg_item);
}

/**
 * @brief Apply the port directory's overrides to @p args.
 *
 * For a transport probing a device firmware describes: whatever it has put in
 * @p args is what a port directory overrides, attribute by attribute.
 *
 * @return Whether a port describing @p dev exists at all.
 */
bool ludlc_cfg_port_apply(struct configfs_subsystem *subsys,
			  struct device *dev, ludlc_platform_args_t *args)
{
	struct ludlc_cfg_port *port;

	port = ludlc_cfg_port_get(subsys, dev);
	if (!port)
		return false;

	scoped_guard(mutex, &port->lock)
		ludlc_cfg_port_override(port, args);

	ludlc_cfg_port_put(port);

	return true;
}
EXPORT_SYMBOL_GPL(ludlc_cfg_port_apply);

/**
 * @brief Give the port directory the device it describes, and its link.
 *
 * @param conn NULL for a port the transport left down: the directory keeps the
 * device, so its "live" attribute can open the link later.
 */
void ludlc_cfg_port_bind(struct configfs_subsystem *subsys, struct device *dev,
			 struct ludlc_connection *conn)
{
	struct ludlc_cfg_port *port;

	/* Held across the attach, which walks the port's device nodes. */
	guard(mutex)(&subsys->su_mutex);

	port = ludlc_cfg_port_find(subsys, dev);
	if (!port)
		return;

	scoped_guard(mutex, &port->lock) {
		if (!port->dev)
			port->dev = get_device(dev);

		if (conn)
			ludlc_cfg_devices_group_attach(&port->devices, conn);
	}

	ludlc_cfg_port_put(port);
}
EXPORT_SYMBOL_GPL(ludlc_cfg_port_bind);

/**
 * @brief Take the device away from the port directory.
 *
 * The devices it created are removed first, while their link is still alive.
 */
void ludlc_cfg_port_unbind(struct configfs_subsystem *subsys,
			   struct device *dev)
{
	struct ludlc_cfg_port *port;

	/* Held across the detach, which walks the port's device nodes. */
	guard(mutex)(&subsys->su_mutex);

	port = ludlc_cfg_port_find(subsys, dev);
	if (!port)
		return;

	/*
	 * Clearing @dev under the same lock a "live" store takes stops configfs
	 * from opening a port the transport is leaving.
	 */
	scoped_guard(mutex, &port->lock) {
		if (port->devices.conn)
			ludlc_cfg_devices_group_detach(&port->devices);

		__clear_bit(LUDLC_CFG_PORT_F_LIVE, &port->flags);

		if (port->dev) {
			put_device(port->dev);
			port->dev = NULL;
		}
	}

	ludlc_cfg_port_put(port);
}
EXPORT_SYMBOL_GPL(ludlc_cfg_port_unbind);

/**
 * @brief Create /sys/kernel/config/<transport>/.
 *
 * A transport does this before registering its driver: a port can probe as
 * soon as it does, and it has to find this ready.
 *
 * @return 0, or a negative errno from configfs.
 */
int ludlc_cfg_port_subsys_register(struct configfs_subsystem *subsys)
{
	config_group_init(&subsys->su_group);
	mutex_init(&subsys->su_mutex);

	return configfs_register_subsystem(subsys);
}
EXPORT_SYMBOL_GPL(ludlc_cfg_port_subsys_register);

/** @brief Remove the subsystem, ports and all. */
void ludlc_cfg_port_subsys_unregister(struct configfs_subsystem *subsys)
{
	configfs_unregister_subsystem(subsys);
}
EXPORT_SYMBOL_GPL(ludlc_cfg_port_subsys_unregister);

/* SPDX-License-Identifier: GPL-2.0-only */
/**
 * @file ludlc_bus.h
 *
 * @brief LuDLC software bus API.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * Each connection is a bus of its own, carrying that link's channels as
 * devices, so a protocol client binds to one as an ordinary Linux driver. A
 * channel belongs to exactly one device; a device owns one channel, or several
 * when a single driver must handle a group of them.
 */

#ifndef __LUDLC_BUS_H__
#define __LUDLC_BUS_H__

#include <linux/device.h>
#include <linux/mutex.h>
#include <ludlc.h>
#include <ludlc_proto.h>

struct ludlc_connection;
struct fwnode_handle;
extern const struct bus_type ludlc_bus_type;

/**
 * @brief Highest channel id @ref ludlc_channel_t can represent.
 *
 * A link with a tag in its channel field has fewer: only the bits below the
 * tag select a channel. See ludlc_chan_id().
 */
#define LUDLC_CHANNEL_MAX	((u32)(ludlc_channel_t)~0U)

/**
 * @struct ludlc_device
 * @brief One or more channels of one link, as a device on the LuDLC bus.
 */
struct ludlc_device {
	/** The link this device sits on; each connection is its own bus. */
	struct ludlc_connection *conn;

	/**
	 * @brief The bound driver, and the lock that keeps it there.
	 *
	 * @c drv is set once probe() has succeeded and cleared before remove()
	 * runs, both under @c cb_lock. Traffic for this device's channels is
	 * dispatched under the same lock, so no callback can enter a driver
	 * that is being unbound.
	 */
	struct mutex cb_lock;
	struct ludlc_driver *drv;

	/**
	 * @brief The session of the link the driver was probed in.
	 *
	 * Set with @c drv. A device whose driver was probed in an earlier
	 * session is probed again when the peer comes back.
	 */
	int session;

	/**
	 * @brief What this device is, and whether it is listening.
	 *
	 * @c ENUMERATED marks a device the bus created from firmware, which
	 * comes and goes with the link; one created by any other code stays
	 * until its creator removes it. @c SUSPENDED blocks delivery from the
	 * end of the driver's suspend() to the start of its resume().
	 * @c REMOVED marks a device on its way off the bus, which only one
	 * caller takes off and no driver binds any more. Written
	 * non-atomically: the first bit before the device is visible, the
	 * others only under @c cb_lock, so the writers never overlap.
	 */
#define LUDLC_DEV_ENUMERATED_F	0
#define LUDLC_DEV_SUSPENDED_F	1
#define LUDLC_DEV_REMOVED_F	2
	unsigned long flags;

	/**
	 * @brief How many channels this device owns.
	 *
	 * A device cannot own more channels than the link has, and the control
	 * channel is not one of them, so the count fits in a channel id.
	 */
	ludlc_channel_t nchans;

	struct device dev;

	/**
	 * @brief The channels this device owns, unique within @c conn.
	 *
	 * Set by ludlc_device_alloc() and never changed; the device is named
	 * after the first. A device owning several channels is told which one
	 * a packet belongs to by the @p chan argument of receive() and
	 * confirm().
	 */
	ludlc_channel_t chans[] __counted_by(nchans);
};

/**
 * @struct ludlc_driver
 * @brief A driver for one kind of device on the LuDLC bus.
 *
 * Matching is on compatible strings from a DT node, an ACPI node or a configfs
 * software node, so the match table is the ordinary @c drv.of_match_table,
 * plus @c drv.acpi_match_table for an ACPI-only driver; use the former with
 * MODULE_DEVICE_TABLE(of, ...), whose aliases this bus reports in the uevent
 * and in each device's sysfs "modalias". Power management is the ordinary
 * @c drv.pm: the link is up for all of prepare() and outlasts suspend(), so a
 * driver may still talk to the peer from either, but between the start of
 * suspend() and the end of resume() receive() is called only for a wakeup
 * source, while confirm() is never held back. A driver that must not be
 * suspended must return a negative error from its suspend().
 */
struct ludlc_driver {
	/**
	 * @brief Bind to @p ldev.
	 *
	 * Nothing is dispatched to the driver until probe() returns. A driver
	 * that must talk to the peer before it can finish calls
	 * ludlc_device_listen() once it is ready to handle a callback.
	 *
	 * A device is probed for each session with the peer: when the peer
	 * goes away and comes back, the driver is removed and probed again,
	 * the peer being possibly one that restarted.
	 */
	int (*probe)(struct ludlc_device *ldev);

	/**
	 * @brief Unbind from @p ldev.
	 *
	 * Nothing is dispatched to the driver once this is called. A driver
	 * that still has something to say to the peer calls
	 * ludlc_device_listen() first, and ludlc_device_unlisten() before it
	 * frees what its callbacks use.
	 */
	void (*remove)(struct ludlc_device *ldev);

	/** Quiesce the channel on reboot or poweroff. Optional. */
	void (*shutdown)(struct ludlc_device *ldev);

	/**
	 * @brief A packet arrived on @p chan, or a packet sent on it was
	 * acknowledged: @c error is 0 if the peer acknowledged it and negative
	 * if it never did.
	 *
	 * @p chan is the channel field of the packet, so on a link with a tag
	 * it carries the tag as well: ludlc_chan_id() gives the channel and
	 * ludlc_chan_tag() the tag. A reply sent on @p chan keeps the tag.
	 *
	 * Both run in the transport's receive context, between a successful
	 * probe() and remove(), unless the driver asked for them earlier or
	 * later with ludlc_device_listen(), and never two at once for the same
	 * device, on any of its channels. They may sleep, but the link's traffic waits
	 * while they do, and neither may remove the device from the bus:
	 * remove() waits for them under the lock they are called with.
	 */
	void (*receive)(struct ludlc_device *ldev, ludlc_channel_t chan,
			const void *data, ludlc_payload_size_t size,
			ludlc_timestamp_t tstamp);
	void (*confirm)(struct ludlc_device *ldev, ludlc_channel_t chan,
			int error, const void *data, ludlc_payload_size_t size);

	struct device_driver	drv;
};

#define to_ludlc_device(_dev) container_of(_dev, struct ludlc_device, dev)
#define to_ludlc_driver(_drv) container_of(_drv, struct ludlc_driver, drv)

/**
 * @brief Allocate a device for the @p nchans channels in @p chans, on @p
 * conn's bus and parented to the link's own device.
 *
 * The channels cannot change afterwards. The device comes back initialized but
 * not yet visible, so the caller can set its firmware node before calling
 * ludlc_device_add().
 *
 * @return The device, whose reference belongs to the caller: drop it with
 * ludlc_device_put() if it never reaches the bus. ERR_PTR(-EINVAL) for a
 * channel that is reserved, listed twice, or does not fit below the tag of
 * the link, or ERR_PTR(-ENOMEM).
 */
struct ludlc_device *ludlc_device_alloc(struct ludlc_connection *conn,
					const ludlc_channel_t *chans,
					unsigned int nchans);

/**
 * @brief Make @p ldev visible on its bus, claiming the channels it was
 * allocated with.
 *
 * @return 0, -EBUSY if a sibling device already owns one of them, or another
 * negative errno. On failure the device is left allocated and initialized, so
 * cleanup is always ludlc_device_put().
 */
int ludlc_device_add(struct ludlc_device *ldev);

/**
 * @brief Take @p ldev off its bus, releasing every channel it owns.
 *
 * Consumes the reference ludlc_device_alloc() returned, and does nothing for a
 * device that is already off the bus.
 */
void ludlc_device_unregister(struct ludlc_device *ldev);

static inline void ludlc_device_put(struct ludlc_device *ldev)
{
	if (ldev)
		put_device(&ldev->dev);
}

/**
 * @brief Start dispatching @p ldev's channels to the driver from within its
 * own probe(), rather than when probe() returns, or again from within its
 * remove().
 *
 * For a driver whose probe() needs a reply from the peer. Its receive() and
 * confirm() may be called as soon as this returns, so it must be called only
 * once everything they use is initialised. The bus undoes it if probe() then
 * fails.
 *
 * A remove() that still has to talk to the peer calls it too, the bus having
 * stopped the dispatch before calling remove(), and then calls
 * ludlc_device_unlisten() before it frees what the callbacks use. The bus
 * stops the dispatch again when remove() returns.
 */
void ludlc_device_listen(struct ludlc_device *ldev);

/**
 * @brief Stop dispatching @p ldev's channels to the driver.
 *
 * Once this returns, receive() and confirm() are neither running nor about to
 * run, so whatever they use can be torn down. The bus does the same before
 * calling remove(). This is for a probe() that called ludlc_device_listen()
 * and then has to fail, as the bus only undoes it after probe() has returned.
 */
void ludlc_device_unlisten(struct ludlc_device *ldev);

/**
 * @brief Stop the bus from following its link, and start it again.
 *
 * For a system transition, during which the driver core walks the devices and
 * none may be added or removed underneath it. The platform glue pairs these
 * with LUDLC_CONN_BUS_HELD_F.
 */
void ludlc_bus_conn_hold(struct ludlc_connection *conn);
void ludlc_bus_conn_resync(struct ludlc_connection *conn);

/**
 * @brief Connection events, handled by the bus for every Linux transport.
 *
 * The link state creates and removes the devices firmware describes below the
 * transport, and payloads go to the driver bound to the channel's device. What
 * the peer announces in the handshake is kept for ludlc_device_peer_caps(). A
 * transport passes this to ludlc_connection_init() and needs no callbacks of
 * its own; drivers see the traffic through @ref ludlc_driver instead.
 */
extern const struct ludlc_conn_cb ludlc_bus_conn_cb;

/**
 * @brief Copy out what the peer on @p ldev's link announced about itself.
 *
 * The peer announces its capabilities in the handshake, before the link comes
 * up, so a device the link enumerates finds them in place from its probe().
 * They are those of the latest handshake: a device that outlives a session
 * keeps the previous peer's until the next one has announced. Their layout is
 * what the two ends agree on.
 *
 * @param ldev A device on the link.
 * @param buf Where to copy them, or NULL with @p len zero to learn the size.
 * @param len Room at @p buf; no more than this is copied.
 * @return The size of the capabilities, more than @p len when they did not
 * fit, zero if the peer announced none, -ENODATA if it has not announced yet,
 * or -EINVAL.
 */
ssize_t ludlc_device_peer_caps(struct ludlc_device *ldev, void *buf,
			       size_t len);

/**
 * @brief Look up the device @p chan belongs to on @p conn.
 *
 * @p chan may be the channel field of a packet: a tag in it plays no part in
 * the lookup.
 *
 * @return The device with a reference held, which the caller drops with
 * ludlc_device_put(), or NULL if the channel is unclaimed.
 */
struct ludlc_device *ludlc_device_find_by_channel(struct ludlc_connection *conn,
						  ludlc_channel_t chan);

/**
 * @brief Apply what firmware says about the tag in the channel field of the
 * link on @p dev.
 *
 * ludlc,channel-tag-bits = <n>, on the node of the link. For a transport's
 * default_args(); a width no link can run is warned about and ignored.
 */
void ludlc_fw_chan_tag_bits(struct device *dev, struct ludlc_conn_cfg *cfg);

/**
 * @brief Register @p ldrv, owned by its module so that it cannot be unloaded
 * while bound.
 *
 * @return 0, or a negative errno. A driver must provide probe(); the rest is
 * optional.
 */
int __ludlc_driver_register(struct ludlc_driver *ldrv, struct module *owner);
#define ludlc_driver_register(ldrv) __ludlc_driver_register(ldrv, THIS_MODULE)

void ludlc_driver_unregister(struct ludlc_driver *ldrv);

/** @brief For the common case of a module containing only this driver. */
#define module_ludlc_driver(__ludlc_driver) \
	module_driver(__ludlc_driver, ludlc_driver_register, \
		      ludlc_driver_unregister)

#if IS_REACHABLE(CONFIG_CONFIGFS_FS)
#include <linux/configfs.h>

/**
 * @struct ludlc_cfg_devices
 * @brief Reusable configfs group holding LuDLC bus device nodes.
 *
 * A transport embeds one of these - typically a "ludlc" subdirectory of its
 * own port group - so that every device created in it appears on the
 * transport's own bus. @c conn is the link the group passes to
 * ludlc_device_alloc(): set once the transport binds and cleared when it goes
 * away, so nodes created in the meantime cannot be brought up.
 */
struct ludlc_cfg_devices {
	struct config_group group;
	struct ludlc_connection *conn;
};

void ludlc_cfg_devices_group_init(struct ludlc_cfg_devices *devs,
				  const char *name);

void ludlc_cfg_devices_group_attach(struct ludlc_cfg_devices *devs,
				    struct ludlc_connection *conn);

void ludlc_cfg_devices_group_detach(struct ludlc_cfg_devices *devs);

struct ludlc_cfg_port;

/**
 * @struct ludlc_cfg_port_ops
 * @brief What a port directory needs of the transport whose port it describes.
 *
 * The checksum, the framing and the frame size are the same on every wire, and
 * so is what a port directory does with them. These callbacks are the parts
 * that are not: which devices the directory may name, what their firmware
 * says, and how a link on them is opened and closed.
 */
struct ludlc_cfg_port_ops {
	/**
	 * @brief The device called @p name among those this transport drives.
	 *
	 * @return The device with a reference held, which this layer drops
	 * with put_device(), or NULL if the transport drives no such device.
	 */
	struct device *(*dev_find)(const char *name);

	/** @brief The transport's defaults for @p dev, firmware included. */
	void (*default_args)(struct device *dev, ludlc_platform_args_t *args);

	/**
	 * @brief Apply the bus attributes the user wrote to @p args.
	 *
	 * Optional: for a bus whose ports have settings of their own, beyond
	 * the ones every link has. Called with @p port->lock held.
	 */
	void (*override)(const struct ludlc_cfg_port *port,
			 ludlc_platform_args_t *args);

	/** @brief Open a link on @p dev, or -EBUSY if one is already up. */
	int (*link_up)(struct device *dev, const ludlc_platform_args_t *args,
		       struct ludlc_connection **conn);

	/** @brief Take down the link on @p dev, if it has one. */
	void (*link_down)(struct device *dev);

	/** @brief The link running on @p dev, or NULL while it is down. */
	struct ludlc_connection *(*get_conn)(struct device *dev);

	/**
	 * @brief Can a link on this bus carry @p framing?
	 *
	 * The "framing" attribute refuses what a transport would refuse at
	 * open, and leaves it out of the list it offers.
	 */
	bool (*framing_ok)(const struct ludlc_framing *framing);
};

/**
 * @struct ludlc_cfg_port
 * @brief One configfs directory, describing the link on one transport port.
 *
 * A transport registers a subsystem of these and gets the tree every LuDLC
 * port has: the checksum, the framing, the frame size and the channel tag a
 * link opened from here will run, a "live" switch that opens it, and the
 * "ludlc" subgroup holding the bus devices it carries. What the transport
 * adds are the settings of its own wire - see @ref ludlc_cfg_port_ops.
 */
struct ludlc_cfg_port {
	struct config_group group;

	/** Default subgroup "ludlc": the LuDLC bus nodes below this port. */
	struct ludlc_cfg_devices devices;

	const struct ludlc_cfg_port_ops *ops;

	/** The subsystem this directory was created in. */
	struct configfs_subsystem *subsys;

	/**
	 * @brief The device this directory is named after, with a reference
	 * held, or NULL while no transport is bound to it.
	 *
	 * Only a bound port can be brought up from here.
	 */
	struct device *dev;

	/** Serializes @c flags, @c args, @c dev and the devices group. */
	struct mutex lock;

	/**
	 * @brief Which of @c args the user actually wrote, and so which ones
	 * override firmware, plus whether the link was opened from here.
	 *
	 * Written under @c lock, and so non-atomically; a show() may read a
	 * bit without the lock and get a value that has since changed, which
	 * is all a single read of this directory can offer anyway.
	 */
	unsigned long flags;

	ludlc_platform_args_t args;
};

/*
 * The bits this layer keeps in ludlc_cfg_port.flags. A transport numbers the
 * ones for its own attributes from LUDLC_CFG_PORT_F_BUS up.
 */
#define LUDLC_CFG_PORT_F_LIVE		0
#define LUDLC_CFG_PORT_F_FRAMING_SET	1
#define LUDLC_CFG_PORT_F_PAYLOAD_SET	2
#define LUDLC_CFG_PORT_F_CSUM_SET	3
#define LUDLC_CFG_PORT_F_TAG_BITS_SET	4
#define LUDLC_CFG_PORT_F_BUS		8

static inline
struct ludlc_cfg_port *to_ludlc_cfg_port(struct config_item *item)
{
	return container_of(to_config_group(item), struct ludlc_cfg_port,
			    group);
}

/*
 * The attributes every port has, whatever carries it. A transport splices
 * these into the array of its own port type, which is what ties them to the
 * module the type belongs to.
 */
extern struct configfs_attribute ludlc_cfg_port_attr_csum;
extern struct configfs_attribute ludlc_cfg_port_attr_csum_available;
extern struct configfs_attribute ludlc_cfg_port_attr_framing;
extern struct configfs_attribute ludlc_cfg_port_attr_framing_available;
extern struct configfs_attribute ludlc_cfg_port_attr_max_payload;
extern struct configfs_attribute ludlc_cfg_port_attr_max_payload_available;
extern struct configfs_attribute ludlc_cfg_port_attr_channel_tag_bits;
extern struct configfs_attribute ludlc_cfg_port_attr_live;

#define LUDLC_CFG_PORT_ATTRS				\
	&ludlc_cfg_port_attr_csum,			\
	&ludlc_cfg_port_attr_csum_available,		\
	&ludlc_cfg_port_attr_framing,			\
	&ludlc_cfg_port_attr_framing_available,		\
	&ludlc_cfg_port_attr_max_payload,		\
	&ludlc_cfg_port_attr_max_payload_available,	\
	&ludlc_cfg_port_attr_channel_tag_bits,		\
	&ludlc_cfg_port_attr_live

/** @brief Frees a port and its devices; for a port type's ct_item_ops. */
extern struct configfs_item_operations ludlc_cfg_port_item_ops;

struct config_group *ludlc_cfg_port_make(struct config_group *group,
					 const char *name,
					 const struct ludlc_cfg_port_ops *ops,
					 const struct config_item_type *type);

bool ludlc_cfg_port_apply(struct configfs_subsystem *subsys,
			  struct device *dev, ludlc_platform_args_t *args);

void ludlc_cfg_port_bind(struct configfs_subsystem *subsys, struct device *dev,
			 struct ludlc_connection *conn);

void ludlc_cfg_port_unbind(struct configfs_subsystem *subsys,
			   struct device *dev);

int ludlc_cfg_port_subsys_register(struct configfs_subsystem *subsys);

void ludlc_cfg_port_subsys_unregister(struct configfs_subsystem *subsys);
#endif

#endif /* __LUDLC_BUS_H__ */

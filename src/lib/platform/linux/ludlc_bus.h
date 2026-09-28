// SPDX-License-Identifier: GPL-2.0-only
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

/** @brief Highest channel id @ref ludlc_channel_t can represent. */
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
	 * @brief What this device is, and whether it is listening.
	 *
	 * @c ENUMERATED marks a device the bus created from firmware, which
	 * comes and goes with the link; one created by any other code stays
	 * until its creator removes it. @c SUSPENDED blocks delivery from the
	 * end of the driver's suspend() to the start of its resume(). Written
	 * non-atomically: the first bit before the device is visible, the
	 * second only under @c cb_lock, so the two writers never overlap.
	 */
#define LUDLC_DEV_ENUMERATED_F	0
#define LUDLC_DEV_SUSPENDED_F	1
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
	 */
	int (*probe)(struct ludlc_device *ldev);
	void (*remove)(struct ludlc_device *ldev);

	/** Quiesce the channel on reboot or poweroff. Optional. */
	void (*shutdown)(struct ludlc_device *ldev);

	/**
	 * @brief A packet arrived on @p chan, or a packet sent on it was
	 * acknowledged: @c error is 0 if the peer acknowledged it and negative
	 * if it never did.
	 *
	 * Both run in the transport's receive context, between a successful
	 * probe() and remove(), and never two at once for the same device, on
	 * any of its channels. They may sleep, but the link's traffic waits
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
 * channel that is reserved or listed twice, or ERR_PTR(-ENOMEM).
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
 * own probe(), rather than when probe() returns.
 *
 * For a driver whose probe() needs a reply from the peer. Its receive() and
 * confirm() may be called as soon as this returns, so it must be called only
 * once everything they use is initialised. The bus undoes it if probe() then
 * fails.
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
 * transport, and payloads go to the driver bound to the channel's device. A
 * transport passes this to ludlc_connection_init() and needs no callbacks of
 * its own; drivers see the traffic through @ref ludlc_driver instead.
 */
extern const struct ludlc_conn_cb ludlc_bus_conn_cb;

/**
 * @brief Look up the device @p chan belongs to on @p conn.
 *
 * @return The device with a reference held, which the caller drops with
 * ludlc_device_put(), or NULL if the channel is unclaimed.
 */
struct ludlc_device *ludlc_device_find_by_channel(struct ludlc_connection *conn,
						  ludlc_channel_t chan);

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

#endif /* __LUDLC_BUS_H__ */

// SPDX-License-Identifier: GPL-2.0-only
/**
 * @file ludlc_bus.c
 *
 * @brief The LuDLC software bus, and the module entry point of the core.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * Each connection is a bus of its own, and carries the channels of its link
 * as devices, which protocol clients bind to as ordinary Linux drivers. The
 * bus follows the link: it creates the devices firmware describes when the
 * peer answers and removes them when the peer goes away, and it probes the
 * drivers of the other devices again for each session. This file also holds
 * what the core needs from the platform on Linux - its timers and the
 * per-connection state - and registers the bus when the module loads.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/cleanup.h>
#include <linux/acpi.h>
#include <linux/of_device.h>
#include <linux/mod_devicetable.h>
#include <linux/property.h>
#include <linux/bitops.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/xarray.h>
#include <linux/version.h>

#include <ludlc.h>
#include <ludlc_private.h>

#include "ludlc_bus.h"
#include "ludlc_csum.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 2, 0)
#define timer_delete_sync(t)	del_timer_sync(t)
#define timer_shutdown_sync(t)	del_timer_sync(t)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 16, 0)
#define timer_container_of(p, t, f)	from_timer(p, t, f)
#endif

/*
 * Alias for a device with no firmware node to derive one from. The "of:" form
 * is written out by hand so that such a device is still reachable through the
 * module aliases modpost generates from an of_device_id table, this bus having
 * no other match table.
 */
#define LUDLC_OF_MODALIAS_FMT	"of:N*T*C%s"

/** @brief Run a core timer's callback, re-arming a periodic one first. */
static void ludlc_timer_fn(struct timer_list *t)
{
	ludlc_platform_timer_t *timer =
		timer_container_of(timer, t, timer);
	unsigned long period = READ_ONCE(timer->period);

	/*
	 * Re-arm from the deadline just met, so that timer rounding and softirq
	 * latency do not shift the period. A callback that runs later than the
	 * next deadline skips ahead instead of trying to catch up.
	 */
	if (period) {
		unsigned long next = timer->timer.expires + period;

		if (time_before_eq(next, jiffies))
			next = jiffies + period;

		mod_timer(&timer->timer, next);
	}

	if (timer->cb)
		timer->cb(timer);
}

/** @brief Bind @p timer to @p conn and @p cb, disarmed. */
int ludlc_platform_init_timer(struct ludlc_connection *conn,
			      ludlc_platform_timer_t *timer,
			      ludlc_timer_cb_t cb)
{
	if (!conn || !timer || !cb)
		return -EINVAL;

	timer->conn = conn;
	timer->period = 0;
	timer->cb = cb;

	/* Also makes a timer shut down by destroy_timer() usable again. */
	timer_setup(&timer->timer, ludlc_timer_fn, 0);

	return 0;
}
EXPORT_SYMBOL_GPL(ludlc_platform_init_timer);

/** @brief Arm @p timer, unless its link is suspended. */
int ludlc_platform_start_timer(ludlc_platform_timer_t *timer,
			       const unsigned int delay,
			       const unsigned int period)
{
	if (!timer)
		return -EINVAL;

	/* Read by the callback arming the round after this one. */
	WRITE_ONCE(timer->period, period ? usecs_to_jiffies(period) : 0);

	/*
	 * Refused from any caller, however late in the suspend: the link starts
	 * over on resume and arms what it needs then.
	 */
	if (timer->conn &&
	    test_bit(LUDLC_CONN_SUSPENDED_F, &timer->conn->pconn.flags))
		return 0;

	mod_timer(&timer->timer, jiffies + usecs_to_jiffies(delay));

	return 0;
}
EXPORT_SYMBOL_GPL(ludlc_platform_start_timer);

/** @brief Disarm @p timer, with nothing of it left running. */
int ludlc_platform_stop_timer(ludlc_platform_timer_t *timer)
{
	if (!timer)
		return 0;

	/*
	 * Clear the period first so that the wait can end, the callback being
	 * the only thing that re-arms. Waiting is safe under conn->lock, where
	 * the core stops its timers, because the callback takes no lock.
	 */
	WRITE_ONCE(timer->period, 0);
	timer_delete_sync(&timer->timer);

	return 0;
}
EXPORT_SYMBOL_GPL(ludlc_platform_stop_timer);

/** @brief Disarm @p timer for good and wait for its last callback. */
void ludlc_platform_destroy_timer(ludlc_platform_timer_t *timer)
{
	if (!timer)
		return;

	/*
	 * Shutdown, not stop: an arm from a transport still working a link
	 * that is going away is ignored rather than left to fire into freed
	 * storage. Only init_timer() makes the timer usable again.
	 */
	WRITE_ONCE(timer->period, 0);
	timer_shutdown_sync(&timer->timer);

	timer->cb = NULL;
	timer->conn = NULL;
}
EXPORT_SYMBOL_GPL(ludlc_platform_destroy_timer);

/**
 * @brief Does @p drv drive @p dev?
 *
 * Matching is by compatible string from a DT node, an ACPI companion (PRP0001
 * nodes included) or, for the software nodes configfs builds its devices from,
 * the "compatible" property itself: of_match_device() cannot see those, having
 * no of_node to work from.
 *
 * @return 1 on a match, 0 otherwise.
 */
static int ludlc_bus_match(struct device *dev, const struct device_driver *drv)
{
	const struct of_device_id *id;
	int ret;

	/* A driver_override from user space decides the match on its own. */
	ret = device_match_driver_override(dev, drv);
	if (ret >= 0)
		return ret;

	if (dev->of_node)
		return of_driver_match_device(dev, drv);

	/*
	 * The ACPI companion also covers PRP0001 nodes, which describe
	 * themselves with a DT compatible string.
	 */
	if (acpi_driver_match_device(dev, drv))
		return 1;

	/* A driver that declares no compatibles matches nothing else. */
	if (!drv->of_match_table)
		return 0;

	/* Nothing else to go on: try the match table against the property. */
	for (id = drv->of_match_table; id->compatible[0]; id++)
		if (fwnode_property_match_string(dev_fwnode(dev), "compatible",
						 id->compatible) >= 0)
			return 1;

	return 0;
}

/**
 * @brief Describe @p dev to user space: its first channel, and an alias to
 * load a driver by.
 *
 * @return 0, or a negative errno when @p env is full.
 */
static int ludlc_bus_uevent(const struct device *dev,
			    struct kobj_uevent_env *env)
{
	const struct ludlc_device *ldev = to_ludlc_device(dev);
	const char *compatible;
	int ret;

	ret = add_uevent_var(env, "LUDLC_CHANNEL=%u", ldev->chans[0]);
	if (ret)
		return ret;

	ret = of_device_uevent_modalias(dev, env);
	if (ret != -ENODEV)
		return ret;

	ret = acpi_device_uevent_modalias(dev, env);
	if (ret != -ENODEV)
		return ret;

	/* Devices from configfs have only their "compatible" property. */
	if (!device_property_read_string(dev, "compatible", &compatible))
		return add_uevent_var(env, "MODALIAS=" LUDLC_OF_MODALIAS_FMT,
				      compatible);

	return 0;
}

/** @brief The channels this device owns, which identify it. */
static ssize_t channel_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	const struct ludlc_device *ldev = to_ludlc_device(dev);
	unsigned int i;
	int len = 0;

	for (i = 0; i < ldev->nchans; i++)
		len += sysfs_emit_at(buf, len, "%s%u", i ? " " : "",
				     ldev->chans[i]);

	return len + sysfs_emit_at(buf, len, "\n");
}
static DEVICE_ATTR_RO(channel);

/* The same alias as the uevent, for coldplug to read from sysfs. */
static ssize_t modalias_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	const char *compatible;
	int len;

	len = of_device_modalias(dev, buf, PAGE_SIZE);
	if (len != -ENODEV)
		return len;

	len = acpi_device_modalias(dev, buf, PAGE_SIZE - 1);
	if (len != -ENODEV)
		return len;

	if (device_property_read_string(dev, "compatible", &compatible))
		return -ENODEV;

	return sysfs_emit(buf, LUDLC_OF_MODALIAS_FMT "\n", compatible);
}
static DEVICE_ATTR_RO(modalias);

static struct attribute *ludlc_dev_attrs[] = {
	&dev_attr_channel.attr,
	&dev_attr_modalias.attr,
	NULL,
};
ATTRIBUTE_GROUPS(ludlc_dev);

/**
 * @brief Bind @p dev to its driver.
 *
 * @return 0, or the error the driver's probe() returned.
 */
static int ludlc_bus_probe(struct device *dev)
{
	struct ludlc_driver *ldrv = to_ludlc_driver(dev->driver);
	struct ludlc_device *ldev = to_ludlc_device(dev);
	struct ludlc_connection *conn = ldev->conn;
	bool up = READ_ONCE(conn->conn_state) == LUDLC_STATE_CONNECTED;
	int session = atomic_read(&conn->pconn.session);
	int ret;

	/* A device on its way off the bus takes no driver. */
	scoped_guard(mutex, &ldev->cb_lock)
		if (test_bit(LUDLC_DEV_REMOVED_F, &ldev->flags))
			return -ENODEV;

	if (device_property_read_bool(dev, "wakeup-source")) {
		ret = device_init_wakeup(dev, true);
		if (ret)
			dev_warn(dev,
				"cannot make it a wakeup source: %d\n", ret);
	}

	ret = ldrv->probe(ldev);
	if (ret) {
		/* The driver may have called ludlc_device_listen() first. */
		scoped_guard(mutex, &ldev->cb_lock)
			ldev->drv = NULL;

		device_init_wakeup(dev, false);
		return ret;
	}

	/*
	 * A probe() that began with the link down said nothing to an earlier
	 * peer: what it sent waited for the session that began meanwhile, so
	 * that is the session its driver belongs to.
	 */
	if (!up)
		session = atomic_read(&conn->pconn.session);

	/*
	 * The channel is dispatched to the driver once probe() has returned,
	 * unless the driver called ludlc_device_listen() to start earlier.
	 */
	scoped_guard(mutex, &ldev->cb_lock) {
		ldev->drv = ldrv;
		ldev->session = session;
	}

	return 0;
}

/** @brief Unbind @p dev from its driver, stopping its traffic first. */
static void ludlc_bus_remove(struct device *dev)
{
	struct ludlc_driver *ldrv = to_ludlc_driver(dev->driver);
	struct ludlc_device *ldev = to_ludlc_device(dev);

	/*
	 * Taking the lock waits for a callback that is already running, and
	 * clearing @drv stops the next one from entering a driver that is
	 * being removed.
	 */
	scoped_guard(mutex, &ldev->cb_lock)
		ldev->drv = NULL;

	if (ldrv->remove)
		ldrv->remove(ldev);

	/*
	 * A remove() with something left to say to the peer may have called
	 * ludlc_device_listen(). Nothing may reach the driver once it has
	 * returned, whether or not it stopped the traffic again itself.
	 */
	scoped_guard(mutex, &ldev->cb_lock)
		ldev->drv = NULL;

	device_init_wakeup(dev, false);
}

void ludlc_device_listen(struct ludlc_device *ldev)
{
	guard(mutex)(&ldev->cb_lock);
	ldev->drv = to_ludlc_driver(ldev->dev.driver);
}
EXPORT_SYMBOL_GPL(ludlc_device_listen);

void ludlc_device_unlisten(struct ludlc_device *ldev)
{
	/* As in remove(): the lock waits for a running callback to finish. */
	guard(mutex)(&ldev->cb_lock);
	ldev->drv = NULL;
}
EXPORT_SYMBOL_GPL(ludlc_device_unlisten);

/** @brief Quiesce @p dev on reboot or poweroff. */
static void ludlc_bus_shutdown(struct device *dev)
{
	struct ludlc_driver *ldrv;

	/* Called for every device on the bus, bound or not. */
	if (!dev->driver)
		return;

	ldrv = to_ludlc_driver(dev->driver);
	if (ldrv->shutdown)
		ldrv->shutdown(to_ludlc_device(dev));
}

/**
 * @brief Stop dispatching unsolicited packets to @p dev while it is suspended.
 *
 * This affects receive() only, and only between the start of the driver's
 * suspend() and the end of its resume(), unless the device is a wakeup source.
 * confirm() is never gated, because the link outlives every device on it, so a
 * driver can still send from its own suspend() and get an answer. Anything the
 * peer sends meanwhile is dropped, as for any channel with no driver bound;
 * the protocol acknowledges the packet regardless, and a driver that must not
 * miss anything can refuse the transition from its suspend().
 */
static void ludlc_bus_pm_gate(struct device *dev, bool suspended)
{
	struct ludlc_device *ldev = to_ludlc_device(dev);

	guard(mutex)(&ldev->cb_lock);
	__assign_bit(LUDLC_DEV_SUSPENDED_F, &ldev->flags, suspended);
}

/**
 * @brief Ask the driver about the coming suspend, but never let it be skipped.
 *
 * A positive return from prepare() tells the core the device is already in the
 * right state and that its suspend and resume can be skipped. This bus cannot
 * allow that: those two callbacks are where dispatch to a channel stops and
 * starts, so a device that skipped them would be handed packets while its
 * driver is suspended. The driver is called directly because
 * pm_generic_prepare() is not exported, and there is no matching complete()
 * because the core then calls the driver's own, which is all this would do.
 *
 * @return 0, or the driver's negative error.
 */
static int ludlc_bus_prepare(struct device *dev)
{
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;
	int ret = 0;

	if (pm && pm->prepare)
		ret = pm->prepare(dev);

	return ret < 0 ? ret : 0;
}

/** @brief Stop dispatch to the channel, then suspend the driver with @p op. */
static inline
int ludlc_bus_pm_down(struct device *dev, int (*op)(struct device *))
{
	int ret;

	ludlc_bus_pm_gate(dev, true);

	/* Nothing resumes a device whose suspend failed, so undo it here. */
	ret = op(dev);
	if (ret)
		ludlc_bus_pm_gate(dev, false);

	return ret;
}

/**
 * @brief Resume the driver with @p op, and allow dispatch again if it worked.
 *
 * A driver that failed to resume is in no state to be handed packets, so the
 * gate stays shut. Nothing resumes a device twice, so the channel stays quiet
 * until another suspend and resume bring the driver back: dispatch follows the
 * driver, not the system.
 */
static inline
int ludlc_bus_pm_up(struct device *dev, int (*op)(struct device *))
{
	int ret = op(dev);

	if (!ret)
		ludlc_bus_pm_gate(dev, false);

	return ret;
}

static int ludlc_bus_suspend(struct device *dev)
{
	return ludlc_bus_pm_down(dev, pm_generic_suspend);
}

static int ludlc_bus_freeze(struct device *dev)
{
	return ludlc_bus_pm_down(dev, pm_generic_freeze);
}

static int ludlc_bus_poweroff(struct device *dev)
{
	return ludlc_bus_pm_down(dev, pm_generic_poweroff);
}

static int ludlc_bus_resume(struct device *dev)
{
	return ludlc_bus_pm_up(dev, pm_generic_resume);
}

static int ludlc_bus_thaw(struct device *dev)
{
	return ludlc_bus_pm_up(dev, pm_generic_thaw);
}

static int ludlc_bus_restore(struct device *dev)
{
	return ludlc_bus_pm_up(dev, pm_generic_restore);
}

/*
 * Each transition is passed on as itself rather than folded into a single
 * suspend/resume pair: a driver that distinguishes hibernation from suspend
 * expects to be told which one it got.
 */
static const struct dev_pm_ops ludlc_bus_pm_ops = {
	.prepare	= ludlc_bus_prepare,
	.suspend	= ludlc_bus_suspend,
	.resume		= ludlc_bus_resume,
	.freeze		= ludlc_bus_freeze,
	.thaw		= ludlc_bus_thaw,
	.poweroff	= ludlc_bus_poweroff,
	.restore	= ludlc_bus_restore,
};

/*
 * Fields this bus deliberately leaves unset, and why:
 *
 * - .dev_name: every device is named after its link and channel before it is
 *   added, so the core never has to name one.
 * - .bus_groups: the counters, the channel and the alias each belong to one
 *   link or one device, and are exposed there.
 * - .drv_groups: i2c puts new_id/remove_id here for devices firmware never
 *   described; here such devices come from configfs, and driver_override
 *   selects the driver.
 * - .sync_state: a channel holds no resource on behalf of consumers that have
 *   not probed yet, only its own place in the map.
 * - .online/.offline: a peer that stops answering does not ask permission, and
 *   unbind covers the rest.
 * - .suspend/.resume: the pm_message_t pair predating dev_pm_ops, ignored by
 *   the core once .pm is set.
 * - .dma_configure, .dma_cleanup, .num_vf, .irq_get_affinity: a channel is a
 *   byte stream on the transport's port and owns no hardware of its own.
 * - .need_parent_lock: the transport removes its devices from its own remove
 *   path, where the driver core already holds the parent lock.
 * - runtime PM: a channel has nothing of its own to idle, and the port it
 *   shares with its siblings is the transport's responsibility.
 */
const struct bus_type ludlc_bus_type = {
	.name		 = "ludlc",
	.dev_groups	 = ludlc_dev_groups,
	.driver_override = true,
	.match		 = ludlc_bus_match,
	.uevent		 = ludlc_bus_uevent,
	.probe		 = ludlc_bus_probe,
	.remove		 = ludlc_bus_remove,
	.shutdown	 = ludlc_bus_shutdown,
	.pm		 = pm_sleep_ptr(&ludlc_bus_pm_ops),
};
EXPORT_SYMBOL_GPL(ludlc_bus_type);

/** @brief Free @p dev once the last reference to it is gone. */
static void ludlc_device_release(struct device *dev)
{
	kfree(to_ludlc_device(dev));
}

/**
 * @brief Whether the @p n channels in @p chans can be given to one device on
 * @p conn.
 *
 * Each has to fit below the tag of the link, the control channel is reserved
 * for the protocol, and no channel may appear twice, since a channel belongs
 * to one device and is claimed once.
 */
static bool ludlc_channels_ok(struct ludlc_connection *conn,
			      const ludlc_channel_t *chans, unsigned int n)
{
	struct device *parent = conn->pconn.dev;
	unsigned int id_max = ludlc_conn_chan_id_mask(conn);
	unsigned int i, j;

	if (!n || n > id_max) {
		dev_err(parent, "a device owns from one channel to %u, not %u\n",
			id_max, n);
		return false;
	}

	for (i = 0; i < n; i++) {
		if (chans[i] > id_max) {
			dev_err(parent, "channel %u overlaps the %u-bit tag\n",
				chans[i], ludlc_conn_chan_tag_bits(conn));
			return false;
		}

		if (chans[i] == CONFIG_LUDLC_CONTROL_CHANNEL) {
			dev_err(parent, "channel %u is reserved\n", chans[i]);
			return false;
		}

		for (j = 0; j < i; j++) {
			if (chans[j] != chans[i])
				continue;

			dev_err(parent, "channel %u is listed twice\n",
				chans[i]);
			return false;
		}
	}

	return true;
}

/**
 * @brief Allocate a device for @p nchans channels of @p conn, parented to the
 * link's device.
 *
 * @return The device, initialized but not yet visible, or an ERR_PTR. The
 * caller owns the reference; see ludlc_bus.h.
 */
struct ludlc_device *ludlc_device_alloc(struct ludlc_connection *conn,
					const ludlc_channel_t *chans,
					unsigned int nchans)
{
	struct ludlc_device *ldev;
	unsigned int i;

	/* The connection is the bus, so a device cannot exist without one. */
	if (!conn || !chans)
		return ERR_PTR(-EINVAL);

	if (!ludlc_channels_ok(conn, chans, nchans))
		return ERR_PTR(-EINVAL);

	ldev = kzalloc(struct_size(ldev, chans, nchans), GFP_KERNEL);
	if (!ldev)
		return ERR_PTR(-ENOMEM);

	ldev->conn = conn;
	ldev->nchans = nchans;
	for (i = 0; i < nchans; i++)
		ldev->chans[i] = chans[i];

	mutex_init(&ldev->cb_lock);

	device_initialize(&ldev->dev);
	ldev->dev.bus	  = &ludlc_bus_type;
	ldev->dev.parent  = conn->pconn.dev;
	ldev->dev.release = ludlc_device_release;

	return ldev;
}
EXPORT_SYMBOL_GPL(ludlc_device_alloc);

#ifdef CONFIG_ACPI
/**
 * @brief The channel _ADR gives @p fwnode, for a node with no "reg".
 *
 * @return true if @p chan was filled in.
 */
static bool ludlc_acpi_channel(struct device *parent,
			       struct fwnode_handle *fwnode, u32 *chan)
{
	struct acpi_device *adev = to_acpi_device_node(fwnode);
	u64 adr;

	if (!adev || !adev->pnp.type.bus_address)
		return false;

	adr = acpi_device_adr(adev);
	if (adr > LUDLC_CHANNEL_MAX) {
		dev_err(parent, "_ADR %llu is not a usable channel\n", adr);
		return false;
	}

	*chan = adr;

	return true;
}
#else
static bool ludlc_acpi_channel(struct device *parent,
			       struct fwnode_handle *fwnode, u32 *chan)
{
	return false;
}
#endif

/**
 * @brief Allocate a device for the channels @p fwnode names.
 *
 * Firmware records them where it records the address of a device on its parent
 * bus: the DT-style "reg" property, which may list several channels, or _ADR
 * on an ACPI node, which names one. Both are 32-bit, so each value must fit a
 * channel id.
 *
 * @return The device, or an ERR_PTR, including for firmware that names none.
 */
static struct ludlc_device *ludlc_fwnode_device_alloc(struct ludlc_connection *conn,
						      struct fwnode_handle *fwnode)
{
	struct device *parent = conn->pconn.dev;
	struct ludlc_device *ldev;
	ludlc_channel_t *chans;
	u32 *regs;
	int i, n, ret;

	n = fwnode_property_count_u32(fwnode, "reg");
	if (n <= 0) {
		ludlc_channel_t chan;
		u32 adr;

		if (!ludlc_acpi_channel(parent, fwnode, &adr)) {
			dev_err(parent, "%pfwP names no channel\n", fwnode);
			return ERR_PTR(-EINVAL);
		}

		chan = adr;

		return ludlc_device_alloc(conn, &chan, 1);
	}

	regs = kmalloc_array(n, sizeof(*regs), GFP_KERNEL);
	chans = kmalloc_array(n, sizeof(*chans), GFP_KERNEL);
	if (!regs || !chans) {
		ldev = ERR_PTR(-ENOMEM);
		goto out;
	}

	ret = fwnode_property_read_u32_array(fwnode, "reg", regs, n);
	if (ret) {
		ldev = ERR_PTR(ret);
		goto out;
	}

	for (i = 0; i < n; i++) {
		if (regs[i] > LUDLC_CHANNEL_MAX) {
			dev_err(parent, "channel %u is out of range\n",
				regs[i]);
			ldev = ERR_PTR(-EINVAL);
			goto out;
		}

		chans[i] = regs[i];
	}

	ldev = ludlc_device_alloc(conn, chans, n);

out:
	kfree(chans);
	kfree(regs);

	return ldev;
}

/**
 * @brief Make @p ldev visible on its bus, claiming its channels.
 *
 * Each connection is its own bus, so a channel only has to be free on that
 * link.
 *
 * @return 0, -EBUSY if a sibling device already owns one of the channels, or
 * another negative errno.
 */
int ludlc_device_add(struct ludlc_device *ldev)
{
	struct xarray *channels;
	unsigned int i;
	int ret;

	if (!ldev || !ldev->conn)
		return -EINVAL;

	channels = &ldev->conn->pconn.channels;

	/*
	 * Channels are unique per link, so the name is scoped by the parent,
	 * the way SPI names its devices after their controller. A device
	 * owning several channels is named after the first.
	 */
	if (ldev->dev.parent)
		ret = dev_set_name(&ldev->dev, "%s.%u",
				   dev_name(ldev->dev.parent), ldev->chans[0]);
	else
		ret = dev_set_name(&ldev->dev, "ludlc%u", ldev->chans[0]);

	if (ret)
		return ret;

	/* Claim every channel before the device becomes visible. */
	for (i = 0; i < ldev->nchans; i++) {
		ret = xa_insert(channels, ldev->chans[i], ldev, GFP_KERNEL);
		if (ret) {
			if (ret == -EBUSY)
				dev_err(ldev->conn->pconn.dev,
					"channel %u is already in use\n",
					ldev->chans[i]);
			goto err_release;
		}
	}

	ret = device_add(&ldev->dev);
	if (ret)
		goto err_release;

	return 0;

err_release:
	while (i--)
		xa_erase(channels, ldev->chans[i]);

	return ret;
}
EXPORT_SYMBOL_GPL(ludlc_device_add);

/** @brief Take @p ldev off its bus, if it is still on it. */
void ludlc_device_unregister(struct ludlc_device *ldev)
{
	struct xarray *channels;
	unsigned int i;

	if (!ldev)
		return;

	channels = &ldev->conn->pconn.channels;

	/*
	 * Setting the flag claims the removal: a device can be removed both by
	 * its creator and by the sweep that follows its link going down, and
	 * only the caller that sets it owns the reference dropped here.
	 */
	scoped_guard(mutex, &ldev->cb_lock)
		if (__test_and_set_bit(LUDLC_DEV_REMOVED_F, &ldev->flags))
			return;

	/*
	 * The driver goes first, while the channels still lead to the device:
	 * a remove() with a last word for the peer needs the answer to find
	 * its way back.
	 */
	device_release_driver(&ldev->dev);

	for (i = 0; i < ldev->nchans; i++)
		xa_erase(channels, ldev->chans[i]);

	device_unregister(&ldev->dev);
}
EXPORT_SYMBOL_GPL(ludlc_device_unregister);

/** @brief Free an enumerated device and release the node it held. */
static void ludlc_fwnode_device_release(struct device *dev)
{
	fwnode_handle_put(dev_fwnode(dev));
	ludlc_device_release(dev);
}

/**
 * @brief Create a bus device for every firmware child node of the link.
 *
 * Called when a link comes up, for whatever DT or ACPI describes below the
 * transport's port, each child naming its channels in a "reg" property. A
 * child that cannot be registered is skipped rather than failing the whole
 * link, the way SPI and I2C treat their controllers' children.
 */
static void ludlc_bus_populate(struct ludlc_connection *conn)
{
	struct fwnode_handle *child;
	struct device *parent;

	if (!conn || !conn->pconn.dev)
		return;

	parent = conn->pconn.dev;

	device_for_each_child_node(parent, child) {
		struct ludlc_device *ldev;
		int ret;

		if (!fwnode_device_is_available(child))
			continue;

		ldev = ludlc_fwnode_device_alloc(conn, child);
		if (IS_ERR(ldev)) {
			dev_warn(parent, "cannot register %pfwP: %pe\n", child,
				 ldev);
			continue;
		}

		/* The device holds the child node until it is released. */
		ldev->dev.release = ludlc_fwnode_device_release;
		device_set_node(&ldev->dev, fwnode_handle_get(child));
		__set_bit(LUDLC_DEV_ENUMERATED_F, &ldev->flags);

		ret = ludlc_device_add(ldev);
		if (ret) {
			dev_warn(parent, "cannot register %pfwP: %d\n",
				 child, ret);
			ludlc_device_put(ldev);
		}
	}
}

/**
 * @brief Take the devices on @p conn off the bus.
 *
 * @param enumerated_only Remove only the devices the link enumerated itself,
 * rather than all of them, which is what a link going away for good needs.
 *
 * The walk only reports that a channel was in use; the device behind it may
 * already be going away, and a device owning several channels appears once per
 * channel. Looking it up again takes a reference, which makes it safe to
 * touch, and ludlc_device_unregister() decides which caller removes it.
 */
static void ludlc_bus_sweep(struct ludlc_connection *conn, bool enumerated_only)
{
	struct ludlc_device *ldev;
	unsigned long chan;
	void *entry;

	xa_for_each(&conn->pconn.channels, chan, entry) {
		ldev = ludlc_device_find_by_channel(conn, chan);
		if (!ldev)
			continue;

		if (!enumerated_only ||
		    test_bit(LUDLC_DEV_ENUMERATED_F, &ldev->flags))
			ludlc_device_unregister(ldev);

		ludlc_device_put(ldev);
	}
}

/**
 * @brief Bind the drivers of the devices the link did not enumerate again.
 *
 * Those devices belong to whoever created them and stay on the bus when the
 * peer goes away, so their drivers are still bound from the session before.
 * The peer of a new session may have restarted and lost what it was told, so
 * each driver is removed and probed again, as a driver of an enumerated
 * device is when its device comes back. A device left without a driver,
 * because its probe() found no peer to talk to, gets one the same way.
 */
static void ludlc_bus_reprobe(struct ludlc_connection *conn)
{
	int session = atomic_read(&conn->pconn.session);
	struct ludlc_device *ldev;
	unsigned long chan;
	void *entry;

	xa_for_each(&conn->pconn.channels, chan, entry) {
		bool fresh;

		ldev = ludlc_device_find_by_channel(conn, chan);
		if (!ldev)
			continue;

		/* Once per device, however many channels it owns. */
		if (chan != ldev->chans[0] ||
		    test_bit(LUDLC_DEV_ENUMERATED_F, &ldev->flags)) {
			ludlc_device_put(ldev);
			continue;
		}

		/*
		 * A driver probed in this session has met this peer. The
		 * device lock waits for a probe() under way, whose driver may
		 * turn out to be one of those.
		 */
		device_lock(&ldev->dev);
		scoped_guard(mutex, &ldev->cb_lock)
			fresh = ldev->dev.driver && ldev->session == session;
		device_unlock(&ldev->dev);

		if (!fresh && device_reprobe(&ldev->dev))
			dev_warn(&ldev->dev, "cannot bind a driver again\n");

		ludlc_device_put(ldev);
	}
}

/**
 * @brief Bring the bus in line with the link.
 *
 * Deferred out of the link handlers because adding a device probes its driver,
 * which may need to talk to the peer: those handlers run either on the thread
 * that has just received a packet or on the one holding the port to send one,
 * and neither can carry that traffic while it waits for a probe.
 *
 * The link state is read here rather than taken as an argument, so what is
 * followed is where the link has got to, not each step of the way. A session
 * that ended is the exception: it is remembered, because the next one may
 * have begun by the time this runs, and its peer may be one that restarted. The
 * devices of the old session are then removed before those of the new one are
 * created, so every driver meets the peer afresh.
 */
static void ludlc_bus_link_workfn(struct work_struct *work)
{
	struct ludlc_platform_connection *pconn =
		container_of(work, struct ludlc_platform_connection, bus_work);
	struct ludlc_connection *conn =
		container_of(pconn, struct ludlc_connection, pconn);
	bool up = READ_ONCE(conn->conn_state) == LUDLC_STATE_CONNECTED;

	/* Another caller holds the bus and will resync it. */
	if (test_bit(LUDLC_CONN_BUS_HOLD_F, &pconn->flags))
		return;

	/* The session the bus was populated for is over, whatever came next. */
	if (test_and_clear_bit(LUDLC_CONN_SESSION_LOST_F, &pconn->flags) &&
	    test_and_clear_bit(LUDLC_CONN_POPULATED_F, &pconn->flags))
		ludlc_bus_sweep(conn, true);

	/* The bus already matches the link. */
	if (up == test_bit(LUDLC_CONN_POPULATED_F, &pconn->flags))
		return;

	assign_bit(LUDLC_CONN_POPULATED_F, &pconn->flags, up);

	if (up) {
		ludlc_bus_populate(conn);
		ludlc_bus_reprobe(conn);
	} else {
		ludlc_bus_sweep(conn, true);
	}
}

/**
 * @brief Keep what the peer announced, for the drivers on this link.
 *
 * Runs before the link comes up, so a driver probing a device the link
 * enumerates finds it in place.
 */
static void ludlc_bus_on_caps(struct ludlc_connection *conn, const void *data,
			      ludlc_payload_size_t size, void *user_arg)
{
	struct ludlc_platform_connection *pconn = &conn->pconn;
	unsigned long flags;

	/* The link refuses a packet larger than the build carries. */
	if (WARN_ON_ONCE(size > sizeof(pconn->peer_caps)))
		return;

	spin_lock_irqsave(&pconn->caps_lock, flags);
	if (size)
		memcpy(pconn->peer_caps, data, size);
	pconn->peer_caps_len = size;
	spin_unlock_irqrestore(&pconn->caps_lock, flags);
}

/** @brief The peer answered: the devices on this link can be created. */
static void ludlc_bus_on_connect(struct ludlc_connection *conn, void *user_arg)
{
	atomic_inc(&conn->pconn.session);
	schedule_work(&conn->pconn.bus_work);
}

/** @brief The peer is gone, and with it everything the link enumerated. */
static void ludlc_bus_on_disconnect(struct ludlc_connection *conn,
				    void *user_arg)
{
	set_bit(LUDLC_CONN_SESSION_LOST_F, &conn->pconn.flags);
	schedule_work(&conn->pconn.bus_work);
}

/**
 * @brief Is there a driver on @p ldev to pass a packet to?
 *
 * One that is bound and either awake or suspended as a wakeup source. Marking
 * a device as a wakeup source, in firmware or from user space, asks for the
 * peer to be heard whatever the system is doing.
 */
static bool ludlc_dev_listening(struct ludlc_device *ldev)
{
	lockdep_assert_held(&ldev->cb_lock);

	if (!ldev->drv)
		return false;

	return !test_bit(LUDLC_DEV_SUSPENDED_F, &ldev->flags) ||
	       device_may_wakeup(&ldev->dev);
}

/** @brief Pass a packet to the driver on @p chan, if one is listening. */
static void ludlc_bus_on_recv(struct ludlc_connection *conn,
			      ludlc_channel_t chan, const void *data,
			      ludlc_payload_size_t size,
			      ludlc_timestamp_t tstamp, void *user_arg)
{
	struct ludlc_device *ldev;

	ldev = ludlc_device_find_by_channel(conn, chan);
	if (!ldev) {
		dev_dbg_ratelimited(conn->pconn.dev,
				    "no device on channel %u\n",
				    ludlc_chan_id(conn, chan));
		return;
	}

	scoped_guard(mutex, &ldev->cb_lock)
		if (ludlc_dev_listening(ldev) && ldev->drv->receive)
			ldev->drv->receive(ldev, chan, data, size, tstamp);

	ludlc_device_put(ldev);
}

/** @brief Tell the driver on @p chan how the packet it sent turned out. */
static void ludlc_bus_on_confirm(struct ludlc_connection *conn, int error,
				 ludlc_channel_t chan, const void *data,
				 ludlc_payload_size_t size, void *user_arg)
{
	struct ludlc_device *ldev;

	ldev = ludlc_device_find_by_channel(conn, chan);
	if (!ldev)
		return;

	/*
	 * A suspend does not gate this: it answers for a packet the driver
	 * queued itself, and the answer is often that the link went down,
	 * which is what a suspend does to whatever was left in the queue.
	 */
	scoped_guard(mutex, &ldev->cb_lock)
		if (ldev->drv && ldev->drv->confirm)
			ldev->drv->confirm(ldev, chan, error, data, size);

	ludlc_device_put(ldev);
}

const struct ludlc_conn_cb ludlc_bus_conn_cb = {
	.on_connect	= ludlc_bus_on_connect,
	.on_disconnect	= ludlc_bus_on_disconnect,
	.on_recv	= ludlc_bus_on_recv,
	.on_confirm	= ludlc_bus_on_confirm,
	.on_caps	= ludlc_bus_on_caps,
};
EXPORT_SYMBOL_GPL(ludlc_bus_conn_cb);

ssize_t ludlc_device_peer_caps(struct ludlc_device *ldev, void *buf,
			       size_t len)
{
	struct ludlc_platform_connection *pconn;
	unsigned long flags;
	int size;

	if (!ldev || (!buf && len))
		return -EINVAL;

	pconn = &ldev->conn->pconn;

	spin_lock_irqsave(&pconn->caps_lock, flags);
	size = pconn->peer_caps_len;
	if (size > 0 && len)
		memcpy(buf, pconn->peer_caps, min_t(size_t, size, len));
	spin_unlock_irqrestore(&pconn->caps_lock, flags);

	return size < 0 ? -ENODATA : size;
}
EXPORT_SYMBOL_GPL(ludlc_device_peer_caps);

/**
 * @brief Wait for whatever the bus was in the middle of.
 *
 * The caller has set LUDLC_CONN_BUS_HELD_F, so anything queued after this
 * returns immediately; this waits for the run already under way.
 */
void ludlc_bus_conn_hold(struct ludlc_connection *conn)
{
	cancel_work_sync(&conn->pconn.bus_work);
}

/** @brief Bring the bus back in line with a link it could not follow. */
void ludlc_bus_conn_resync(struct ludlc_connection *conn)
{
	schedule_work(&conn->pconn.bus_work);
}

/**
 * @brief Look up the device owning @p chan on @p conn.
 *
 * The devices are filed under the channel alone, so a tag in @p chan is left
 * out of the lookup.
 *
 * @return The device with a reference held, or NULL if the channel is
 * unclaimed. Release it with ludlc_device_put().
 */
struct ludlc_device *ludlc_device_find_by_channel(struct ludlc_connection *conn,
						  ludlc_channel_t chan)
{
	struct xarray *channels;
	struct ludlc_device *ldev;

	if (!conn)
		return NULL;

	channels = &conn->pconn.channels;

	xa_lock(channels);
	ldev = xa_load(channels, ludlc_chan_id(conn, chan));
	if (ldev)
		get_device(&ldev->dev);
	xa_unlock(channels);

	return ldev;
}
EXPORT_SYMBOL_GPL(ludlc_device_find_by_channel);

/**
 * @brief Take the width of the tag in the channel field from the firmware
 * node of @p dev.
 *
 * As with the checksum and the frame size, a width no link can run is warned
 * about and leaves @p cfg as it was: a link that comes up without the tag the
 * peer expects is easier to diagnose than one that does not come up.
 */
void ludlc_fw_chan_tag_bits(struct device *dev, struct ludlc_conn_cfg *cfg)
{
	u32 bits;

	if (device_property_read_u32(dev, "ludlc,channel-tag-bits", &bits))
		return;

	if (!ludlc_chan_tag_bits_ok(bits)) {
		dev_warn(dev, "cannot run a tag of %u bits, keeping %u\n",
			 bits, (unsigned int)cfg->chan_tag_bits);
		return;
	}

	cfg->chan_tag_bits = bits;
}
EXPORT_SYMBOL_GPL(ludlc_fw_chan_tag_bits);

void ludlc_driver_unregister(struct ludlc_driver *ldrv)
{
	if (ldrv)
		driver_unregister(&ldrv->drv);
}
EXPORT_SYMBOL_GPL(ludlc_driver_unregister);

/**
 * @brief Register @p ldrv, owned by @p owner so it cannot be unloaded while
 * bound.
 *
 * @return 0, -EINVAL for a driver with no probe(), or a negative errno from
 * the driver core.
 */
int __ludlc_driver_register(struct ludlc_driver *ldrv, struct module *owner)
{
	if (!ldrv)
		return -EINVAL;

	/*
	 * probe() is where a driver learns which device it got; without it the
	 * receive() and confirm() callbacks would have nothing to work with.
	 */
	if (!ldrv->probe)
		return -EINVAL;

	ldrv->drv.bus = &ludlc_bus_type;
	ldrv->drv.owner = owner;

	return driver_register(&ldrv->drv);
}
EXPORT_SYMBOL_GPL(__ludlc_driver_register);

/*
 * The four PM phases of a link. None of them may be called with conn->lock
 * held: each either takes it or waits for work that does, so that would
 * deadlock.
 */

/** @brief Hold the bus as it is for the coming transition. */
void ludlc_conn_prepare(struct ludlc_connection *conn)
{
	if (!conn)
		return;

	/*
	 * The link itself keeps running: the port works until its own driver
	 * suspends, and the devices on it can still talk to the peer. Only the
	 * bus stops following it, because a peer dropping out mid-transition
	 * would otherwise remove its devices from a bus the driver core is
	 * walking.
	 */
	if (test_and_set_bit(LUDLC_CONN_BUS_HOLD_F, &conn->pconn.flags))
		ludlc_bus_conn_hold(conn);
}
EXPORT_SYMBOL_GPL(ludlc_conn_prepare);

/**
 * @brief Tell the peer this end is going down.
 *
 * @return Whether anything was queued, which a transport that buffers its
 * writes has to send before its port suspends.
 */
bool ludlc_conn_send_disconnect(struct ludlc_connection *conn)
{
	int ret;

	if (!conn)
		return false;

	/*
	 * A link carrying a device that is meant to wake the system must be
	 * left alone: that device already arranged with the peer, from its own
	 * prepare() or suspend(), what happens across the transition.
	 */
	if (conn->pconn.dev && (device_may_wakeup(conn->pconn.dev) ||
				device_wakeup_path(conn->pconn.dev)))
		return false;

	/* Refused outside a session, since there is nothing to end. */
	ret = ludlc_send_disconnect(conn);
	if (ret) {
		dev_dbg(conn->pconn.dev,
			"cannot send the disconnect: %d\n", ret);
		return false;
	}

	return true;
}
EXPORT_SYMBOL_GPL(ludlc_conn_send_disconnect);

/** @brief Stop the link timers: none may fire into a suspended port. */
void ludlc_conn_suspend(struct ludlc_connection *conn)
{
	if (!conn)
		return;

	/*
	 * Set the bit before stopping the timers, so that a start_timer() call
	 * already on its way in is refused rather than re-arming one.
	 */
	set_bit(LUDLC_CONN_SUSPENDED_F, &conn->pconn.flags);

	ludlc_platform_stop_timer(&conn->ping_timer);
	ludlc_platform_stop_timer(&conn->wd_timer);

	/*
	 * A watchdog that fired on the way in has already queued its work, and
	 * it must not end the session behind a suspended bus. The watchdog
	 * armed on resume checks again.
	 */
	cancel_work_sync(&conn->pconn.timeout_work);
}
EXPORT_SYMBOL_GPL(ludlc_conn_suspend);

/** @brief Start the link timers again, and the link itself if it ended. */
void ludlc_conn_resume(struct ludlc_connection *conn)
{
	int ret;

	if (!conn)
		return;

	/* The port can be used again. */
	clear_bit(LUDLC_CONN_SUSPENDED_F, &conn->pconn.flags);

	/*
	 * A link that survived the suspend only needs its timers started
	 * again: the first packet sent re-arms the ping. Only an arriving
	 * packet re-arms the watchdog, though, and a peer that was reset while
	 * the system slept sends none, so it is armed here instead. Left to
	 * the RX path, a session whose far end is gone would never time out.
	 */
	if (READ_ONCE(conn->conn_state) == LUDLC_STATE_CONNECTED) {
		ludlc_watchdog_restart(conn);
		ludlc_platform_request_tx(conn);
		return;
	}

	/*
	 * A link that ended restarts here rather than later, because resume
	 * runs from the port outwards: the devices resuming behind it find a
	 * link that is already coming back. Whatever was still queued when the
	 * system suspended fails as it restarts, and every driver gets its
	 * confirm() for it.
	 */
	ret = ludlc_connect(conn);
	if (ret)
		dev_warn(conn->pconn.dev, "cannot restart the link: %d\n", ret);
}
EXPORT_SYMBOL_GPL(ludlc_conn_resume);

/** @brief The transition is over: let the bus follow its link again. */
void ludlc_conn_complete(struct ludlc_connection *conn)
{
	if (!conn)
		return;

	/*
	 * Every device is awake and the link has had the whole resume to come
	 * back, so the bus can follow it into whatever state it reached.
	 */
	if (test_and_clear_bit(LUDLC_CONN_BUS_HOLD_F, &conn->pconn.flags))
		ludlc_bus_conn_resync(conn);
}
EXPORT_SYMBOL_GPL(ludlc_conn_complete);

/** @brief Ask the transport for another TX round, with or without new data. */
void ludlc_platform_request_tx(struct ludlc_connection *conn)
{
	if (!conn)
		return;

	set_bit(LUDLC_CONN_FORCE_TX_F, &conn->pconn.flags);
	if (conn->pconn.start_tx)
		conn->pconn.start_tx(conn);
}
EXPORT_SYMBOL_GPL(ludlc_platform_request_tx);

/** @brief End the session the watchdog gave up on, where that may sleep. */
static void ludlc_conn_timeout_workfn(struct work_struct *work)
{
	struct ludlc_platform_connection *pconn =
		container_of(work, struct ludlc_platform_connection,
			     timeout_work);
	struct ludlc_connection *conn =
		container_of(pconn, struct ludlc_connection, pconn);

	/* A session that ended meanwhile leaves the core nothing to do. */
	ludlc_handle_disconnect(conn);
}

/** @brief The peer has been silent too long: hand the session to the work. */
void ludlc_platform_conn_timeout(struct ludlc_connection *conn)
{
	/*
	 * Not inline: this runs in the watchdog's softirq, and ending a session
	 * takes conn->lock and calls back into the drivers on the bus.
	 */
	if (conn)
		schedule_work(&conn->pconn.timeout_work);
}

/*
 * The caller is in the work that ends a session the watchdog gave up on,
 * which the receiving thread does on the other platforms, or in a context
 * the transport runs the link in, which the transport knows. Never an
 * interrupt.
 */
bool ludlc_in_callback(struct ludlc_connection *conn)
{
	if (!conn || !in_task())
		return false;

	if (current_work() == &conn->pconn.timeout_work)
		return true;

	return conn->pconn.in_callback && conn->pconn.in_callback(conn);
}
EXPORT_SYMBOL_GPL(ludlc_in_callback);

#if CONFIG_LUDLC_STATS
/**
 * @brief The link an attribute reports on.
 *
 * A connection is not a device of its own, and the device it hangs under
 * belongs to the transport, so each link carries its own copy of the
 * attributes, each pointing back at it.
 */
static struct ludlc_connection *ludlc_stats_conn(struct device_attribute *attr)
{
	struct dev_ext_attribute *ea =
		container_of(attr, struct dev_ext_attribute, attr);

	return ea->var;
}

/*
 * One counter per file, named after the field it reports. The RX and TX paths
 * increment these without a lock, so a reader can catch the set mid-update.
 * Each counter is still read as a whole, which is all that is required.
 */
#define LUDLC_STATS_ATTR(_name)						\
static ssize_t _name##_show(struct device *dev,				\
			    struct device_attribute *attr, char *buf)	\
{									\
	struct ludlc_connection *conn = ludlc_stats_conn(attr);		\
									\
	return sysfs_emit(buf, "%u\n", READ_ONCE(conn->stats._name));	\
}									\
static DEVICE_ATTR_RO(_name)

LUDLC_STATS_ATTR(rx_packet);	/* Packets received and accepted */
LUDLC_STATS_ATTR(tx_packet);	/* Packets encoded and sent */
LUDLC_STATS_ATTR(bad_csum);	/* Received with a bad checksum */
LUDLC_STATS_ATTR(jabber);	/* Received oversized, cut short */
LUDLC_STATS_ATTR(on_conn);	/* Times the link came up */
LUDLC_STATS_ATTR(on_disconn);	/* Times it went down again */
LUDLC_STATS_ATTR(dropped);	/* Dropped, in either direction */
LUDLC_STATS_ATTR(overrun);	/* Lost because RX could not keep up */

static const struct device_attribute * const ludlc_stats_attrs[] = {
	&dev_attr_rx_packet,
	&dev_attr_tx_packet,
	&dev_attr_bad_csum,
	&dev_attr_jabber,
	&dev_attr_on_conn,
	&dev_attr_on_disconn,
	&dev_attr_dropped,
	&dev_attr_overrun,
};

#define LUDLC_STATS_NUM		ARRAY_SIZE(ludlc_stats_attrs)

/*
 * The counters of one link, under the device its transport registered for it:
 * <dev>/ludlc_stats/<counter>.
 */
struct ludlc_stats_sysfs {
	struct attribute_group group;
	struct attribute *attrs[LUDLC_STATS_NUM + 1];
	struct dev_ext_attribute ext[LUDLC_STATS_NUM];
};

/**
 * @brief Publish @p conn's counters under @p dev.
 *
 * @return 0, or a negative errno with nothing added.
 */
static int ludlc_stats_sysfs_add(struct ludlc_connection *conn,
				 struct device *dev)
{
	struct ludlc_stats_sysfs *stats;
	unsigned int i;
	int ret;

	stats = kzalloc(sizeof(*stats), GFP_KERNEL);
	if (!stats)
		return -ENOMEM;

	for (i = 0; i < LUDLC_STATS_NUM; i++) {
		struct dev_ext_attribute *ea = &stats->ext[i];

		ea->attr = *ludlc_stats_attrs[i];
		sysfs_attr_init(&ea->attr.attr);
		ea->var = conn;

		stats->attrs[i] = &ea->attr.attr;
	}

	stats->group.name = "ludlc_stats";
	stats->group.attrs = stats->attrs;

	ret = device_add_group(dev, &stats->group);
	if (ret) {
		kfree(stats);
		return ret;
	}

	conn->pconn.stats = stats;

	return 0;
}

/** @brief Remove @p conn's counters from sysfs. */
static void ludlc_stats_sysfs_remove(struct ludlc_connection *conn)
{
	struct ludlc_stats_sysfs *stats = conn->pconn.stats;

	if (!stats)
		return;

	conn->pconn.stats = NULL;
	device_remove_group(conn->pconn.dev, &stats->group);
	kfree(stats);
}
#else
static int ludlc_stats_sysfs_add(struct ludlc_connection *conn,
				 struct device *dev)
{
	return 0;
}

static void ludlc_stats_sysfs_remove(struct ludlc_connection *conn)
{
}
#endif /* CONFIG_LUDLC_STATS */

/**
 * @brief Give @p conn the device representing its local end.
 *
 * @return 0, -EBUSY if the link already has one, or a negative errno from
 * adding the counters.
 */
int ludlc_conn_set_device(struct ludlc_connection *conn, struct device *dev)
{
	int ret;

	if (!conn || !dev)
		return -EINVAL;

	/* A link keeps the same device for its whole lifetime. */
	if (conn->pconn.dev)
		return -EBUSY;

	conn->pconn.dev = get_device(dev);

	ret = ludlc_stats_sysfs_add(conn, dev);
	if (ret) {
		put_device(dev);
		conn->pconn.dev = NULL;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(ludlc_conn_set_device);

/** @brief Set up the Linux side of @p conn: its flags and its bus. */
int ludlc_platform_conn_init(struct ludlc_connection *conn)
{
	if (!conn)
		return -EINVAL;

	conn->pconn.flags = 0;
	conn->pconn.start_tx = NULL;
	conn->pconn.in_callback = NULL;
	conn->pconn.dev = NULL;
	conn->pconn.stats = NULL;

	xa_init(&conn->pconn.channels);
	INIT_WORK(&conn->pconn.bus_work, ludlc_bus_link_workfn);
	INIT_WORK(&conn->pconn.timeout_work, ludlc_conn_timeout_workfn);

	spin_lock_init(&conn->pconn.caps_lock);
	conn->pconn.peer_caps_len = -1;

	return 0;
}

/** @brief Tear down the Linux side of @p conn: its bus, then its device. */
void ludlc_platform_conn_destroy(struct ludlc_connection *conn)
{
	if (!conn)
		return;

	conn->pconn.start_tx = NULL;
	conn->pconn.in_callback = NULL;

	/*
	 * The timers are shut down by now, so nothing queues this again. It
	 * goes first because ending a session is what asks the bus to follow.
	 */
	cancel_work_sync(&conn->pconn.timeout_work);

	/* The devices below @dev go first, then what the link added to it. */
	cancel_work_sync(&conn->pconn.bus_work);

	/*
	 * The connection is the bus, so nothing may outlive it. The code that
	 * created the devices the link did not enumerate is expected to have
	 * removed them by now; remove what is left rather than leave it
	 * pointing at a freed link.
	 */
	ludlc_bus_sweep(conn, false);
	xa_destroy(&conn->pconn.channels);

	ludlc_stats_sysfs_remove(conn);

	put_device(conn->pconn.dev);
	conn->pconn.dev = NULL;
}

/**
 * @brief This transport's @ref ludlc_get_timestamp_t: the monotonic ktime.
 *
 * Nanoseconds, which is finer than the protocol needs: any monotonic unit will
 * do as long as two consecutive packets get different values.
 */
int ludlc_default_get_timestamp(ludlc_timestamp_t *ts)
{
	if (!ts)
		return -EINVAL;

	*ts = ktime_get();
	return 0;
}
EXPORT_SYMBOL_GPL(ludlc_default_get_timestamp);

/**
 * @brief Settle the checksums, then publish the bus.
 *
 * In that order: a transport can register as soon as the bus exists, and a
 * link it brings up asks for a checksum immediately.
 */
static int __init ludlc_core_init(void)
{
	int ret = ludlc_csum_setup();

	if (ret)
		return ret;

	ret = bus_register(&ludlc_bus_type);
	if (ret < 0)
		pr_err("ludlc: failed to register LuDLC bus type!\n");

	return ret;
}

static void __exit ludlc_core_exit(void)
{
	bus_unregister(&ludlc_bus_type);
}

module_init(ludlc_core_init);
module_exit(ludlc_core_exit);

MODULE_AUTHOR("Andrey Volkov <andrey@volkov.fr>");
MODULE_DESCRIPTION("LuDLC Linux core glue and software bus");
MODULE_LICENSE("GPL");

// SPDX-License-Identifier: GPL-2.0-only
/**
 * @file linux/ludlc_platform.h
 *
 * @brief The platform abstraction ludlc.c expects, as the Linux kernel provides
 * it.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * Bit operations, allocation, locks and timers, mapped onto the kernel's
 * own, and the per-connection state the bus and the transports share.
 */

#ifndef __LUDLC_PLATFORM_H__
#define __LUDLC_PLATFORM_H__

#include <asm/byteorder.h>
#include <linux/types.h>
#include <linux/build_bug.h>
#include <linux/container_of.h>
#include <linux/bitops.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>

#include <ludlc_types.h>
#include <ludlc_proto.h>

struct device;
struct ludlc_csum;
struct ludlc_stats_sysfs;

#define LUDLC_CONTAINER_OF(p, t, m)	container_of(p, t, m)
/** @brief Bits in one @ref ludlc_platform_atomic_t. */
#define LUDLC_BITS_PER_WORD		BITS_PER_LONG

/** @brief Words of ludlc_platform_atomic_t needed to hold @p n bits. */
#define LUDLC_BITS_TO_WORDS(n)		BITS_TO_LONGS(n)

/*
 * The core uses numbered bit helpers (test/set/clear/test_and_*), so keep this
 * as an unsigned long bitset to map directly onto Linux bitops.
 */
typedef unsigned long ludlc_platform_atomic_t;

static inline bool ludlc_platform_test_bit(long nr, ludlc_platform_atomic_t *addr)
{
	return test_bit(nr, addr);
}

static inline void ludlc_platform_set_bit(long nr, ludlc_platform_atomic_t *addr)
{
	set_bit(nr, addr);
}

static inline void ludlc_platform_clear_bit(long nr, ludlc_platform_atomic_t *addr)
{
	clear_bit(nr, addr);
}

static inline bool ludlc_platform_test_and_clear_bit(long nr,
		ludlc_platform_atomic_t *addr)
{
	return test_and_clear_bit(nr, addr);
}

static inline bool ludlc_platform_test_and_set_bit(long nr,
		ludlc_platform_atomic_t *addr)
{
	return test_and_set_bit(nr, addr);
}

/**
 * @brief Atomically sets every bit of @p mask.
 *
 * atomic_long_t wraps the same word the bit calls above already reach, so the
 * cast only picks the other set of names for it.
 *
 * @param mask The bits to set.
 * @param addr The word they live in.
 */
static inline void ludlc_platform_set_mask(ludlc_platform_atomic_t mask,
					   ludlc_platform_atomic_t *addr)
{
	atomic_long_or((long)mask, (atomic_long_t *)addr);
}

/**
 * @brief Atomically clears every bit of @p mask.
 * @param mask The bits to clear.
 * @param addr The word they live in.
 */
static inline void ludlc_platform_clear_mask(ludlc_platform_atomic_t mask,
					     ludlc_platform_atomic_t *addr)
{
	atomic_long_andnot((long)mask, (atomic_long_t *)addr);
}

/**
 * @name Bitsets
 *
 * A bit here is picked at run time, to claim a slot out of a pool, rather
 * than named like the flags elsewhere. The calls above already take a bit
 * number and reach the word it lands in, so only the storage is declared here.
 * @{
 */
/** @brief Declares storage for a bitset of @p nbits bits. */
#define LUDLC_DECLARE_BITSET(name, nbits)	DECLARE_BITMAP(name, nbits)

/** @} */


#define LUDLC_DECLARE_LOCK(name)	struct mutex name
#define LUDLC_LOCK_INIT(lock)		mutex_init(lock)
#define LUDLC_LOCK(lock)		mutex_lock(lock)
#define LUDLC_UNLOCK(lock)		mutex_unlock(lock)

static inline void *ludlc_platform_alloc(size_t size)
{
	return kzalloc(size, GFP_KERNEL);
}

static inline void ludlc_platform_free(void *ptr)
{
	kfree(ptr);
}

struct ludlc_platform_timer;

typedef struct ludlc_platform_timer *ludlc_platform_timer_arg_t;
typedef void (*ludlc_timer_cb_t)(ludlc_platform_timer_arg_t arg);

/**
 * @struct ludlc_platform_timer
 * @brief One of the core's timers, as a kernel timer.
 *
 * They fire whole seconds apart - the ping, and a watchdog three times that -
 * so jiffies are resolution enough and an hrtimer would gain nothing. The cost
 * is context: a timer runs in softirq, so the core callbacks it carries only
 * queue work elsewhere - a TX round from the transport, a disconnect from
 * @c timeout_work - and none of them takes a lock.
 */
struct ludlc_platform_timer {
	struct timer_list	timer;
	ludlc_timer_cb_t	cb;
	struct ludlc_connection	*conn;

	/**
	 * @brief What a periodic timer re-arms with, in jiffies. 0: one-shot.
	 *
	 * Armed and stopped from one context, read by the callback in another,
	 * hence the _ONCE at both ends.
	 */
	unsigned long		period;
};

typedef struct ludlc_platform_timer ludlc_platform_timer_t;

/**
 * @struct ludlc_platform_connection
 * @brief What Linux keeps for a connection: its state, its device, its bus.
 */
struct ludlc_platform_connection {
	/**
	 * @brief The state of the link, for everything that has to follow it.
	 *
	 * @c POPULATED: the bus currently carries its devices. @c SUSPENDED:
	 * the timers are stopped and the port must not be written to, from the
	 * transport's suspend to its resume. @c BUS_HOLD: no device may be
	 * added to or removed from the bus while the driver core walks it for a
	 * system transition, from prepare to complete. @c SESSION_LOST: the
	 * peer went away since the bus last followed the link, so the devices
	 * it carries are those of a session that is over. Accessed atomically,
	 * unlike the fields that belong to a single lock or context: a PM
	 * transition writes two of these bits while the bus work and the link
	 * handlers write the others, and none may lose another's update.
	 */
#define LUDLC_CONN_FORCE_TX_F		0
#define LUDLC_CONN_POPULATED_F		1
#define LUDLC_CONN_SUSPENDED_F		2
#define LUDLC_CONN_BUS_HOLD_F		3
#define LUDLC_CONN_SESSION_LOST_F	4
	unsigned long			flags;

	/**
	 * @brief How many sessions this link has had with its peer.
	 *
	 * Counted when the peer answers. The bus keeps it with each driver it
	 * probes, to tell a driver that has met this peer from one bound in a
	 * session before.
	 */
	atomic_t			session;

	/** Ask the transport for a TX round. */
	void (*start_tx)(struct ludlc_connection *conn);

	/**
	 * @brief The device representing the local end of the link.
	 *
	 * Borrowed from the transport that registered it: the parent of every
	 * device on this bus, and the device the bus logs through. NULL until
	 * ludlc_conn_set_device().
	 */
	struct device			*dev;

	/** This link's counters, exposed under @c dev. */
	struct ludlc_stats_sysfs	*stats;

	/**
	 * @brief Channel to @ref ludlc_device map for this link.
	 *
	 * Every connection is its own bus, so a channel id only has to be
	 * unique within one connection.
	 */
	struct xarray			channels;

	/**
	 * @brief Where the bus is brought in line with @c conn_state.
	 *
	 * The link events cannot do this themselves: registering a device
	 * probes a driver that may need to talk to the peer, and the thread
	 * reporting the event is the one that would have to carry those
	 * packets, being either the RX thread or the TX one holding the port.
	 * This work item runs outside both.
	 */
	struct work_struct		bus_work;

	/**
	 * @brief Where the watchdog ends the session, its own softirq being no
	 * place to take conn->lock and call the drivers back.
	 */
	struct work_struct		timeout_work;

	/**
	 * @brief What the peer announced in its latest handshake, for the
	 * drivers on this link: see ludlc_device_peer_caps().
	 *
	 * Copied in before the link comes up, and replaced by the next
	 * handshake rather than cleared when the link goes down: the end of a
	 * session can be reported after the next handshake has announced, and
	 * clearing it then would bring the link up without it. The length is
	 * negative until the peer has announced anything. Both are read and
	 * written under @c caps_lock.
	 */
	spinlock_t			caps_lock;
	int				peer_caps_len;
	u8 peer_caps[CONFIG_LUDLC_MAX_PAYLOAD_SIZE];
};

static inline
struct ludlc_connection *ludlc_timer_arg_to_conn(ludlc_platform_timer_arg_t arg)
{
	return arg ? arg->conn : NULL;
}

/**
 * @brief Give @p conn the device representing its local end.
 *
 * That device is the parent of the devices on the link's bus, the device the
 * link logs through, and where its counters appear in sysfs. The connection
 * holds a reference to it until cleanup, and accepts only one such device in
 * its lifetime.
 *
 * @return 0, -EBUSY if the link already has one, or a negative errno.
 */
int ludlc_conn_set_device(struct ludlc_connection *conn, struct device *dev);

/**
 * @brief Taking a link through a system sleep, one call per PM phase, for a
 * transport to call from its dev_pm_ops.
 *
 * A suspend lasts longer than the watchdog, so the session does not survive
 * it. prepare() and complete() bracket the whole transition and stop the bus
 * from changing; suspend() and resume() bracket the suspend itself, stopping
 * the timers on the way in so that nothing fires into a suspended port, and
 * restarting the link on the way out. The restart is early because resume runs
 * from the port outwards and the devices resuming behind it may have something
 * to send. The suspend phase reaches the port last, once every device has been
 * through its own suspend() with the link still up, so
 * ludlc_conn_send_disconnect() is the last thing sent and the peer need not
 * wait for its watchdog.
 */
void ludlc_conn_prepare(struct ludlc_connection *conn);
void ludlc_conn_suspend(struct ludlc_connection *conn);
void ludlc_conn_resume(struct ludlc_connection *conn);
void ludlc_conn_complete(struct ludlc_connection *conn);

/**
 * @brief Tell the peer this end is going down, before the timers are stopped.
 *
 * Nothing is sent for a link that is not up, or for one carrying a device that
 * is expected to wake the system: that device and the peer have their own
 * arrangement, which this layer must not break.
 *
 * @return Whether anything was queued. A transport that buffers its writes has
 * to send that frame before its port suspends, or the peer is left with its
 * watchdog after all.
 */
bool ludlc_conn_send_disconnect(struct ludlc_connection *conn);

#endif /* _LUDLC_PLATFORM_H_ */

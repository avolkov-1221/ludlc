/* SPDX-License-Identifier: GPL-2.0-only */
/**
 * @file ludlc_compat.h
 *
 * @brief The helpers of later kernels, for a kernel before 6.11.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * The Makefile includes this file ahead of every source when the kernel is
 * older than 6.11. Each block gives one helper in the form the kernel that
 * introduced it has, under that kernel's version; a macro is given only when
 * the kernel defines none, so a kernel that already carries it keeps its own.
 * The headers a later kernel split out of kernel.h and device.h are beside
 * this file, in linux/, on the include path after the kernel's own.
 */
#ifndef LUDLC_COMPAT_H
#define LUDLC_COMPAT_H

#include <linux/version.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/kfifo.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pm.h>
#include <linux/property.h>
#include <linux/rwsem.h>
#include <linux/sysfs.h>
#include <linux/acpi.h>
#include <linux/configfs.h>
#include <linux/of_device.h>

/*
 * 6.5: cleanup.h, which the lock headers include from then on; before, the
 * copy in linux/ beside this file.
 */
#include <linux/cleanup.h>

/* 4.15: fwnode_handle_get() returns the node. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0)
static inline struct fwnode_handle *
ludlc_fwnode_handle_get(struct fwnode_handle *fwnode)
{
	fwnode_handle_get(fwnode);
	return fwnode;
}
#define fwnode_handle_get ludlc_fwnode_handle_get
#endif

/* 4.16: assign_bit() and __assign_bit() of bitops.h. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 16, 0)
static __always_inline void assign_bit(long nr, volatile unsigned long *addr,
				       bool value)
{
	if (value)
		set_bit(nr, addr);
	else
		clear_bit(nr, addr);
}

static __always_inline void __assign_bit(long nr, volatile unsigned long *addr,
					 bool value)
{
	if (value)
		__set_bit(nr, addr);
	else
		__clear_bit(nr, addr);
}
#endif

/* 4.16: sizeof_field() of stddef.h. */
#ifndef sizeof_field
#define sizeof_field(TYPE, MEMBER) sizeof((((TYPE *)0)->MEMBER))
#endif

/* 4.18: slab.h includes overflow.h, which struct_size() comes from. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 18, 0)
#include <linux/overflow.h>
#endif

/* 5.1: static_assert() of build_bug.h. */
#ifndef static_assert
#define static_assert(expr, ...) __static_assert(expr, ##__VA_ARGS__, #expr)
#define __static_assert(expr, msg, ...) _Static_assert(expr, msg)
#endif

/* 5.3: the count of an array property. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 3, 0)
static inline int fwnode_property_count_u32(const struct fwnode_handle *fwnode,
					    const char *propname)
{
	return fwnode_property_read_u32_array(fwnode, propname, NULL, 0);
}

static inline int device_property_count_u32(struct device *dev,
					    const char *propname)
{
	return device_property_read_u32_array(dev, propname, NULL, 0);
}
#endif

/* 5.3: a device looked up by name among those a driver is bound to. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 3, 0)
static inline int ludlc_device_match_name(struct device *dev, void *name)
{
	return sysfs_streq(dev_name(dev), name);
}

static inline struct device *
driver_find_device_by_name(struct device_driver *drv, const char *name)
{
	return driver_find_device(drv, NULL, (void *)name,
				  ludlc_device_match_name);
}
#endif

/* 5.6: BITS_TO_BYTES() of bitops.h. */
#ifndef BITS_TO_BYTES
#define BITS_TO_BYTES(nr)	DIV_ROUND_UP(nr, BITS_PER_TYPE(char))
#endif

/* 5.10: fsleep() of delay.h. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 10, 0)
static inline void fsleep(unsigned long usecs)
{
	if (usecs <= 10)
		udelay(usecs);
	else if (usecs <= 20000)
		usleep_range(usecs, 2 * usecs);
	else
		msleep(DIV_ROUND_UP(usecs, 1000));
}
#endif

/*
 * 5.10: sysfs_emit() and sysfs_emit_at() of sysfs.h. Not inline: a function
 * with a variable argument list cannot be, and the kernel's inline insists.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 10, 0)
static __maybe_unused __printf(2, 3)
int sysfs_emit(char *buf, const char *fmt, ...)
{
	va_list args;
	int len;

	if (WARN(!buf || offset_in_page(buf),
		 "invalid sysfs_emit: buf:%p\n", buf))
		return 0;

	va_start(args, fmt);
	len = vscnprintf(buf, PAGE_SIZE, fmt, args);
	va_end(args);

	return len;
}

static __maybe_unused __printf(3, 4)
int sysfs_emit_at(char *buf, int at, const char *fmt, ...)
{
	va_list args;
	int len;

	if (WARN(!buf || offset_in_page(buf) || at < 0 || at >= PAGE_SIZE,
		 "invalid sysfs_emit_at: buf:%p at:%d\n", buf, at))
		return 0;

	va_start(args, fmt);
	len = vscnprintf(buf + at, PAGE_SIZE - at, fmt, args);
	va_end(args);

	return len;
}
#endif

/* 5.11: device_wakeup_path() of pm_wakeup.h. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 11, 0)
static inline bool device_wakeup_path(struct device *dev)
{
	return IS_ENABLED(CONFIG_PM_SLEEP) && dev->power.wakeup_path;
}
#endif

/* 5.14: device_set_node(). */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 14, 0)
static inline void device_set_node(struct device *dev,
				   struct fwnode_handle *fwnode)
{
	dev->fwnode = fwnode;
	dev->of_node = to_of_node(fwnode);
}
#endif

/* 5.17: pm_sleep_ptr() and SYSTEM_SLEEP_PM_OPS() of pm.h. */
#ifndef PTR_IF
#define PTR_IF(cond, ptr)	((cond) ? (ptr) : NULL)
#endif
#ifndef pm_sleep_ptr
#define pm_sleep_ptr(_ptr) PTR_IF(IS_ENABLED(CONFIG_PM_SLEEP), (_ptr))
#endif
#ifndef SYSTEM_SLEEP_PM_OPS
#define SYSTEM_SLEEP_PM_OPS(suspend_fn, resume_fn) \
	.suspend = pm_sleep_ptr(suspend_fn), \
	.resume = pm_sleep_ptr(resume_fn), \
	.freeze = pm_sleep_ptr(suspend_fn), \
	.thaw = pm_sleep_ptr(resume_fn), \
	.poweroff = pm_sleep_ptr(suspend_fn), \
	.restore = pm_sleep_ptr(resume_fn),
#endif

/* 6.5: __counted_by() of compiler_types.h. */
#ifndef __counted_by
#define __counted_by(member)
#endif

/*
 * 6.8: the conditional guards of cleanup.h. The cleanup.h of 6.5 to 6.7, and
 * the one 6.1.79 took, has none, and its guards unlock without looking at the
 * lock. Give such a kernel the conditional mutex and rwsem guards that the
 * copy in linux/ beside this file gives a kernel before 6.5, as classes of
 * their own, with a scoped_cond_guard() that runs the fail statement instead
 * of the body when the lock was not taken.
 */
#ifndef scoped_cond_guard
DEFINE_CLASS(mutex_try, struct mutex *, if (_T) mutex_unlock(_T),
	     mutex_trylock(_T) ? _T : NULL, struct mutex *_T)
DEFINE_CLASS(mutex_intr, struct mutex *, if (_T) mutex_unlock(_T),
	     mutex_lock_interruptible(_T) ? NULL : _T, struct mutex *_T)
DEFINE_CLASS(mutex_kill, struct mutex *, if (_T) mutex_unlock(_T),
	     mutex_lock_killable(_T) ? NULL : _T, struct mutex *_T)
DEFINE_CLASS(rwsem_read_try, struct rw_semaphore *, if (_T) up_read(_T),
	     down_read_trylock(_T) ? _T : NULL, struct rw_semaphore *_T)
DEFINE_CLASS(rwsem_write_try, struct rw_semaphore *, if (_T) up_write(_T),
	     down_write_trylock(_T) ? _T : NULL, struct rw_semaphore *_T)

#define scoped_cond_guard(_name, _fail, args...)			\
	for (CLASS(_name, __ludlc_scope)(args), *__ludlc_done = NULL;	\
	     !__ludlc_done; __ludlc_done = (void *)1)			\
		if (!__ludlc_scope)					\
			_fail;						\
		else
#endif

/*
 * 6.10: kfifo_out_linear(), kfifo_out_linear_ptr() and kfifo_skip_count() of
 * kfifo.h, with the two lib/kfifo.c routines the first two rest on.
 */
#ifndef kfifo_out_linear_ptr
static inline unsigned int ludlc_kfifo_out_linear(struct __kfifo *fifo,
						  unsigned int *tail,
						  unsigned int n)
{
	unsigned int size = fifo->mask + 1;
	unsigned int off = fifo->out & fifo->mask;

	if (tail)
		*tail = off;

	return min3(n, fifo->in - fifo->out, size - off);
}

static inline unsigned int ludlc_kfifo_out_linear_r(struct __kfifo *fifo,
						    unsigned int *tail,
						    unsigned int n,
						    size_t recsize)
{
	unsigned char *data = fifo->data;
	unsigned int mask = fifo->mask;
	unsigned int l;

	if (fifo->in == fifo->out)
		return 0;

	if (tail)
		*tail = fifo->out + recsize;

	l = data[fifo->out & mask];
	if (--recsize)
		l |= data[(fifo->out + 1) & mask] << 8;

	return min(n, l);
}

#define kfifo_out_linear(fifo, tail, n) \
__kfifo_uint_must_check_helper( \
({ \
	typeof((fifo) + 1) __tmp = (fifo); \
	unsigned int *__tail = (tail); \
	unsigned long __n = (n); \
	const size_t __recsize = sizeof(*__tmp->rectype); \
	struct __kfifo *__kfifo = &__tmp->kfifo; \
	(__recsize) ? \
	ludlc_kfifo_out_linear_r(__kfifo, __tail, __n, __recsize) : \
	ludlc_kfifo_out_linear(__kfifo, __tail, __n); \
}) \
)

#define kfifo_out_linear_ptr(fifo, ptr, n) \
__kfifo_uint_must_check_helper( \
({ \
	typeof((fifo) + 1) ___tmp = (fifo); \
	unsigned int ___tail; \
	unsigned int ___n = kfifo_out_linear(___tmp, &___tail, (n)); \
	*(ptr) = ___tmp->kfifo.data + ___tail * kfifo_esize(___tmp); \
	___n; \
}) \
)
#endif

#ifndef kfifo_skip_count
#define	kfifo_skip_count(fifo, count) do { \
	typeof((fifo) + 1) __tmp = (fifo); \
	const size_t __recsize = sizeof(*__tmp->rectype); \
	struct __kfifo *__kfifo = &__tmp->kfifo; \
	if (__recsize) \
		__kfifo_skip_r(__kfifo, __recsize); \
	else \
		__kfifo->out += (count); \
} while(0)
#endif

/*
 * 4.15, 6.3 and 6.4: kernel calls that take the const pointers this port
 * passes. Before, each took a plain pointer it never wrote through, so the
 * wrappers below cast the const away until the version that made it const.
 */
#define ludlc_unconst(type, ptr)	((type)(ptr))

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0)
#define config_item_init_type_name(item, name, type)			\
	config_item_init_type_name(item, name,				\
		ludlc_unconst(struct config_item_type *, type))
#define config_group_init_type_name(group, name, type)			\
	config_group_init_type_name(group, name,			\
		ludlc_unconst(struct config_item_type *, type))
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)
#define bus_unregister(bus)						\
	bus_unregister(ludlc_unconst(struct bus_type *, bus))
#define bus_register_notifier(bus, nb)					\
	bus_register_notifier(ludlc_unconst(struct bus_type *, bus), nb)
#define bus_unregister_notifier(bus, nb)				\
	bus_unregister_notifier(ludlc_unconst(struct bus_type *, bus), nb)
#define of_device_uevent_modalias(dev, env)				\
	of_device_uevent_modalias(ludlc_unconst(struct device *, dev), env)
#define acpi_device_uevent_modalias(dev, env)				\
	acpi_device_uevent_modalias(ludlc_unconst(struct device *, dev), env)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
#define bus_register(bus)						\
	bus_register(ludlc_unconst(struct bus_type *, bus))
#define device_property_read_string(dev, name, val)			\
	device_property_read_string(ludlc_unconst(struct device *, dev),\
				    name, val)
#endif

#endif /* LUDLC_COMPAT_H */

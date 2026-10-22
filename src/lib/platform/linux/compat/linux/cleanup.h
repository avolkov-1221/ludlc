/* SPDX-License-Identifier: GPL-2.0 */
/**
 * @file compat/linux/cleanup.h
 *
 * @brief The scope-based cleanup helpers and lock guards of Linux 7.3, for a
 * kernel before 6.5, which has no <linux/cleanup.h>.
 *
 * @details
 * include/linux/cleanup.h of 7.3. On a kernel before 6.11 the Makefile puts
 * compat/ on the include path after the kernel's own headers, so this copy is
 * <linux/cleanup.h> only where the kernel has none, before 6.5. It differs
 * from the original in two blocks marked "Before 6.5:": the definitions that
 * the original takes from headers such a kernel lacks, and the guards that the
 * lock headers of 7.3 define.
 */
#ifndef _LINUX_CLEANUP_H
#define _LINUX_CLEANUP_H

#include <linux/compiler.h>
#include <linux/err.h>
#include <linux/args.h>
#include <linux/rwsem.h>

/*
 * Before 6.5: the headers of such a kernel lack these, but for
 * typeof_member(), which 5.3 and later define the same way. The definitions
 * are those of 7.3, from <linux/compiler_attributes.h>, <linux/container_of.h>,
 * <linux/compiler_types.h>, and <linux/compiler-context-analysis.h> without
 * the analysis.
 */
#define __cleanup(func)			__attribute__((__cleanup__(func)))
#define __nonnull_args(x...)		__attribute__((__nonnull__(x)))
#define __no_context_analysis
#define typeof_member(T, m)	typeof(((T*)0)->m)

#if __STDC_VERSION__ < 202311L
# define auto __auto_type
#endif

/**
 * DOC: scope-based cleanup helpers
 *
 * The "goto error" pattern is notorious for introducing subtle resource
 * leaks. It is tedious and error prone to add new resource acquisition
 * constraints into code paths that already have several unwind
 * conditions. The "cleanup" helpers enable the compiler to help with
 * this tedium and can aid in maintaining LIFO (last in first out)
 * unwind ordering to avoid unintentional leaks.
 *
 * As drivers make up the majority of the kernel code base, here is an
 * example of using these helpers to clean up PCI drivers. The target of
 * the cleanups are occasions where a goto is used to unwind a device
 * reference (pci_dev_put()), or unlock the device (pci_dev_unlock())
 * before returning.
 *
 * The DEFINE_FREE() macro can arrange for PCI device references to be
 * dropped when the associated variable goes out of scope::
 *
 *	DEFINE_FREE(pci_dev_put, struct pci_dev *, if (_T) pci_dev_put(_T))
 *	...
 *	struct pci_dev *dev __free(pci_dev_put) =
 *		pci_get_slot(parent, PCI_DEVFN(0, 0));
 *
 * The above will automatically call pci_dev_put() if @dev is non-NULL
 * when @dev goes out of scope (automatic variable scope). If a function
 * wants to invoke pci_dev_put() on error, but return @dev (i.e. without
 * freeing it) on success, it can do::
 *
 *	return no_free_ptr(dev);
 *
 * ...or::
 *
 *	return_ptr(dev);
 *
 * The DEFINE_GUARD() macro can arrange for the PCI device lock to be
 * dropped when the scope where guard() is invoked ends::
 *
 *	DEFINE_GUARD(pci_dev, struct pci_dev *, pci_dev_lock(_T), pci_dev_unlock(_T))
 *	...
 *	guard(pci_dev)(dev);
 *
 * The lifetime of the lock obtained by the guard() helper follows the
 * scope of automatic variable declaration. Take the following example::
 *
 *	func(...)
 *	{
 *		if (...) {
 *			...
 *			guard(pci_dev)(dev); // pci_dev_lock() invoked here
 *			...
 *		} // <- implied pci_dev_unlock() triggered here
 *	}
 *
 * Observe the lock is held for the remainder of the "if ()" block not
 * the remainder of "func()".
 *
 * The ACQUIRE() macro can be used in all places that guard() can be
 * used and additionally support conditional locks::
 *
 *	DEFINE_GUARD_COND(pci_dev, _try, pci_dev_trylock(_T))
 *	...
 *	ACQUIRE(pci_dev_try, lock)(dev);
 *	rc = ACQUIRE_ERR(pci_dev_try, &lock);
 *	if (rc)
 *		return rc;
 *	// @lock is held
 *
 * Now, when a function uses both __free() and guard()/ACQUIRE(), or
 * multiple instances of __free(), the LIFO order of variable definition
 * order matters. GCC documentation says:
 *
 * "When multiple variables in the same scope have cleanup attributes,
 * at exit from the scope their associated cleanup functions are run in
 * reverse order of definition (last defined, first cleanup)."
 *
 * When the unwind order matters it requires that variables be defined
 * mid-function scope rather than at the top of the file.  Take the
 * following example and notice the bug highlighted by "!!"::
 *
 *	LIST_HEAD(list);
 *	DEFINE_MUTEX(lock);
 *
 *	struct object {
 *	        struct list_head node;
 *	};
 *
 *	static struct object *alloc_add(void)
 *	{
 *	        struct object *obj;
 *
 *	        lockdep_assert_held(&lock);
 *	        obj = kzalloc(sizeof(*obj), GFP_KERNEL);
 *	        if (obj) {
 *	                LIST_HEAD_INIT(&obj->node);
 *	                list_add(obj->node, &list):
 *	        }
 *	        return obj;
 *	}
 *
 *	static void remove_free(struct object *obj)
 *	{
 *	        lockdep_assert_held(&lock);
 *	        list_del(&obj->node);
 *	        kfree(obj);
 *	}
 *
 *	DEFINE_FREE(remove_free, struct object *, if (_T) remove_free(_T))
 *	static int init(void)
 *	{
 *	        struct object *obj __free(remove_free) = NULL;
 *	        int err;
 *
 *	        guard(mutex)(&lock);
 *	        obj = alloc_add();
 *
 *	        if (!obj)
 *	                return -ENOMEM;
 *
 *	        err = other_init(obj);
 *	        if (err)
 *	                return err; // remove_free() called without the lock!!
 *
 *	        no_free_ptr(obj);
 *	        return 0;
 *	}
 *
 * That bug is fixed by changing init() to call guard() and define +
 * initialize @obj in this order::
 *
 *	guard(mutex)(&lock);
 *	struct object *obj __free(remove_free) = alloc_add();
 *
 * Given that the "__free(...) = NULL" pattern for variables defined at
 * the top of the function poses this potential interdependency problem
 * the recommendation is to always define and assign variables in one
 * statement and not group variable definitions at the top of the
 * function when __free() is used.
 *
 * Lastly, given that the benefit of cleanup helpers is removal of
 * "goto", and that the "goto" statement can jump between scopes, the
 * expectation is that usage of "goto" and cleanup helpers is never
 * mixed in the same function. I.e. for a given routine, convert all
 * resources that need a "goto" cleanup to scope-based cleanup, or
 * convert none of them.
 */

/*
 * DEFINE_FREE(name, type, free):
 *	simple helper macro that defines the required wrapper for a __free()
 *	based cleanup function. @free is an expression using '_T' to access the
 *	variable. @free should typically include a NULL test before calling a
 *	function, see the example below.
 *
 * __free(name):
 *	variable attribute to add a scoped based cleanup to the variable.
 *
 * no_free_ptr(var):
 *	like a non-atomic xchg(var, NULL), such that the cleanup function will
 *	be inhibited -- provided it sanely deals with a NULL value.
 *
 *	NOTE: this has __must_check semantics so that it is harder to accidentally
 *	leak the resource.
 *
 * return_ptr(p):
 *	returns p while inhibiting the __free().
 *
 * Ex.
 *
 * DEFINE_FREE(kfree, void *, if (_T) kfree(_T))
 *
 * void *alloc_obj(...)
 * {
 *	struct obj *p __free(kfree) = kmalloc(...);
 *	if (!p)
 *		return NULL;
 *
 *	if (!init_obj(p))
 *		return NULL;
 *
 *	return_ptr(p);
 * }
 *
 * NOTE: the DEFINE_FREE()'s @free expression includes a NULL test even though
 * kfree() is fine to be called with a NULL value. This is on purpose. This way
 * the compiler sees the end of our alloc_obj() function as:
 *
 *	tmp = p;
 *	p = NULL;
 *	if (p)
 *		kfree(p);
 *	return tmp;
 *
 * And through the magic of value-propagation and dead-code-elimination, it
 * eliminates the actual cleanup call and compiles into:
 *
 *	return p;
 *
 * Without the NULL test it turns into a mess and the compiler can't help us.
 */

#define DEFINE_FREE(_name, _type, _free) \
	static __always_inline void __free_##_name(void *p) { _type _T = *(_type *)p; _free; }

#define __free(_name)	__cleanup(__free_##_name)

#define __get_and_null(p, nullvalue)	    \
	({                                  \
		auto __ptr = &(p);	    \
		auto __val = *__ptr;	    \
		*__ptr = nullvalue;         \
		__val;                      \
	})

static __always_inline __must_check
const volatile void * __must_check_fn(const volatile void *val)
{ return val; }

#define no_free_ptr(p) \
	((typeof(p)) __must_check_fn((__force const volatile void *)__get_and_null(p, NULL)))

#define return_ptr(p)	return no_free_ptr(p)

/*
 * Only for situations where an allocation is handed in to another function
 * and consumed by that function on success.
 *
 *	struct foo *f __free(kfree) = kzalloc(sizeof(*f), GFP_KERNEL);
 *
 *	setup(f);
 *	if (some_condition)
 *		return -EINVAL;
 *	....
 *	ret = bar(f);
 *	if (!ret)
 *		retain_and_null_ptr(f);
 *	return ret;
 *
 * After retain_and_null_ptr(f) the variable f is NULL and cannot be
 * dereferenced anymore.
 */
#define retain_and_null_ptr(p)		((void)__get_and_null(p, NULL))

/*
 * DEFINE_CLASS(name, type, exit, init, init_args...):
 *	helper to define the destructor and constructor for a type.
 *	@exit is an expression using '_T' -- similar to FREE above.
 *	@init is an expression in @init_args resulting in @type
 *
 * EXTEND_CLASS(name, ext, init, init_args...):
 *	extends class @name to @name@ext with the new constructor
 *
 * CLASS(name, var)(args...):
 *	declare the variable @var as an instance of the named class
 *
 * CLASS_INIT(name, var, init_expr):
 *	declare the variable @var as an instance of the named class with
 *	custom initialization expression.
 *
 * Ex.
 *
 * DEFINE_CLASS(fdget, struct fd, fdput(_T), fdget(fd), int fd)
 *
 *	CLASS(fdget, f)(fd);
 *	if (fd_empty(f))
 *		return -EBADF;
 *
 *	// use 'f' without concern
 */

#define DEFINE_CLASS(_name, _type, _exit, _init, _init_args...)		\
typedef _type class_##_name##_t;					\
typedef _type lock_##_name##_t;						\
static __always_inline void class_##_name##_destructor(_type *p)	\
	__no_context_analysis						\
{ _type _T = *p; _exit; }						\
static __always_inline _type class_##_name##_constructor(_init_args)	\
	__no_context_analysis						\
{ _type t = _init; return t; }

#define EXTEND_CLASS_COND(_name, ext, _cond, _init, _init_args...)	\
typedef lock_##_name##_t lock_##_name##ext##_t;				\
typedef class_##_name##_t class_##_name##ext##_t;			\
static __always_inline void class_##_name##ext##_destructor(class_##_name##_t *_T) \
{ if (_cond) return; class_##_name##_destructor(_T); }			\
static __always_inline class_##_name##_t class_##_name##ext##_constructor(_init_args) \
	__no_context_analysis \
{ class_##_name##_t t = _init; return t; }

#define EXTEND_CLASS(_name, ext, _init, _init_args...)			\
	EXTEND_CLASS_COND(_name, ext, 0, _init, _init_args)

#define CLASS(_name, var)						\
	class_##_name##_t var __cleanup(class_##_name##_destructor) =	\
		class_##_name##_constructor

#define CLASS_INIT(_name, _var, _init_expr)                             \
        class_##_name##_t _var __cleanup(class_##_name##_destructor) = (_init_expr)

#define __scoped_class(_name, var, _label, args...)        \
	for (CLASS(_name, var)(args); ; ({ goto _label; })) \
		if (0) {                                   \
_label:                                                    \
			break;                             \
		} else

#define scoped_class(_name, var, args...) \
	__scoped_class(_name, var, __UNIQUE_ID(label), args)

/*
 * DEFINE_GUARD(name, type, lock, unlock):
 *	trivial wrapper around DEFINE_CLASS() above specifically
 *	for locks.
 *
 * DEFINE_GUARD_COND(name, ext, condlock)
 *	wrapper around EXTEND_CLASS above to add conditional lock
 *	variants to a base class, eg. mutex_trylock() or
 *	mutex_lock_interruptible().
 *
 * guard(name):
 *	an anonymous instance of the (guard) class, not recommended for
 *	conditional locks.
 *
 * scoped_guard (name, args...) { }:
 *	similar to CLASS(name, scope)(args), except the variable (with the
 *	explicit name 'scope') is declard in a for-loop such that its scope is
 *	bound to the next (compound) statement.
 *
 *	for conditional locks the loop body is skipped when the lock is not
 *	acquired.
 *
 * scoped_cond_guard (name, fail, args...) { }:
 *      similar to scoped_guard(), except it does fail when the lock
 *      acquire fails.
 *
 *      Only for conditional locks.
 *
 * ACQUIRE(name, var):
 *	a named instance of the (guard) class, suitable for conditional
 *	locks when paired with ACQUIRE_ERR().
 *
 * ACQUIRE_ERR(name, &var):
 *	a helper that is effectively a PTR_ERR() conversion of the guard
 *	pointer. Returns 0 when the lock was acquired and a negative
 *	error code otherwise.
 */

#define __DEFINE_CLASS_IS_CONDITIONAL(_name, _is_cond)	\
static __maybe_unused const bool class_##_name##_is_conditional = _is_cond

#define DEFINE_CLASS_IS_UNCONDITIONAL(_name)		\
	__DEFINE_CLASS_IS_CONDITIONAL(_name, false);	\
	static inline void * class_##_name##_lock_ptr(class_##_name##_t *_T) \
	{ return (void *)1; }

#define __GUARD_IS_ERR(_ptr)                                       \
	({                                                         \
		unsigned long _rc = (__force unsigned long)(_ptr); \
		unlikely((_rc - 1) >= -MAX_ERRNO - 1);             \
	})

#define __DEFINE_GUARD_LOCK_PTR(_name, _exp)                                \
	static __always_inline void *class_##_name##_lock_ptr(class_##_name##_t *_T) \
	{                                                                   \
		void *_ptr = (void *)(__force unsigned long)*(_exp);        \
		if (IS_ERR(_ptr)) {                                         \
			_ptr = NULL;                                        \
		}                                                           \
		return _ptr;                                                \
	}                                                                   \
	static __always_inline int class_##_name##_lock_err(class_##_name##_t *_T) \
	{                                                                   \
		long _rc = (__force unsigned long)*(_exp);                  \
		if (!_rc) {                                                 \
			_rc = -EBUSY;                                       \
		}                                                           \
		if (!IS_ERR_VALUE(_rc)) {                                   \
			_rc = 0;                                            \
		}                                                           \
		return _rc;                                                 \
	}

#define DEFINE_CLASS_IS_GUARD(_name) \
	__DEFINE_CLASS_IS_CONDITIONAL(_name, false); \
	__DEFINE_GUARD_LOCK_PTR(_name, _T)

#define DEFINE_CLASS_IS_COND_GUARD(_name) \
	__DEFINE_CLASS_IS_CONDITIONAL(_name, true); \
	__DEFINE_GUARD_LOCK_PTR(_name, _T)

#define DEFINE_GUARD(_name, _type, _lock, _unlock) \
	static __always_inline __nonnull_args(1) _type class_##_name##_constructor(_type _T); \
	DEFINE_CLASS(_name, _type, _unlock, ({ _lock; _T; }), _type _T); \
	DEFINE_CLASS_IS_GUARD(_name)

#define DEFINE_GUARD_COND_4(_name, _ext, _lock, _cond) \
	__DEFINE_CLASS_IS_CONDITIONAL(_name##_ext, true); \
	EXTEND_CLASS_COND(_name, _ext, __GUARD_IS_ERR(*_T), \
		     ({ void *_t = _T; int _RET = (_lock); if (_T && !(_cond)) _t = ERR_PTR(_RET); _t; }), \
		     class_##_name##_t _T) \
	static __always_inline void * class_##_name##_ext##_lock_ptr(class_##_name##_t *_T) \
	{ return class_##_name##_lock_ptr(_T); } \
	static __always_inline int class_##_name##_ext##_lock_err(class_##_name##_t *_T) \
	{ return class_##_name##_lock_err(_T); }

/*
 * Default binary condition; success on 'true'.
 */
#define DEFINE_GUARD_COND_3(_name, _ext, _lock) \
	DEFINE_GUARD_COND_4(_name, _ext, _lock, _RET)

#define DEFINE_GUARD_COND(X...) CONCATENATE(DEFINE_GUARD_COND_, COUNT_ARGS(X))(X)

#define guard(_name) \
	CLASS(_name, __UNIQUE_ID(guard))

#define __guard_ptr(_name) class_##_name##_lock_ptr
#define __guard_err(_name) class_##_name##_lock_err
#define __is_cond_ptr(_name) class_##_name##_is_conditional

#define ACQUIRE(_name, _var)     CLASS(_name, _var)
#define ACQUIRE_ERR(_name, _var) __guard_err(_name)(_var)

/*
 * Helper macro for scoped_guard().
 *
 * Note that the "!__is_cond_ptr(_name)" part of the condition ensures that
 * compiler would be sure that for the unconditional locks the body of the
 * loop (caller-provided code glued to the else clause) could not be skipped.
 * It is needed because the other part - "__guard_ptr(_name)(&scope)" - is too
 * hard to deduce (even if could be proven true for unconditional locks).
 */
#define __scoped_guard(_name, _label, args...)				\
	for (CLASS(_name, scope)(args);					\
	     __guard_ptr(_name)(&scope) || !__is_cond_ptr(_name);	\
	     ({ goto _label; }))					\
		if (0) {						\
_label:									\
			break;						\
		} else

#define scoped_guard(_name, args...)	\
	__scoped_guard(_name, __UNIQUE_ID(label), args)

#define __scoped_cond_guard(_name, _fail, _label, args...)		\
	for (CLASS(_name, scope)(args); true; ({ goto _label; }))	\
		if (!__guard_ptr(_name)(&scope)) {			\
			BUILD_BUG_ON(!__is_cond_ptr(_name));		\
			_fail;						\
_label:									\
			break;						\
		} else

#define scoped_cond_guard(_name, _fail, args...)	\
	__scoped_cond_guard(_name, _fail, __UNIQUE_ID(label), args)

/*
 * Additional helper macros for generating lock guards with types, either for
 * locks that don't have a native type (eg. RCU, preempt) or those that need a
 * 'fat' pointer (eg. spin_lock_irqsave).
 *
 * DEFINE_LOCK_GUARD_0(name, lock, unlock, ...)
 * DEFINE_LOCK_GUARD_1(name, type, lock, unlock, ...)
 * DEFINE_LOCK_GUARD_1_COND(name, ext, condlock)
 *
 * will result in the following type:
 *
 *   typedef struct {
 *	type *lock;		// 'type := void' for the _0 variant
 *	__VA_ARGS__;
 *   } class_##name##_t;
 *
 * As above, both _lock and _unlock are statements, except this time '_T' will
 * be a pointer to the above struct.
 */

#define __DEFINE_UNLOCK_GUARD(_name, _type, _unlock, ...)		\
typedef _type lock_##_name##_t;						\
typedef struct {							\
	_type *lock;							\
	__VA_ARGS__;							\
} class_##_name##_t;							\
									\
static __always_inline void class_##_name##_destructor(class_##_name##_t *_T) \
	__no_context_analysis						\
{									\
	_unlock;							\
}									\
									\
__DEFINE_GUARD_LOCK_PTR(_name, &_T->lock)

#define __DEFINE_LOCK_GUARD_1(_name, _type, ...)			\
static __always_inline __nonnull_args(1)				\
class_##_name##_t class_##_name##_constructor(_type *l)			\
	__no_context_analysis						\
{									\
	class_##_name##_t _t = { .lock = l }, *_T = &_t;		\
	__VA_ARGS__;							\
	return _t;							\
}

#define __DEFINE_LOCK_GUARD_0(_name, ...)				\
static __always_inline class_##_name##_t class_##_name##_constructor(void) \
	__no_context_analysis						\
{									\
	class_##_name##_t _t = { .lock = (void*)1 },			\
			 *_T __maybe_unused = &_t;			\
	__VA_ARGS__;							\
	return _t;							\
}

#define DECLARE_LOCK_GUARD_0_ATTRS(_name, _lock, _unlock)		\
static inline class_##_name##_t class_##_name##_constructor(void) _lock;\
static inline void class_##_name##_destructor(class_##_name##_t *_T) _unlock;

/*
 * To support Context Analysis, we need to allow the compiler to see the
 * acquisition and release of the context lock. However, the "cleanup" helpers
 * wrap the lock in a struct passed through separate helper functions, which
 * hides the lock alias from the compiler (no inter-procedural analysis).
 *
 * To make it work, we introduce an explicit alias to the context lock instance
 * that is "cleaned" up with a separate cleanup helper. This helper is a dummy
 * function that does nothing at runtime, but has the "_unlock" attribute to
 * tell the compiler what happens at the end of the scope.
 *
 * To generalize the pattern, the WITH_LOCK_GUARD_1_ATTRS() macro should be used
 * to redefine the constructor, which then also creates the alias variable with
 * the right "cleanup" attribute, *after* DECLARE_LOCK_GUARD_1_ATTRS() has been
 * used.
 *
 * Example usage:
 *
 *   DECLARE_LOCK_GUARD_1_ATTRS(mutex, __acquires(_T), __releases(*(struct mutex **)_T))
 *   #define class_mutex_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(mutex, _T)
 *
 * Note: To support the for-loop based scoped helpers, the auxiliary variable
 * must be a pointer to the "class" type because it is defined in the same
 * statement as the guard variable. However, we initialize it with the lock
 * pointer (despite the type mismatch, the compiler's alias analysis still works
 * as expected). The "_unlock" attribute receives a pointer to the auxiliary
 * variable (a double pointer to the class type), and must be cast and
 * dereferenced appropriately.
 */
#define DECLARE_LOCK_GUARD_1_ATTRS(_name, _lock, _unlock)		\
static inline class_##_name##_t class_##_name##_constructor(lock_##_name##_t *_T) _lock;\
static __always_inline void __class_##_name##_cleanup_ctx(class_##_name##_t **_T) \
	__no_context_analysis _unlock { }
#define WITH_LOCK_GUARD_1_ATTRS(_name, _T)				\
	class_##_name##_constructor(_T),				\
	*__UNIQUE_ID(unlock) __cleanup(__class_##_name##_cleanup_ctx) = (void *)(unsigned long)(_T)

#define DEFINE_LOCK_GUARD_1(_name, _type, _lock, _unlock, ...)		\
__DEFINE_CLASS_IS_CONDITIONAL(_name, false);				\
__DEFINE_UNLOCK_GUARD(_name, _type, _unlock, __VA_ARGS__)		\
__DEFINE_LOCK_GUARD_1(_name, _type, _lock)

#define DEFINE_LOCK_GUARD_0(_name, _lock, _unlock, ...)			\
__DEFINE_CLASS_IS_CONDITIONAL(_name, false);				\
__DEFINE_UNLOCK_GUARD(_name, void, _unlock, __VA_ARGS__)		\
__DEFINE_LOCK_GUARD_0(_name, _lock)

#define DEFINE_LOCK_GUARD_1_COND_4(_name, _ext, _lock, _cond)		\
	__DEFINE_CLASS_IS_CONDITIONAL(_name##_ext, true);		\
	EXTEND_CLASS_COND(_name, _ext, __GUARD_IS_ERR(_T->lock),	\
		     ({ class_##_name##_t _t = { .lock = l }, *_T = &_t;\
		        int _RET = (_lock);                             \
		        if (_T->lock && !(_cond)) _T->lock = ERR_PTR(_RET);\
			_t; }),						\
		     typeof_member(class_##_name##_t, lock) l)		\
	static __always_inline void * class_##_name##_ext##_lock_ptr(class_##_name##_t *_T) \
	{ return class_##_name##_lock_ptr(_T); } \
	static __always_inline int class_##_name##_ext##_lock_err(class_##_name##_t *_T) \
	{ return class_##_name##_lock_err(_T); }

#define DEFINE_LOCK_GUARD_1_COND_3(_name, _ext, _lock) \
	DEFINE_LOCK_GUARD_1_COND_4(_name, _ext, _lock, _RET)

#define DEFINE_LOCK_GUARD_1_COND(X...) CONCATENATE(DEFINE_LOCK_GUARD_1_COND_, COUNT_ARGS(X))(X)

/*
 * Before 6.5: the lock headers define no guards. These are those of
 * <linux/mutex.h> and <linux/spinlock.h> of 7.3.
 */
#include <linux/mutex.h>
#include <linux/spinlock.h>

DEFINE_LOCK_GUARD_1(mutex, struct mutex, mutex_lock(_T->lock), mutex_unlock(_T->lock))
DEFINE_LOCK_GUARD_1_COND(mutex, _try, mutex_trylock(_T->lock))
DEFINE_LOCK_GUARD_1_COND(mutex, _intr, mutex_lock_interruptible(_T->lock), _RET == 0)
DEFINE_LOCK_GUARD_1_COND(mutex, _kill, mutex_lock_killable(_T->lock), _RET == 0)
DEFINE_LOCK_GUARD_1(mutex_init, struct mutex, mutex_init(_T->lock), /* */)

DECLARE_LOCK_GUARD_1_ATTRS(mutex,	__acquires(_T), __releases(*(struct mutex **)_T))
#define class_mutex_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(mutex, _T)
DECLARE_LOCK_GUARD_1_ATTRS(mutex_try,	__acquires(_T), __releases(*(struct mutex **)_T))
#define class_mutex_try_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(mutex_try, _T)
DECLARE_LOCK_GUARD_1_ATTRS(mutex_intr,	__acquires(_T), __releases(*(struct mutex **)_T))
#define class_mutex_intr_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(mutex_intr, _T)
DECLARE_LOCK_GUARD_1_ATTRS(mutex_kill,	__acquires(_T), __releases(*(struct mutex **)_T))
#define class_mutex_kill_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(mutex_kill, _T)
DECLARE_LOCK_GUARD_1_ATTRS(mutex_init,	__acquires(_T), __releases(*(struct mutex **)_T))
#define class_mutex_init_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(mutex_init, _T)

DEFINE_LOCK_GUARD_1(raw_spinlock, raw_spinlock_t,
		    raw_spin_lock(_T->lock),
		    raw_spin_unlock(_T->lock))
DECLARE_LOCK_GUARD_1_ATTRS(raw_spinlock, __acquires(_T), __releases(*(raw_spinlock_t **)_T))
#define class_raw_spinlock_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(raw_spinlock, _T)

DEFINE_LOCK_GUARD_1_COND(raw_spinlock, _try, raw_spin_trylock(_T->lock))
DECLARE_LOCK_GUARD_1_ATTRS(raw_spinlock_try, __acquires(_T), __releases(*(raw_spinlock_t **)_T))
#define class_raw_spinlock_try_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(raw_spinlock_try, _T)

DEFINE_LOCK_GUARD_1(raw_spinlock_nested, raw_spinlock_t,
		    raw_spin_lock_nested(_T->lock, SINGLE_DEPTH_NESTING),
		    raw_spin_unlock(_T->lock))
DECLARE_LOCK_GUARD_1_ATTRS(raw_spinlock_nested, __acquires(_T), __releases(*(raw_spinlock_t **)_T))
#define class_raw_spinlock_nested_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(raw_spinlock_nested, _T)

DEFINE_LOCK_GUARD_1(raw_spinlock_irq, raw_spinlock_t,
		    raw_spin_lock_irq(_T->lock),
		    raw_spin_unlock_irq(_T->lock))
DECLARE_LOCK_GUARD_1_ATTRS(raw_spinlock_irq, __acquires(_T), __releases(*(raw_spinlock_t **)_T))
#define class_raw_spinlock_irq_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(raw_spinlock_irq, _T)

DEFINE_LOCK_GUARD_1_COND(raw_spinlock_irq, _try, raw_spin_trylock_irq(_T->lock))
DECLARE_LOCK_GUARD_1_ATTRS(raw_spinlock_irq_try, __acquires(_T), __releases(*(raw_spinlock_t **)_T))
#define class_raw_spinlock_irq_try_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(raw_spinlock_irq_try, _T)

DEFINE_LOCK_GUARD_1(raw_spinlock_bh, raw_spinlock_t,
		    raw_spin_lock_bh(_T->lock),
		    raw_spin_unlock_bh(_T->lock))
DECLARE_LOCK_GUARD_1_ATTRS(raw_spinlock_bh, __acquires(_T), __releases(*(raw_spinlock_t **)_T))
#define class_raw_spinlock_bh_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(raw_spinlock_bh, _T)

DEFINE_LOCK_GUARD_1_COND(raw_spinlock_bh, _try, raw_spin_trylock_bh(_T->lock))
DECLARE_LOCK_GUARD_1_ATTRS(raw_spinlock_bh_try, __acquires(_T), __releases(*(raw_spinlock_t **)_T))
#define class_raw_spinlock_bh_try_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(raw_spinlock_bh_try, _T)

DEFINE_LOCK_GUARD_1(raw_spinlock_irqsave, raw_spinlock_t,
		    raw_spin_lock_irqsave(_T->lock, _T->flags),
		    raw_spin_unlock_irqrestore(_T->lock, _T->flags),
		    unsigned long flags)
DECLARE_LOCK_GUARD_1_ATTRS(raw_spinlock_irqsave, __acquires(_T), __releases(*(raw_spinlock_t **)_T))
#define class_raw_spinlock_irqsave_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(raw_spinlock_irqsave, _T)

DEFINE_LOCK_GUARD_1_COND(raw_spinlock_irqsave, _try,
			 raw_spin_trylock_irqsave(_T->lock, _T->flags))
DECLARE_LOCK_GUARD_1_ATTRS(raw_spinlock_irqsave_try, __acquires(_T), __releases(*(raw_spinlock_t **)_T))
#define class_raw_spinlock_irqsave_try_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(raw_spinlock_irqsave_try, _T)

DEFINE_LOCK_GUARD_1(raw_spinlock_init, raw_spinlock_t, raw_spin_lock_init(_T->lock), /* */)
DECLARE_LOCK_GUARD_1_ATTRS(raw_spinlock_init, __acquires(_T), __releases(*(raw_spinlock_t **)_T))
#define class_raw_spinlock_init_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(raw_spinlock_init, _T)

DEFINE_LOCK_GUARD_1(spinlock, spinlock_t,
		    spin_lock(_T->lock),
		    spin_unlock(_T->lock))
DECLARE_LOCK_GUARD_1_ATTRS(spinlock, __acquires(_T), __releases(*(spinlock_t **)_T))
#define class_spinlock_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(spinlock, _T)

DEFINE_LOCK_GUARD_1_COND(spinlock, _try, spin_trylock(_T->lock))
DECLARE_LOCK_GUARD_1_ATTRS(spinlock_try, __acquires(_T), __releases(*(spinlock_t **)_T))
#define class_spinlock_try_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(spinlock_try, _T)

DEFINE_LOCK_GUARD_1(spinlock_irq, spinlock_t,
		    spin_lock_irq(_T->lock),
		    spin_unlock_irq(_T->lock))
DECLARE_LOCK_GUARD_1_ATTRS(spinlock_irq, __acquires(_T), __releases(*(spinlock_t **)_T))
#define class_spinlock_irq_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(spinlock_irq, _T)

DEFINE_LOCK_GUARD_1_COND(spinlock_irq, _try,
			 spin_trylock_irq(_T->lock))
DECLARE_LOCK_GUARD_1_ATTRS(spinlock_irq_try, __acquires(_T), __releases(*(spinlock_t **)_T))
#define class_spinlock_irq_try_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(spinlock_irq_try, _T)

DEFINE_LOCK_GUARD_1(spinlock_bh, spinlock_t,
		    spin_lock_bh(_T->lock),
		    spin_unlock_bh(_T->lock))
DECLARE_LOCK_GUARD_1_ATTRS(spinlock_bh, __acquires(_T), __releases(*(spinlock_t **)_T))
#define class_spinlock_bh_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(spinlock_bh, _T)

DEFINE_LOCK_GUARD_1_COND(spinlock_bh, _try,
			 spin_trylock_bh(_T->lock))
DECLARE_LOCK_GUARD_1_ATTRS(spinlock_bh_try, __acquires(_T), __releases(*(spinlock_t **)_T))
#define class_spinlock_bh_try_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(spinlock_bh_try, _T)

DEFINE_LOCK_GUARD_1(spinlock_irqsave, spinlock_t,
		    spin_lock_irqsave(_T->lock, _T->flags),
		    spin_unlock_irqrestore(_T->lock, _T->flags),
		    unsigned long flags)
DECLARE_LOCK_GUARD_1_ATTRS(spinlock_irqsave, __acquires(_T), __releases(*(spinlock_t **)_T))
#define class_spinlock_irqsave_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(spinlock_irqsave, _T)

DEFINE_LOCK_GUARD_1_COND(spinlock_irqsave, _try,
			 spin_trylock_irqsave(_T->lock, _T->flags))
DECLARE_LOCK_GUARD_1_ATTRS(spinlock_irqsave_try, __acquires(_T), __releases(*(spinlock_t **)_T))
#define class_spinlock_irqsave_try_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(spinlock_irqsave_try, _T)

DEFINE_LOCK_GUARD_1(spinlock_init, spinlock_t, spin_lock_init(_T->lock), /* */)
DECLARE_LOCK_GUARD_1_ATTRS(spinlock_init, __acquires(_T), __releases(*(spinlock_t **)_T))
#define class_spinlock_init_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(spinlock_init, _T)

DEFINE_LOCK_GUARD_1(read_lock, rwlock_t,
		    read_lock(_T->lock),
		    read_unlock(_T->lock))
DECLARE_LOCK_GUARD_1_ATTRS(read_lock, __acquires(_T), __releases(*(rwlock_t **)_T))
#define class_read_lock_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(read_lock, _T)

DEFINE_LOCK_GUARD_1(read_lock_irq, rwlock_t,
		    read_lock_irq(_T->lock),
		    read_unlock_irq(_T->lock))
DECLARE_LOCK_GUARD_1_ATTRS(read_lock_irq, __acquires(_T), __releases(*(rwlock_t **)_T))
#define class_read_lock_irq_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(read_lock_irq, _T)

DEFINE_LOCK_GUARD_1(read_lock_irqsave, rwlock_t,
		    read_lock_irqsave(_T->lock, _T->flags),
		    read_unlock_irqrestore(_T->lock, _T->flags),
		    unsigned long flags)
DECLARE_LOCK_GUARD_1_ATTRS(read_lock_irqsave, __acquires(_T), __releases(*(rwlock_t **)_T))
#define class_read_lock_irqsave_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(read_lock_irqsave, _T)

DEFINE_LOCK_GUARD_1(write_lock, rwlock_t,
		    write_lock(_T->lock),
		    write_unlock(_T->lock))
DECLARE_LOCK_GUARD_1_ATTRS(write_lock, __acquires(_T), __releases(*(rwlock_t **)_T))
#define class_write_lock_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(write_lock, _T)

DEFINE_LOCK_GUARD_1(write_lock_irq, rwlock_t,
		    write_lock_irq(_T->lock),
		    write_unlock_irq(_T->lock))
DECLARE_LOCK_GUARD_1_ATTRS(write_lock_irq, __acquires(_T), __releases(*(rwlock_t **)_T))
#define class_write_lock_irq_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(write_lock_irq, _T)

DEFINE_LOCK_GUARD_1(write_lock_irqsave, rwlock_t,
		    write_lock_irqsave(_T->lock, _T->flags),
		    write_unlock_irqrestore(_T->lock, _T->flags),
		    unsigned long flags)
DECLARE_LOCK_GUARD_1_ATTRS(write_lock_irqsave, __acquires(_T), __releases(*(rwlock_t **)_T))
#define class_write_lock_irqsave_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(write_lock_irqsave, _T)

DEFINE_LOCK_GUARD_1(rwlock_init, rwlock_t, rwlock_init(_T->lock), /* */)
DECLARE_LOCK_GUARD_1_ATTRS(rwlock_init, __acquires(_T), __releases(*(rwlock_t **)_T))
#define class_rwlock_init_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(rwlock_init, _T)

/* Before 6.5: the rwsem guards of 7.3's rwsem.h, which this port uses. */
DEFINE_LOCK_GUARD_1(rwsem_read, struct rw_semaphore,
		    down_read(_T->lock), up_read(_T->lock))
DEFINE_LOCK_GUARD_1_COND(rwsem_read, _try, down_read_trylock(_T->lock))

DEFINE_LOCK_GUARD_1(rwsem_write, struct rw_semaphore,
		    down_write(_T->lock), up_write(_T->lock))
DEFINE_LOCK_GUARD_1_COND(rwsem_write, _try, down_write_trylock(_T->lock))

#endif /* _LINUX_CLEANUP_H */

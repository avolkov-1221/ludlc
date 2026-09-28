// SPDX-License-Identifier: GPL-2.0-only
/**
 * @file ludlc_csum.c
 *
 * @brief The checksums a LuDLC link can run, and the registry other modules
 * add to.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * This lives with the core rather than with a transport because both ends of a
 * link must agree on the checksum: it describes the network, not the wire, and
 * every transport picks one the same way. See ludlc_csum.h.
 */

#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/sysfs.h>
#include <linux/crc-ccitt.h>
#include <linux/crc16.h>
#include <linux/crc32.h>

#include "ludlc_csum.h"

#if IS_ENABLED(CONFIG_CRC_CCITT)
/*
 * The kernel crc_ccitt() is the reflected 0x1021 table, which seeded with zero
 * is the CRC-16/KERMIT the other platforms send. One octet at a time, because
 * that is how the serial encoder feeds the frame it escapes as it goes.
 */
static ludlc_csum_t ludlc_crc16_ccitt_byte(ludlc_csum_t csum, u8 byte)
{
	return crc_ccitt_byte((u16)csum, byte);
}
#endif

#if IS_ENABLED(CONFIG_CRC16)
/* CRC-16/ARC: reflected 0x8005, the other 16-bit CRC the kernel provides. */
static ludlc_csum_t ludlc_crc16_byte(ludlc_csum_t csum, u8 byte)
{
	return crc16_byte((u16)csum, byte);
}
#endif

#if IS_ENABLED(CONFIG_CRC32)
/* CRC-32/ISO-HDLC, for a link that needs more than 16 bits. */
static ludlc_csum_t ludlc_crc32_byte(ludlc_csum_t csum, u8 byte)
{
	return crc32_le((u32)csum, &byte, 1);
}
#endif

#if IS_ENABLED(CONFIG_CRC_CCITT)
/*
 * HDLC FCS-16, as X.25 and PPP send it: the same reflected 0x1021 as above,
 * but seeded with all ones and complemented on output, which leaves the
 * decoder with 0xf0b8 instead of zero. The encoder does the complementing,
 * hence a transform rather than part of the CRC. Byte order does not matter:
 * the framing always sends the low octet first.
 */
static ludlc_csum_t ludlc_x25_to_wire(ludlc_csum_t csum)
{
	return (u16)~csum;
}
#endif

/*
 * The checksums a link can use, one choice per link. Which ones a build has
 * depends on the kernel CRC options, since each is computed with a library of
 * its own: no transport needs any of them, and none constrains the width
 * beyond a whole number of octets, up to eight.
 *
 * Every residual below was computed and checked, since a descriptor declaring
 * the wrong one is unusable. Adding an entry means doing the same and stating
 * its width, which decides how many octets are sent.
 */
#if IS_ENABLED(CONFIG_CRC_CCITT)
const struct ludlc_csum ludlc_crc16_ccitt_pr0 = {
	.name = "crc16-ccitt-pr0",
	.octets = BITS_TO_BYTES(16),
	.csum = ludlc_crc16_ccitt_byte,
};
EXPORT_SYMBOL_GPL(ludlc_crc16_ccitt_pr0);

const struct ludlc_csum ludlc_crc16_x25_pr1 = {
	.name = "crc16-x25-pr1",
	.octets = BITS_TO_BYTES(16),
	.csum = ludlc_crc16_ccitt_byte,
	.init = 0xffff,
	.verify = 0xf0b8,
	.to_wire = ludlc_x25_to_wire,
};
EXPORT_SYMBOL_GPL(ludlc_crc16_x25_pr1);
#endif

#if IS_ENABLED(CONFIG_CRC16)
const struct ludlc_csum ludlc_crc16_pr0 = {
	.name = "crc16-pr0",
	.octets = BITS_TO_BYTES(16),
	.csum = ludlc_crc16_byte,
};
EXPORT_SYMBOL_GPL(ludlc_crc16_pr0);
#endif

#if IS_ENABLED(CONFIG_CRC32)
const struct ludlc_csum ludlc_crc32_ccitt_pr0 = {
	.name = "crc32-ccitt-pr0",
	.octets = BITS_TO_BYTES(32),
	.csum = ludlc_crc32_byte,
};
EXPORT_SYMBOL_GPL(ludlc_crc32_ccitt_pr0);
#endif

/*
 * The same descriptors, in the order the text interfaces search them.
 *
 * Every name states its seed, after the HDLC line parameters in
 * <uapi/linux/hdlc/ioctl.h> where PR0 and PR1 distinguish 0x0000 from 0xffff.
 * "CCITT" alone is ambiguous in this kernel: crc_ccitt() is reflected and
 * seeded with zero, which is CRC-16/KERMIT and the one used here, while
 * crc_itu_t() is seeded with 0xffff and is what the Zodiac supervisor driver
 * calls ccitt. Firmware naming the wrong one gets a link that only receives
 * garbage.
 */
static const struct ludlc_csum * const ludlc_crcs[] = {
#if IS_ENABLED(CONFIG_CRC_CCITT)
	&ludlc_crc16_ccitt_pr0,
#endif
#if IS_ENABLED(CONFIG_CRC16)
	&ludlc_crc16_pr0,
#endif
#if IS_ENABLED(CONFIG_CRC_CCITT)
	&ludlc_crc16_x25_pr1,
#endif
#if IS_ENABLED(CONFIG_CRC32)
	&ludlc_crc32_ccitt_pr0,
#endif
};

/**
 * @struct ludlc_crc_node
 * @brief A checksum registered by another module.
 *
 * An entry pins its provider while a link uses it, so that module's code stays
 * loaded. The node belongs to the core and outlives the registration for as
 * long as a reference is held.
 */
struct ludlc_crc_node {
	struct list_head list;
	const struct ludlc_csum *crc;
	struct module *owner;
	refcount_t refs;
	bool registered;
};

static LIST_HEAD(ludlc_crc_nodes);
static DEFINE_MUTEX(ludlc_csum_lock);

/*
 * One checksum for the whole network is the normal case, and this overrides
 * the one the build chose, for every link that does not name its own; "none"
 * leaves those links without one. An unknown name fails the module load
 * instead of silently using the wrong checksum on every link. Only a built-in
 * can be named here: anything registered comes from a module that must have
 * loaded after this one.
 */
static char *ludlc_crc_param;
module_param_named(crc, ludlc_crc_param, charp, 0444);
MODULE_PARM_DESC(crc,
	"checksum of a link firmware and configfs leave unset, or \"none\"");

static const struct ludlc_csum *ludlc_crc_dflt;

/**
 * @brief The registry entry for @p crc, if it has one.
 *
 * NULL for a built-in or a caller's own descriptor, since neither has a
 * provider to account for.
 */
static struct ludlc_crc_node *
ludlc_csum_node_of(const struct ludlc_csum *crc)
{
	struct ludlc_crc_node *node;

	lockdep_assert_held(&ludlc_csum_lock);

	list_for_each_entry(node, &ludlc_crc_nodes, list)
		if (node->crc == crc)
			return node;

	return NULL;
}

/** @brief Look up @p name, built in or registered. Takes no reference. */
static const struct ludlc_csum *ludlc_lookup_csum(const char *name)
{
	struct ludlc_crc_node *node;
	unsigned int i;

	lockdep_assert_held(&ludlc_csum_lock);

	for (i = 0; i < ARRAY_SIZE(ludlc_crcs); i++)
		if (sysfs_streq(name, ludlc_crcs[i]->name))
			return ludlc_crcs[i];

	list_for_each_entry(node, &ludlc_crc_nodes, list)
		if (node->registered && sysfs_streq(name, node->crc->name))
			return node->crc;

	return NULL;
}

/**
 * @brief Keep @p crc from disappearing while it is in use.
 *
 * The built-in descriptors and the caller's own have no provider to reference;
 * a registered one pins the module its code belongs to.
 *
 * @return false only if the provider is already being unloaded.
 */
static bool ludlc_csum_hold(const struct ludlc_csum *csum)
{
	struct ludlc_crc_node *node = ludlc_csum_node_of(csum);

	if (!node)
		return true;

	if (!try_module_get(node->owner))
		return false;

	refcount_inc(&node->refs);

	return true;
}

/** @brief Take a reference to @p crc for a caller about to use it. */
const struct ludlc_csum *
ludlc_csum_get(const struct ludlc_csum *csum)
{
	if (!csum)
		return NULL;

	guard(mutex)(&ludlc_csum_lock);

	return ludlc_csum_hold(csum) ? csum : NULL;
}
EXPORT_SYMBOL_GPL(ludlc_csum_get);

/** @brief Drop a reference to @p csum, freeing an unreferenced entry. */
void ludlc_csum_put(const struct ludlc_csum *csum)
{
	struct ludlc_crc_node *node;
	struct module *owner;

	if (!csum)
		return;

	scoped_guard(mutex, &ludlc_csum_lock) {
		node = ludlc_csum_node_of(csum);
		if (!node)
			return;

		owner = node->owner;
		if (refcount_dec_and_test(&node->refs)) {
			list_del(&node->list);
			kfree(node);
		}
	}

	module_put(owner);
}
EXPORT_SYMBOL_GPL(ludlc_csum_put);

/**
 * @brief Look up the checksum called @p name and take a reference to it.
 *
 * @return NULL if no such checksum is known here. Release it with
 * ludlc_csum_put().
 */
const struct ludlc_csum *ludlc_crc_find(const char *name)
{
	const struct ludlc_csum *csum;

	if (!name)
		return NULL;

	guard(mutex)(&ludlc_csum_lock);

	csum = ludlc_lookup_csum(name);
	if (!csum)
		return NULL;

	return ludlc_csum_hold(csum) ? csum : NULL;
}
EXPORT_SYMBOL_GPL(ludlc_crc_find);

/** @brief Look up @p name, "none" giving no checksum. */
int ludlc_crc_by_name(const char *name, const struct ludlc_csum **csum)
{
	if (name && sysfs_streq(name, LUDLC_CSUM_NONE)) {
		*csum = NULL;
		return 0;
	}

	*csum = ludlc_crc_find(name);

	return *csum ? 0 : -EINVAL;
}
EXPORT_SYMBOL_GPL(ludlc_crc_by_name);

/** @brief The checksum a link runs when nothing chooses one, NULL for none. */
const struct ludlc_csum *ludlc_csum_default(void)
{
	return ludlc_crc_dflt;
}
EXPORT_SYMBOL_GPL(ludlc_csum_default);

/** @brief The names ludlc_crc_by_name() accepts, as text. */
int ludlc_csum_list(char *page)
{
	struct ludlc_crc_node *node;
	unsigned int i;
	int len = sysfs_emit(page, "%s", LUDLC_CSUM_NONE);

	/* All of them: the module would not have loaded with a bad one. */
	for (i = 0; i < ARRAY_SIZE(ludlc_crcs); i++)
		len += sysfs_emit_at(page, len, " %s", ludlc_crcs[i]->name);

	scoped_guard(mutex, &ludlc_csum_lock)
		list_for_each_entry(node, &ludlc_crc_nodes, list)
			if (node->registered)
				len += sysfs_emit_at(page, len, " %s",
						     node->crc->name);

	return len + sysfs_emit_at(page, len, "\n");
}
EXPORT_SYMBOL_GPL(ludlc_csum_list);

/**
 * @brief Does @p csum leave behind the residual it declares?
 *
 * Encode a frame the way the encoder does - checksum transformed for the wire,
 * then sent least significant octet first - and run the CRC over those octets
 * the way the decoder does, which must end on @p verify. Two frames, so that a
 * residual matching one of them by chance is not taken as proof: a residual
 * measured for the other byte order, or a width the CRC does not compute,
 * differs from frame to frame and gives a link on which nothing ever arrives.
 */
static bool ludlc_csum_verifies(const struct ludlc_csum *csum)
{
	static const u8 frames[2][7] = {
		{ 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 },
		{ 0xa5, 0x5a, 0xff, 0x00, 0x81, 0x7e, 0x18 },
	};
	unsigned int f, i;

	for (f = 0; f < ARRAY_SIZE(frames); f++) {
		ludlc_csum_t val = csum->init;
		ludlc_csum_t wire;

		for (i = 0; i < sizeof(frames[f]); i++)
			val = csum->csum(val, frames[f][i]);

		wire = csum->to_wire ? csum->to_wire(val) : val;

		/* As the encoder sends it: low octet first, @octets of them. */
		for (i = 0; i < csum->octets; i++)
			val = csum->csum(val, (u8)(wire >> (8 * i)));

		if (val != csum->verify)
			return false;
	}

	return true;
}

/** @brief Check a descriptor a link is about to use, and report why not. */
int ludlc_csum_check(struct device *dev, const struct ludlc_csum *csum)
{
	if (!csum->csum) {
		dev_err(dev, "checksum %s computes nothing\n", csum->name);
		return -EINVAL;
	}

	/* Checked first, as the residual below runs over that many octets of
	 * the register.
	 */
	if (!csum->octets || csum->octets > LUDLC_MAX_CSUM_SIZE) {
		dev_err(dev, "checksum %s sends %u octets, not 1 to %zu\n",
			csum->name, csum->octets, LUDLC_MAX_CSUM_SIZE);
		return -EOPNOTSUPP;
	}

	if (!ludlc_csum_verifies(csum)) {
		dev_err(dev, "checksum %s does not leave residual %#llx\n",
			csum->name, csum->verify);
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(ludlc_csum_check);

/**
 * @brief Register @p crc under its name, owned by @p owner.
 *
 * @return 0, -EEXIST if the name is taken, -EOPNOTSUPP for a width that cannot
 * be sent, -EINVAL for an inconsistent descriptor, or -ENOMEM.
 */
int __ludlc_crc_register(const struct ludlc_csum *crc,
			 struct module *owner)
{
	struct ludlc_crc_node *node;

	if (!crc || !crc->csum || !crc->name || !*crc->name)
		return -EINVAL;

	if (!crc->octets || crc->octets > LUDLC_MAX_CSUM_SIZE)
		return -EOPNOTSUPP;

	if (!ludlc_csum_verifies(crc))
		return -EINVAL;

	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (!node)
		return -ENOMEM;

	node->crc = crc;
	node->owner = owner;
	node->registered = true;
	refcount_set(&node->refs, 1);

	guard(mutex)(&ludlc_csum_lock);

	/* One name must mean one checksum, or the two ends cannot agree. */
	if (ludlc_lookup_csum(crc->name)) {
		kfree(node);
		return -EEXIST;
	}

	list_add_tail(&node->list, &ludlc_crc_nodes);

	return 0;
}
EXPORT_SYMBOL_GPL(__ludlc_crc_register);

/** @brief Stop offering @p crc to new links; existing ones keep running. */
void ludlc_crc_unregister(const struct ludlc_csum *crc)
{
	struct ludlc_crc_node *node;

	guard(mutex)(&ludlc_csum_lock);

	node = ludlc_csum_node_of(crc);
	if (!node)
		return;

	/*
	 * A link already using this keeps its reference and the node stays
	 * where put() can find it; it only stops being available to new links.
	 */
	node->registered = false;
	if (refcount_dec_and_test(&node->refs)) {
		list_del(&node->list);
		kfree(node);
	}
}
EXPORT_SYMBOL_GPL(ludlc_crc_unregister);

/** @brief Give @p conn the checksum it will run, and hold it there. */
int ludlc_link_csum_set(const struct ludlc_csum **slot, struct device *dev,
			const struct ludlc_csum *csum)
{
	int ret;

	if (!slot || !dev)
		return -EINVAL;

	/* A link that runs no checksum has nothing to hold or check. */
	if (!csum) {
		*slot = NULL;
		return 0;
	}

	/*
	 * The caller only borrows the descriptor; the link takes a reference
	 * for as long as it uses it, which keeps a provider module loaded.
	 */
	if (!ludlc_csum_get(csum)) {
		dev_err(dev, "checksum %s is being unloaded\n", csum->name);
		return -ENODEV;
	}

	/* Recorded as soon as it is held, so that a drop finds it. */
	*slot = csum;

	ret = ludlc_csum_check(dev, csum);
	if (ret)
		ludlc_link_csum_drop(slot);

	return ret;
}
EXPORT_SYMBOL_GPL(ludlc_link_csum_set);

/** @brief Let go of the checksum in @p slot and clear it. */
void ludlc_link_csum_drop(const struct ludlc_csum **slot)
{
	if (!slot)
		return;

	ludlc_csum_put(*slot);
	*slot = NULL;
}
EXPORT_SYMBOL_GPL(ludlc_link_csum_drop);

/** @brief Check the built-in checksums and decide the system default. */
int ludlc_csum_setup(void)
{
	const struct ludlc_csum *crc;
	unsigned int i;

	/*
	 * Built-ins take the same test as a registered checksum. They are
	 * correct by construction, so a failure is a bug here rather than a
	 * misconfiguration: refuse to load rather than trust the table.
	 */
	for (i = 0; i < ARRAY_SIZE(ludlc_crcs); i++) {
		crc = ludlc_crcs[i];

		if (!ludlc_csum_verifies(crc)) {
			pr_err("ludlc: %s does not leave residual %#llx\n",
			       crc->name, crc->verify);
			return -EINVAL;
		}
	}

	if (ludlc_crc_param) {
		if (ludlc_crc_by_name(ludlc_crc_param, &ludlc_crc_dflt)) {
			pr_err("ludlc: no checksum called \"%s\"\n",
			       ludlc_crc_param);
			return -EINVAL;
		}
	} else {
#if IS_ENABLED(CONFIG_LUDLC_CRC_DFLT_X25_PR1)
		ludlc_crc_dflt = &ludlc_crc16_x25_pr1;
#elif IS_ENABLED(CONFIG_LUDLC_CRC_DFLT_CRC16_PR0)
		ludlc_crc_dflt = &ludlc_crc16_pr0;
#elif IS_ENABLED(CONFIG_LUDLC_CRC_DFLT_CRC32_PR0)
		ludlc_crc_dflt = &ludlc_crc32_ccitt_pr0;
#elif IS_ENABLED(CONFIG_LUDLC_CRC_DFLT_NONE)
		ludlc_crc_dflt = NULL;
#elif IS_ENABLED(CONFIG_CRC_CCITT)	/* CCITT_PR0, or no LuDLC Kconfig */
		ludlc_crc_dflt = &ludlc_crc16_ccitt_pr0;
#else
#error "LuDLC: the default checksum needs its kernel CRC library"
#endif
	}

	return 0;
}

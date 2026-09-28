// SPDX-License-Identifier: GPL-2.0-only
/**
 * @file ludlc_cfg.c
 *
 * @brief Configfs description of a LuDLC bus device, for a system where
 * neither DT nor ACPI describes one.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * A transport embeds a @ref ludlc_cfg_devices group - typically a "ludlc"
 * subdirectory of its own port - and each node created below it describes one
 * device: a compatible string to match a driver by, the channels it owns, and
 * a "live" switch that puts it on the transport's bus. The properties become a
 * software node, so such a device looks to a driver like one described by
 * firmware.
 */

#include <linux/bitops.h>
#include <linux/cleanup.h>
#include <linux/configfs.h>
#include <linux/ctype.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <ludlc.h>
#include <ludlc_private.h>

#include "ludlc_bus.h"

/**
 * @brief How many channels one node may name in "reg".
 *
 * Bounds a single write, so that the list can be parsed on the stack. Local
 * policy, not a kernel limit.
 */
#define LUDLC_CFG_REG_MAX	16

/**
 * @struct ludlc_cfg_dev_item
 * @brief One configfs node, describing one device on a transport's bus.
 */
struct ludlc_cfg_dev_item {
	struct ludlc_device *ldev;
	struct fwnode_handle *fwnode;

	/*
	 * The link the device created by this item joins. It comes from the
	 * ludlc_cfg_devices group of the owning transport, so it is only known
	 * once that transport has bound.
	 */
	struct ludlc_connection *conn;

	/* Serializes this item: its strings, its @chans, @flags and @ldev. */
	struct mutex lock;
	struct config_item item;

	/*
	 * Whether the device this item describes is on the bus. Written under
	 * @lock, and so non-atomically.
	 */
#define LUDLC_CFG_DEV_F_LIVE	0
	unsigned long flags;

	char *compatible;
	char *label;

	/*
	 * The channels "reg" names. @nchans is zero until it is written, since
	 * channel zero is itself a valid channel.
	 */
	ludlc_channel_t chans[LUDLC_CFG_REG_MAX];
	unsigned int nchans;
};

static inline
struct ludlc_cfg_dev_item *to_ludlc_cfg_dev_item(struct config_item *it)
{
	return container_of(it, struct ludlc_cfg_dev_item, item);
}

/**
 * @brief Is @p s usable as a device-tree style "compatible" string?
 *
 * The usual "vendor,device" identifier: alphanumeric at both ends, at least
 * one comma, and nothing but lowercase letters, digits and [,._+-]. Relax the
 * comma and character set rules here if single-token compatibles such as
 * "syscon" ever need to be supported.
 *
 * @param s Not necessarily NUL-terminated; only the first @p len bytes are
 * examined.
 */
static bool ludlc_compatible_valid(const char *s, size_t len)
{
	bool has_comma = false;
	size_t i;

	/*
	 * A string longer than of_device_id.compatible[] could never match a
	 * driver's of_match_table, so cap it there, NUL included. The lower
	 * bound is the shortest possible "vendor,device".
	 */
	if (len < 3 || len >= sizeof_field(struct of_device_id, compatible))
		return false;

	if (!isalnum(s[0]) || !isalnum(s[len - 1]))
		return false;

	for (i = 0; i < len; i++) {
		char c = s[i];

		if (islower(c) || isdigit(c))
			continue;

		switch (c) {
		case ',':
			has_comma = true;
			break;
		case '.':
		case '_':
		case '+':
		case '-':
			break;
		default:
			return false;
		}
	}

	return has_comma;
}

#define LUDLC_CFG_LABEL_MAX	64

/**
 * @brief Is @p s usable as a free-form "label"?
 *
 * A printable single-line token, bounded in length so that sysfs reads of it
 * stay clean. The cap is local policy, not a kernel limit.
 *
 * @param s Not necessarily NUL-terminated; only the first @p len bytes are
 * examined.
 */
static bool ludlc_label_valid(const char *s, size_t len)
{
	size_t i;

	if (len >= LUDLC_CFG_LABEL_MAX)
		return false;

	for (i = 0; i < len; i++)
		if (!isprint(s[i]))
			return false;

	return true;
}

/**
 * @brief Store @p page in *@p dst, without its trailing newline.
 *
 * Any previous value is freed. @p valid, if given, is applied to the stripped
 * string first, so a rejected write allocates nothing.
 *
 * @return 0, -EINVAL for an empty or rejected string, or -ENOMEM.
 */
static int ludlc_dev_str_set(char **dst, const char *page,
			     bool (*valid)(const char *, size_t))
{
	char *str;
	__kernel_size_t n;

	if (!dst || !page)
		return -EINVAL;

	n = strcspn(page, "\n");
	if (!n)
		return -EINVAL;

	if (valid && !valid(page, n))
		return -EINVAL;

	str = kstrndup(page, n, GFP_KERNEL);
	if (!str)
		return -ENOMEM;
	str[n] = '\0';

	kfree(*dst);
	*dst = str;

	return 0;
}

/**
 * @brief Parse the channel list of "reg" into @p chans.
 *
 * A comma-separated list, hyphenated ranges included, in any base
 * get_options() takes: "2", "2,4,7", "0x2-0x4". Whether the channels may be
 * owned together is for ludlc_device_alloc() to say.
 *
 * @param chans Room for @ref LUDLC_CFG_REG_MAX channels.
 *
 * @return The number of channels parsed, -EINVAL for a list that is empty,
 * malformed or longer than that, or -ERANGE for a value that is no channel.
 */
static int ludlc_dev_chans_parse(const char *page, ludlc_channel_t *chans)
{
	int ints[LUDLC_CFG_REG_MAX + 1];
	const char *end;
	int i;

	end = get_options(page, ARRAY_SIZE(ints), ints);

	/* What is left is either not a number or one value too many. */
	if (*end && *end != '\n')
		return -EINVAL;

	/*
	 * A range wider than the array counts the values it could not store,
	 * so the count is only valid within the array bounds.
	 */
	if (!ints[0] || ints[0] > LUDLC_CFG_REG_MAX)
		return -EINVAL;

	for (i = 1; i <= ints[0]; i++) {
		if (ints[i] < 0 || (u32)ints[i] > LUDLC_CHANNEL_MAX)
			return -ERANGE;

		chans[i - 1] = ints[i];
	}

	return ints[0];
}

/** @brief Remove this node's device from the bus, if it created one. */
static void ludlc_dev_teardown(struct ludlc_cfg_dev_item *it)
{
	lockdep_assert_held(&it->lock);

	if (!__test_and_clear_bit(LUDLC_CFG_DEV_F_LIVE, &it->flags))
		return;

	ludlc_device_unregister(it->ldev);
	fwnode_remove_software_node(it->fwnode);

	it->ldev	= NULL;
	it->fwnode	= NULL;
}

/*
 * What describes the device: a compatible string to match a driver by, an
 * optional label, and the channels it answers to. All of them are set before
 * the device goes on the bus and cannot change while it is there, so a store
 * returns -EBUSY while this node is live. An unwritten "reg" reads back empty,
 * channel zero being a valid channel.
 */
static ssize_t ludlc_dev_compatible_show(struct config_item *item, char *page)
{
	return sysfs_emit(page, "%s\n",
		 to_ludlc_cfg_dev_item(item)->compatible ?: "");
}

static ssize_t ludlc_dev_compatible_store(struct config_item *item,
				       const char *page, size_t count)
{
	struct ludlc_cfg_dev_item *it = to_ludlc_cfg_dev_item(item);
	int ret;

	scoped_guard(mutex, &it->lock) {
		if (test_bit(LUDLC_CFG_DEV_F_LIVE, &it->flags))
			ret = -EBUSY;
		else
			ret = ludlc_dev_str_set(&it->compatible, page,
						ludlc_compatible_valid);
	}

	return ret ? ret : count;
}
CONFIGFS_ATTR(ludlc_dev_, compatible);

static ssize_t ludlc_dev_label_show(struct config_item *item, char *page)
{
	return sysfs_emit(page, "%s\n",
		to_ludlc_cfg_dev_item(item)->label ?: "");
}

static ssize_t ludlc_dev_label_store(struct config_item *item,
				  const char *page, size_t count)
{
	struct ludlc_cfg_dev_item *it = to_ludlc_cfg_dev_item(item);
	int ret;

	mutex_lock(&it->lock);
	if (test_bit(LUDLC_CFG_DEV_F_LIVE, &it->flags))
		ret = -EBUSY;
	else
		ret = ludlc_dev_str_set(&it->label, page, ludlc_label_valid);
	mutex_unlock(&it->lock);

	return ret ? ret : count;
}
CONFIGFS_ATTR(ludlc_dev_, label);

/* A list to read as one value, so this takes @lock where the strings do not. */
static ssize_t ludlc_dev_reg_show(struct config_item *item, char *page)
{
	struct ludlc_cfg_dev_item *it = to_ludlc_cfg_dev_item(item);
	unsigned int i;
	int len = 0;

	guard(mutex)(&it->lock);

	for (i = 0; i < it->nchans; i++)
		len += sysfs_emit_at(page, len, i ? ",0x%x" : "0x%x",
				     it->chans[i]);

	return len + sysfs_emit_at(page, len, "\n");
}

static ssize_t ludlc_dev_reg_store(struct config_item *item,
				const char *page, size_t count)
{
	struct ludlc_cfg_dev_item *it = to_ludlc_cfg_dev_item(item);
	ludlc_channel_t chans[LUDLC_CFG_REG_MAX];
	int ret;

	/* Parsed first, so that a rejected write changes nothing. */
	ret = ludlc_dev_chans_parse(page, chans);
	if (ret < 0)
		return ret;

	guard(mutex)(&it->lock);

	if (test_bit(LUDLC_CFG_DEV_F_LIVE, &it->flags))
		return -EBUSY;

	memcpy(it->chans, chans, ret * sizeof(*chans));
	it->nchans = ret;

	return count;
}
CONFIGFS_ATTR(ludlc_dev_, reg);

static ssize_t ludlc_dev_live_show(struct config_item *item, char *page)
{
	return sysfs_emit(page, "%d\n",
		!!test_bit(LUDLC_CFG_DEV_F_LIVE,
				&to_ludlc_cfg_dev_item(item)->flags));
}

/**
 * @brief Put the device this node describes on the bus, or take it off again.
 *
 * The properties written here become a software node, which is what lets a
 * driver match and read them as it would those of a firmware-described device.
 * Nothing can be brought up without a channel and a compatible string, or
 * before the owning transport has a link for it to join.
 *
 * @return @p count, -EINVAL without a channel or a compatible string, -ENODEV
 * while the transport is down, or a negative errno from the bus: -EINVAL for
 * channels that cannot be owned together, -EBUSY for one a sibling device
 * already holds.
 */
static ssize_t ludlc_dev_live_store(struct config_item *item,
				 const char *page, size_t count)
{
	struct ludlc_cfg_dev_item *it = to_ludlc_cfg_dev_item(item);
	struct property_entry props[4];
	struct fwnode_handle *fwnode;
	u32 regs[LUDLC_CFG_REG_MAX];
	struct ludlc_device *ldev;
	unsigned int i;
	bool want;
	int n = 0;
	int ret;

	ret = kstrtobool(page, &want);
	if (ret)
		return ret;

	mutex_lock(&it->lock);

	/* Already in the requested state. */
	if (want == !!test_bit(LUDLC_CFG_DEV_F_LIVE, &it->flags))
		goto out;

	if (!want) {
		ludlc_dev_teardown(it);
		goto out;
	}

	/* A channel and a compatible string are the least a device needs. */
	if (!it->compatible || !it->nchans) {
		ret = -EINVAL;
		goto out;
	}

	/* Without a connection there is no bus to put the device on. */
	if (!it->conn) {
		ret = -ENODEV;
		goto out;
	}

	/* "reg" as firmware would have set it; the node copies the array. */
	for (i = 0; i < it->nchans; i++)
		regs[i] = it->chans[i];

	props[n++] = PROPERTY_ENTRY_STRING("compatible", it->compatible);
	props[n++] = PROPERTY_ENTRY_U32_ARRAY_LEN("reg", regs, it->nchans);
	if (it->label)
		props[n++] = PROPERTY_ENTRY_STRING("label", it->label);
	props[n] = (struct property_entry){ };

	fwnode = fwnode_create_software_node(props, NULL);
	if (IS_ERR(fwnode)) {
		ret = PTR_ERR(fwnode);
		goto out;
	}

	ldev = ludlc_device_alloc(it->conn, it->chans, it->nchans);
	if (IS_ERR(ldev)) {
		fwnode_remove_software_node(fwnode);
		ret = PTR_ERR(ldev);
		goto out;
	}

	device_set_node(&ldev->dev, fwnode);

	ret = ludlc_device_add(ldev);
	if (ret) {
		ludlc_device_put(ldev);
		fwnode_remove_software_node(fwnode);
		goto out;
	}

	it->ldev   = ldev;
	it->fwnode = fwnode;
	__set_bit(LUDLC_CFG_DEV_F_LIVE, &it->flags);

out:
	mutex_unlock(&it->lock);
	return ret ? ret : count;
}
CONFIGFS_ATTR(ludlc_dev_, live);

static struct configfs_attribute *ludlc_cfg_dev_attrs[] = {
	&ludlc_dev_attr_compatible,
	&ludlc_dev_attr_label,
	&ludlc_dev_attr_reg,
	&ludlc_dev_attr_live,
	NULL,
};

/** @brief Free a node, including the strings it holds. */
static void ludlc_cfg_dev_item_release(struct config_item *item)
{
	struct ludlc_cfg_dev_item *it = to_ludlc_cfg_dev_item(item);

	kfree(it->compatible);
	kfree(it->label);
	kfree(it);
}

static struct configfs_item_operations ludlc_cfg_dev_item_ops = {
	.release = ludlc_cfg_dev_item_release,
};

static const struct config_item_type ludlc_dev_item_type = {
	.ct_item_ops	= &ludlc_cfg_dev_item_ops,
	.ct_attrs	= ludlc_cfg_dev_attrs,
	.ct_owner	= THIS_MODULE,
};

/**
 * @brief mkdir: an empty node, describing nothing yet.
 *
 * @param conn The bus its device will join, or NULL while the owning transport
 * is down; ludlc_cfg_devices_group_attach() supplies it later.
 */
static struct config_item *ludlc_cfg_dev_item_create(const char *name,
						     struct ludlc_connection *conn)
{
	struct ludlc_cfg_dev_item *it;

	it = kzalloc(sizeof(*it), GFP_KERNEL);
	if (!it)
		return ERR_PTR(-ENOMEM);

	mutex_init(&it->lock);
	it->conn = conn;
	config_item_init_type_name(&it->item, name, &ludlc_dev_item_type);

	return &it->item;
}

/** @brief rmdir: the device goes away with the node that described it. */
static void ludlc_cfg_drop_item(struct config_group *group,
			 struct config_item *cit)
{
	struct ludlc_cfg_dev_item *it = to_ludlc_cfg_dev_item(cit);

	mutex_lock(&it->lock);
	ludlc_dev_teardown(it);
	mutex_unlock(&it->lock);

	config_item_put(cit);
}

/** @brief A node below a transport's group joins that transport's bus. */
static struct config_item *ludlc_cfg_devices_make_item(struct config_group *group,
						       const char *name)
{
	struct ludlc_cfg_devices *devs =
		container_of(group, struct ludlc_cfg_devices, group);

	return ludlc_cfg_dev_item_create(name, devs->conn);
}

static struct configfs_group_operations ludlc_cfg_devices_group_ops = {
	.make_item = ludlc_cfg_devices_make_item,
	.drop_item = ludlc_cfg_drop_item,
};

/*
 * No ct_item_ops/release: a ludlc_cfg_devices is embedded in, and freed with,
 * the transport structure that owns it.
 */
static const struct config_item_type ludlc_devices_group_type = {
	.ct_group_ops	= &ludlc_cfg_devices_group_ops,
	.ct_owner	= THIS_MODULE,
};

/**
 * @brief Set up @p devs as a group named @p name, with no link yet.
 *
 * For a transport to add to its own port group, typically as "ludlc".
 */
void ludlc_cfg_devices_group_init(struct ludlc_cfg_devices *devs,
				  const char *name)
{
	devs->conn = NULL;
	config_group_init_type_name(&devs->group, name,
				    &ludlc_devices_group_type);
}
EXPORT_SYMBOL_GPL(ludlc_cfg_devices_group_init);

/**
 * @brief Give @p devs the link its devices are to join.
 *
 * Called once the owning transport has one. Nodes created while it was down
 * are given the link as well, so they can be brought live as they are. The
 * caller must hold the mutex of the subsystem the group belongs to, as
 * configfs itself does around mkdir and rmdir.
 */
void ludlc_cfg_devices_group_attach(struct ludlc_cfg_devices *devs,
				    struct ludlc_connection *conn)
{
	struct config_item *item;

	devs->conn = conn;

	list_for_each_entry(item, &devs->group.cg_children, ci_entry) {
		struct ludlc_cfg_dev_item *it = to_ludlc_cfg_dev_item(item);

		guard(mutex)(&it->lock);

		it->conn = conn;
	}
}
EXPORT_SYMBOL_GPL(ludlc_cfg_devices_group_attach);

/**
 * @brief Remove every device @p devs created, and forget the link.
 *
 * Called when the owning transport goes away: the devices belong to its
 * connection and cannot outlive it. The nodes stay, ready to be brought live
 * again after the next ludlc_cfg_devices_group_attach(), whose locking rules
 * apply here too.
 */
void ludlc_cfg_devices_group_detach(struct ludlc_cfg_devices *devs)
{
	struct config_item *item;

	list_for_each_entry(item, &devs->group.cg_children, ci_entry) {
		struct ludlc_cfg_dev_item *it = to_ludlc_cfg_dev_item(item);

		guard(mutex)(&it->lock);

		ludlc_dev_teardown(it);
		it->conn = NULL;
	}

	devs->conn = NULL;
}
EXPORT_SYMBOL_GPL(ludlc_cfg_devices_group_detach);

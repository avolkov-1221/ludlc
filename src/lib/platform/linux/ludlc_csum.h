/* SPDX-License-Identifier: GPL-2.0-only */
/**
 * @file ludlc_csum.h
 *
 * @brief The checksum a LuDLC link runs, and the registry of the ones that can
 * be asked for by name.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * Both ends of a link must agree on the checksum, which makes it a property of
 * the connection rather than of whatever carries it: a link over a UART and a
 * link over SPI choose theirs the same way, and a module implementing another
 * one registers it here once for all of them. A transport still decides
 * whether to use a checksum at all - one whose controller checks frames itself
 * needs none - but not which one.
 */

#ifndef __LUDLC_CSUM_H__
#define __LUDLC_CSUM_H__

#include <linux/module.h>

/* For struct ludlc_csum, the args a link is brought up from and its cfg. */
#include <ludlc.h>

struct device;

/**
 * @brief The checksums the core provides, for a new link to point its args at.
 *
 * Each follows the kernel CRC library it is computed with, so a caller that
 * needs one must either depend on that config or supply its own descriptor.
 * No transport requires any of them.
 */
#if IS_ENABLED(CONFIG_CRC_CCITT)
extern const struct ludlc_csum ludlc_crc16_ccitt_pr0;
extern const struct ludlc_csum ludlc_crc16_x25_pr1;
#endif
#if IS_ENABLED(CONFIG_CRC16)
extern const struct ludlc_csum ludlc_crc16_pr0;
#endif
#if IS_ENABLED(CONFIG_CRC32)
extern const struct ludlc_csum ludlc_crc32_ccitt_pr0;
#endif

/**
 * @brief Register @p crc under its name so that the text interfaces can select
 * it: a ludlc,csum firmware property or a configfs write.
 *
 * A module that only needs the checksum for its own links should point them at
 * the descriptor instead. @p crc and its name must stay valid until it is
 * unregistered. The owning module is pinned while any link uses the checksum,
 * so its code cannot be unloaded while in use. Unregistering hides the
 * checksum from new links and leaves existing ones running.
 *
 * @return 0 on success, -EEXIST if the name is taken, -EINVAL if the
 * descriptor is inconsistent (the residual it declares is computed and
 * checked, since a broken checksum would otherwise show up only as a link that
 * never comes up), or -EOPNOTSUPP for a width that is not a whole number of
 * octets, 64 bits being the widest supported.
 */
int __ludlc_crc_register(const struct ludlc_csum *crc,
			 struct module *owner);
#define ludlc_crc_register(crc) __ludlc_crc_register(crc, THIS_MODULE)

void ludlc_crc_unregister(const struct ludlc_csum *crc);

/**
 * @brief Look up the checksum registered as @p name and take a reference to it.
 *
 * A trailing newline is ignored, so a configfs write can be passed in as it is.
 * Use this only to turn a name into a descriptor.
 *
 * @return The descriptor, to be released with ludlc_csum_put(), or NULL if no
 * such checksum is registered.
 */
const struct ludlc_csum *ludlc_crc_find(const char *name);

/**
 * @def LUDLC_CSUM_NONE
 * @brief The name that chooses no checksum, wherever one is chosen by name.
 */
#define LUDLC_CSUM_NONE		"none"

/**
 * @brief Turn a checksum name, or @ref LUDLC_CSUM_NONE, into a descriptor.
 *
 * What the crc= module parameter, firmware and configfs go through, so that
 * "none" means the same in all three.
 *
 * @param name As ludlc_crc_find() takes it, or "none".
 * @param[out] csum The descriptor, to be released with ludlc_csum_put(), or
 * NULL for "none".
 * @return 0, or -EINVAL if no checksum is registered as @p name.
 */
int ludlc_crc_by_name(const char *name, const struct ludlc_csum **csum);

/**
 * @brief Take and drop a reference to @p crc.
 *
 * The descriptors the core provides, and any the caller wrote itself, have no
 * provider to reference and are returned unchanged. A registered one pins the
 * module that implements it.
 *
 * @return NULL only if the providing module is already being unloaded.
 */
const struct ludlc_csum *ludlc_csum_get(
					const struct ludlc_csum *crc);
void ludlc_csum_put(const struct ludlc_csum *crc);

/**
 * @brief The checksum used by a link that chooses none itself: the crc=
 * module parameter, or the descriptor this build was configured with. NULL
 * for crc=none, a link then running no checksum.
 */
const struct ludlc_csum *ludlc_csum_default(void);

/**
 * @brief Check a descriptor a link is about to use, and report why not.
 *
 * The same checks a registered checksum went through, repeated because the
 * descriptor may be the caller's own and never have been validated. Costs two
 * short frames per link brought up.
 *
 * @param dev The device to log through, for a transport to name the port.
 * @return 0, -EOPNOTSUPP for a width that cannot be sent, or -EINVAL.
 */
int ludlc_csum_check(struct device *dev,
		     const struct ludlc_csum *csum);

/**
 * @brief List the names ludlc_crc_by_name() accepts, "none" first, space
 * separated and newline terminated, for a configfs attribute to return.
 *
 * The list depends on the kernel config and on what has been registered.
 *
 * @return What sysfs_emit() returned.
 */
int ludlc_csum_list(char *page);

/**
 * @brief Record in @p slot the checksum a link will run, and hold it there.
 *
 * The slot belongs to the transport, which is where every port keeps the
 * checksum: both ends of a link must agree on it, but nothing in the core
 * reads it. The descriptor is checked before it is accepted, because a
 * caller may have written its own, and a wrong residual shows up only as a
 * link on which nothing ever arrives.
 *
 * @param slot Where the transport keeps it, for the life of the link.
 * @param dev The device to log through.
 * @param csum What to run, or NULL for none.
 * @return 0 with the checksum held and in @p slot, -ENODEV if the module
 * providing it is being unloaded, or what ludlc_csum_check() made of the
 * descriptor.
 */
int ludlc_link_csum_set(const struct ludlc_csum **slot, struct device *dev,
			const struct ludlc_csum *csum);

/**
 * @brief Let go of the checksum in @p slot and clear it.
 *
 * Left until the transport tears itself down, because only the transport
 * knows when its framing last touched the checksum: the module implementing
 * it may be released only once the encoder and decoder have stopped.
 */
void ludlc_link_csum_drop(const struct ludlc_csum **slot);

/**
 * @brief Drop the checksum reference @p args carries.
 *
 * Gathering the arguments for a link may have looked a checksum up by name,
 * which holds it; bringing the link up takes a reference of its own. Call this
 * once the attempt is over, whether it worked or not.
 */
static inline void ludlc_args_csum_put(ludlc_platform_args_t *args)
{
	ludlc_csum_put(args->csum);
	args->csum = NULL;
}

/**
 * @brief Check the built-in checksums and decide the system default.
 *
 * Called once as the core module loads, before any transport can ask for a
 * checksum.
 *
 * @return 0, or -EINVAL for a broken built-in or an unknown crc= name, which
 * fails the load rather than leaving every link on the wrong checksum.
 */
int ludlc_csum_setup(void);

#endif /* __LUDLC_CSUM_H__ */

// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only)
/**
 * @file ludlc_framing.c
 *
 * @brief Looking up a framing by name.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * Each framing describes itself in its own file: ludlc_hdlc.c, ludlc_cobs.c
 * and ludlc_raw.c. This file only holds the list a link is opened from, and
 * a build with a single framing does not compile it.
 */

#ifdef __KERNEL__
#include <linux/export.h>
#include <linux/string.h>
#else
#include <string.h>
#endif

#include <ludlc_framing.h>
#include <ludlc_private.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a)	(sizeof(a) / sizeof((a)[0]))
#endif

static const struct {
	const char		   *name;
	const struct ludlc_framing *framing;
} framing[] = {
#if CONFIG_LUDLC_FRAMING_COBS
	{"cobs", &ludlc_framing_cobs},
#endif
#if CONFIG_LUDLC_FRAMING_RAW
	{"raw", &ludlc_framing_raw},
#endif
#if CONFIG_LUDLC_FRAMING_HDLC
	{"hdlc", &ludlc_framing_hdlc},
#endif
};

const struct ludlc_framing *ludlc_framing_lookup(const char *name)
{
	unsigned int i;
#ifndef __KERNEL__
	size_t n = strlen(name);

	while (n && (name[n - 1] == '\n' || name[n - 1] == '\r'))
		n--;
#endif

	for (i = 0; i < ARRAY_SIZE(framing); i++) {
#ifdef __KERNEL__
		if (sysfs_streq(name, framing[i].name))
			return framing[i].framing;
#else
		if (n == strlen(framing[i].name) &&
			!strncmp(name, framing[i].name, n))
			return framing[i].framing;
#endif
	}

	return NULL;
}
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(ludlc_framing_lookup);
#endif

const struct ludlc_framing *ludlc_framing_at(unsigned int i)
{
	return i < ARRAY_SIZE(framing) ? framing[i].framing : NULL;
}
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(ludlc_framing_at);
#endif

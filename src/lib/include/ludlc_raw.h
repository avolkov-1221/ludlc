/* SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only) */
/**
 * @file ludlc_raw.h
 *
 * @brief Entry points of the framing that carries frames unchanged.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * The state machines are in ludlc_raw.c, reached through
 * @ref ludlc_framing_raw_ops. Their prototypes come from
 * @ref LUDLC_DECLARE_FRAMING, as every framing's do, and nothing else is
 * declared here.
 */

#ifndef __LUDLC_RAW_H__
#define __LUDLC_RAW_H__

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stdint.h>
#endif

/* For the packet types and the counters a rejected frame is counted in. */
#include <ludlc_private.h>

/* The encoder and decoder state is private to ludlc_raw.c. A link gets
 * one through create() below and passes it to every other operation.
 */
LUDLC_DECLARE_FRAMING(raw)

#endif /* __LUDLC_RAW_H__ */

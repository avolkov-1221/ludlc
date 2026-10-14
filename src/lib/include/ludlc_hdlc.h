/* SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only) */
/**
 * @file ludlc_hdlc.h
 *
 * @brief The escaping: the octets it reserves, and its entry points.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * The state machines are in ludlc_hdlc.c, reached through
 * @ref ludlc_framing_hdlc_ops. Their prototypes come from
 * @ref LUDLC_DECLARE_FRAMING, as every framing's do.
 */

#ifndef __LUDLC_HDLC_H__
#define __LUDLC_HDLC_H__

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stdint.h>
#endif

/* For the packet types and the counters a rejected frame is counted in. */
#include <ludlc_private.h>

/**
 * @def LUDLC_SOF
 * @brief Start Of packet sequence, aka frame boundary.
 *
 * The value 0x55 (0b01010101) is easier to find/check baudrate
 * in the bitstream by oscilloscope than the original HDLC SOF 0x7E (0b01111110).
 */
#ifndef CONFIG_LUDLC_SOF
#define LUDLC_SOF 0x55
#else
#define LUDLC_SOF CONFIG_LUDLC_SOF
#endif

/**
 * @def LUDLC_ESC
 * @brief Escape sequence. Ditto. 0xaa is 0b10101010.
 *
 * Any octet matching @ref LUDLC_SOF or @ref LUDLC_ESC in the payload
 * is replaced by @ref LUDLC_ESC followed by the original octet
 * XORed with @ref LUDLC_MASK.
 */
#ifndef CONFIG_LUDLC_ESC
#define LUDLC_ESC 0xaa
#else
#define LUDLC_ESC CONFIG_LUDLC_ESC
#endif

/**
 * @def LUDLC_MASK
 * @brief XOR mask used for escaping data octets.
 */
#define LUDLC_MASK 0x20

/* The encoder and decoder state is private to ludlc_hdlc.c. A link gets
 * one through create() below and passes it to every other operation.
 */
LUDLC_DECLARE_FRAMING(hdlc)

#endif /* __LUDLC_HDLC_H__ */

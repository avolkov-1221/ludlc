// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file ludlc_zephyr_csum.c
 *
 * @brief The checksums a link on Zephyr can run.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * Both ends of a link must run the same checksum, so these carry the names,
 * seeds, wire transforms and residuals of the ones the Linux port registers,
 * computed with the Zephyr CRC library. A transport takes one through
 * ludlc_platform_args_t, and a devicetree node selects one by name.
 */

#include <stdint.h>

#include <zephyr/sys/crc.h>

#include <ludlc.h>

/*
 * The reflected 0x1021, one octet at a time as the framing feeds it. Seeded
 * with zero it is CRC-16/KERMIT; seeded with all ones and complemented on the
 * wire, the HDLC FCS-16 of X.25 and PPP.
 */
static ludlc_csum_t ludlc_crc16_ccitt_byte(ludlc_csum_t csum, uint8_t data)
{
	return crc16_ccitt((uint16_t)csum, &data, 1);
}

/* HDLC FCS-16 goes out complemented, which leaves the receiver with 0xf0b8. */
static ludlc_csum_t ludlc_x25_to_wire(ludlc_csum_t csum)
{
	return (uint16_t)~csum;
}

/* CRC-16/ARC: the reflected 0x8005. */
static ludlc_csum_t ludlc_crc16_byte(ludlc_csum_t csum, uint8_t data)
{
	return crc16_reflect(0xa001, (uint16_t)csum, &data, 1);
}

const struct ludlc_csum ludlc_crc16_ccitt_pr0 = {
	.name = "crc16-ccitt-pr0",
	.octets = 2,
	.csum = ludlc_crc16_ccitt_byte,
};

const struct ludlc_csum ludlc_crc16_x25_pr1 = {
	.name = "crc16-x25-pr1",
	.octets = 2,
	.csum = ludlc_crc16_ccitt_byte,
	.init = 0xffff,
	.verify = 0xf0b8,
	.to_wire = ludlc_x25_to_wire,
};

const struct ludlc_csum ludlc_crc16_pr0 = {
	.name = "crc16-pr0",
	.octets = 2,
	.csum = ludlc_crc16_byte,
};

#if CONFIG_LUDLC_CSUM_BITS >= 32
/*
 * The reflected CRC-32 with a zero seed and nothing complemented, as the Linux
 * crc32_le() computes it. crc32_ieee_update() complements on the way in and on
 * the way out, which the complements here undo.
 */
static ludlc_csum_t ludlc_crc32_byte(ludlc_csum_t csum, uint8_t data)
{
	return (uint32_t)~crc32_ieee_update(~(uint32_t)csum, &data, 1);
}

const struct ludlc_csum ludlc_crc32_ccitt_pr0 = {
	.name = "crc32-ccitt-pr0",
	.octets = 4,
	.csum = ludlc_crc32_byte,
};
#endif

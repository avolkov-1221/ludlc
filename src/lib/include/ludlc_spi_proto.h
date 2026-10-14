/* SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only) */
/**
 * @file ludlc_spi_proto.h
 *
 * @brief The wire format of a LuDLC link over SPI, as both ends must implement
 * it.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * SPI has no equivalent of a UART's two independent directions: nothing moves
 * until the controller clocks it, and every octet clocked out brings one
 * back. A peripheral cannot say when it has something to send, so the format
 * below is built around the controller asking, and around the peripheral
 * being able to request that it does. This header describes only what the two
 * ends must agree on and is shared by both, whatever platform either runs on.
 *
 * @section spi_wire The exchange
 *
 * One exchange is two transfers, each with its own chip select.
 *
 * The first is @ref LUDLC_SPI_HDR_LEN octets and never varies. Each end sends
 * a @ref ludlc_spi_hdr giving the length of the frame it has to send, and
 * receives the other's in the same clocks. A length of zero means that end
 * has nothing to send.
 *
 * The second transfer carries the frames and is as long as the longer of the
 * two announced lengths, which both ends know by then. An end whose frame is
 * shorter pads with @ref LUDLC_SPI_PAD, and an end with nothing to send pads
 * the whole transfer. If both announced nothing, it does not happen at all.
 *
 * The chip select is released between the two, which is where a peripheral
 * prepares a buffer of the right length. The length need not be exact: it may
 * offer the most its link can announce - @ref LUDLC_COBS_FRAME_MAX for the
 * widest framing and its checksum - and let the chip select cut the transfer
 * short. The controller waits as long as the peripheral's firmware says it
 * needs.
 *
 * @section spi_frame The frame
 *
 * The second transfer carries a LuDLC packet with its checksum, in whatever
 * form the link's framing puts on the wire. The announcement delimits it: a
 * receiver takes exactly the announced number of octets and hands those to
 * the framing, so nothing in the frame has to stand out from the padding
 * after it.
 *
 * Two framings suit this transport, and a link may run either as long as both
 * ends run the same one:
 *
 * - "raw" sends the frame as it stands and adds nothing. It relies entirely
 *   on the announcement, and is the default here.
 *
 * - "cobs" stuffs the frame and ends it with a zero, costing one octet per
 *   254 plus the delimiter. Nothing here needs it; it is for a peer that
 *   already runs COBS on its UART links.
 *
 * A framing that cannot give a frame length before encoding - the HDLC
 * escaping - cannot be carried at all, as there would be nothing to announce.
 *
 * The checksum is the link's, not the transport's: both ends must agree on
 * it, the same way as on a link over any other wire.
 *
 * @section spi_desync Losing step
 *
 * The two transfers of an exchange are told apart by their order, not by
 * anything in them, so an end that misses one reads the next as the other
 * half. Nothing prevents this: an announcement read out of a frame fails
 * @ref ludlc_spi_hdr_get, a frame read out of an announcement fails the
 * framing or the checksum, and the protocol resends what was lost. A link
 * recovers within an exchange or two.
 */

#ifndef __LUDLC_SPI_PROTO_H__
#define __LUDLC_SPI_PROTO_H__

#ifdef __KERNEL__
#include <linux/build_bug.h>
#include <linux/types.h>
#else
#include <assert.h>
#include <stdint.h>
#include <sys/types.h>
#endif

/* For LUDLC_COBS_FRAME_MAX: the longest frame an announcement may name. */
#include <ludlc_cobs.h>

/**
 * @def LUDLC_SPI_MAGIC
 * @brief What every announcement starts with.
 *
 * An end that is unpowered, held in reset or not listening leaves the line to
 * a pull resistor, so the announcement arrives all-zero or all-ones. Neither
 * passes: the first fails the check octet, the second fails this. Any value
 * other than 0x00 and 0xff would do; this one is easy to recognise on a
 * scope.
 */
#define LUDLC_SPI_MAGIC		0x5a

/**
 * @def LUDLC_SPI_PAD
 * @brief What an end sends for the part of the transfer beyond its frame.
 *
 * Never read: the announcement says where the frame ends and the receiver
 * stops there. Zero because a line left to a pull-down reads the same, so a
 * peer that stops answering mid-transfer produces no data.
 */
#define LUDLC_SPI_PAD		0x00

/**
 * @struct ludlc_spi_hdr
 * @brief What each end sends in the first transfer of an exchange.
 *
 * Little-endian on the wire whatever the two ends are, as everywhere else in
 * this protocol, and written as separate octets rather than one wide field so
 * that it reads the same on either end. Use @ref ludlc_spi_hdr_put and
 * @ref ludlc_spi_hdr_get rather than building or parsing it by hand. The
 * length is four octets so that it can name any frame a link may be
 * configured for.
 */
struct ludlc_spi_hdr {
	/** @ref LUDLC_SPI_MAGIC. */
	uint8_t	magic;
	/**
	 * @brief @c magic, every octet of @c len, and 0xff, xored together.
	 *
	 * Enough to keep a wrong length from being acted on. One that got
	 * through would cost a single exchange, which the framing and the
	 * link's checksum reject and the protocol resends past. The frame
	 * itself is protected by that checksum, not by this.
	 */
	uint8_t	check;
	/** Frame length, least significant octet first, 0 for nothing. */
	uint8_t	len[4];
};

/** @brief Octets in the first transfer of an exchange. */
#define LUDLC_SPI_HDR_LEN	sizeof(struct ludlc_spi_hdr)

/*
 * Six octets in this order on both ends. A compiler that padded this would
 * put the length where the far end does not read it.
 */
static_assert(LUDLC_SPI_HDR_LEN == 6,
	      "the announcement must be six octets with nothing between them");

/*
 * A frame this build can produce but cannot announce would be sent with a
 * truncated length, and the far end would clock the wrong number of octets
 * and treat the result as a frame.
 */
static_assert(LUDLC_COBS_FRAME_MAX(LUDLC_MAX_CSUM_SIZE) <= 0x7fffffffL,
	      "CONFIG_LUDLC_MAX_PAYLOAD_SIZE is too large to announce");

/**
 * @brief Announce a frame of @p len octets, or none if @p len is zero.
 *
 * @param hdr Announcement to fill in, to be sent as it stands.
 * @param len Octets of encoded frame the second transfer will carry.
 */
static inline void ludlc_spi_hdr_put(struct ludlc_spi_hdr *hdr, uint32_t len)
{
	unsigned int i;

	hdr->magic = LUDLC_SPI_MAGIC;
	hdr->check = LUDLC_SPI_MAGIC ^ 0xff;

	for (i = 0; i < sizeof(hdr->len); i++) {
		hdr->len[i] = (uint8_t)(len >> (8U * i));
		hdr->check ^= hdr->len[i];
	}
}

/**
 * @brief How long a frame the far end announced, if it announced one at all.
 *
 * @param hdr The announcement as it arrived.
 * @param max_frame The longest frame this link carries, from
 * ludlc_cobs_frame_sz().
 * @return The length, 0 for an end with nothing to send, or -1 for an
 * announcement that was damaged or never made.
 */
static inline ssize_t ludlc_spi_hdr_get(const struct ludlc_spi_hdr *hdr,
					ludlc_frame_size_t max_frame)
{
	uint8_t check = LUDLC_SPI_MAGIC ^ 0xff;
	/*
	 * Read at the wire width rather than that of a frame size here:
	 * narrowing first could turn a length this link cannot carry into one
	 * it can, past the check below.
	 */
	uint32_t len = 0;
	unsigned int i;

	if (hdr->magic != LUDLC_SPI_MAGIC) {
		return -1;
	}

	for (i = 0; i < sizeof(hdr->len); i++) {
		len |= (uint32_t)hdr->len[i] << (8U * i);
		check ^= hdr->len[i];
	}

	if (hdr->check != check) {
		return -1;
	}

	/*
	 * A length this link cannot carry means the two ends disagree about
	 * the payload size, or the announcement was damaged in a way the
	 * check octet let through. Clocking that many octets achieves
	 * nothing.
	 */
	if (len > max_frame) {
		return -1;
	}

	return (ssize_t)len;
}

#endif /* __LUDLC_SPI_PROTO_H__ */

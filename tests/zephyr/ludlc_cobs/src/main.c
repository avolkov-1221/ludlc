// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file ludlc_cobs/src/main.c
 *
 * @brief Pins the stuffing to the octets a LuDLC frame is made of.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * The same cases as tests/posix/ludlc_cobs, built against Zephyr's CRC and
 * the widths the module's Kconfig settles on, and run on a board rather than
 * a host. The stuffing contains no RTOS code, so what this adds is the build
 * and the board.
 *
 * The cases are fixed octets rather than a comparison against another
 * implementation: the far end of a link may run a COBS written from the
 * standard, so any change to the wire form must fail here.
 *
 * What is checked, in order:
 *
 *   - the published COBS vectors, so that a peer written from the standard
 *     rather than from this code interoperates;
 *   - the exact encoded length of data with no zero in it, at the lengths
 *     where a run ends and a new group starts. The length itself rather than
 *     a bound, since a spare octet would still decode and go unnoticed;
 *   - the exact encoding where a group fills up next to a zero, which is the
 *     one place two COBS implementations differ without either looking wrong;
 *   - a round trip over data of every mix of zero and non-zero;
 *   - that the frame comes out the same however the transport splits up the
 *     writing of it, the encoder buffering nothing between calls;
 *   - one whole frame, checksum included, since the frame layer sits on top
 *     of the stuffing.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

#include <ludlc_private.h>
#include <ludlc_framing.h>

ZTEST_SUITE(ludlc_cobs, NULL, NULL, NULL, NULL, NULL);

/**
 * @def DATA_MAX
 * @brief The most data these tests can push through the framing.
 *
 * A decoded packet size is reported in ludlc_payload_size_t, so a build with a
 * narrow one cannot report more than this however the frame was split on the
 * way in.
 */
#define DATA_MAX	((size_t)((ludlc_payload_size_t)-1))

/**
 * @brief Data to frame, from a fixed sequence rather than from entropy.
 *
 * Nothing here is cryptographic; a mix of zero and non-zero octets is all
 * that is needed. Fixed so a failure can be reproduced from the report alone,
 * and local so the board needs no entropy source.
 */
static uint32_t prng(void)
{
	static uint32_t x = 12345;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;

	return x;
}

/**
 * @brief A zero-width checksum, so that a frame is its data and nothing else.
 *
 * The vectors and boundaries below are about the stuffing, and a checksum on
 * the end would only add octets to stuff. The frame test uses a real one.
 */
static ludlc_csum_t sum_none(ludlc_csum_t c, uint8_t b)
{
	ARG_UNUSED(b);

	return c;
}

static const struct ludlc_csum none = {
	.name = "none",
	.octets = 0,
	.csum = sum_none,
};

/** @brief CRC-16/KERMIT, as LuDLC peers speak: seed and residual both zero. */
static ludlc_csum_t csum_byte(ludlc_csum_t crc, uint8_t data)
{
	return crc16_ccitt(crc, &data, 1);
}

static const struct ludlc_csum alg = {
	.name = "crc16-kermit",
	.octets = 2,
	.csum = csum_byte,
};

/*
 * Static rather than on a test thread's stack: a build whose payload is a
 * megabyte would otherwise need three of them there, which is the cost this
 * framing exists to avoid.
 */
static uint8_t pay[CONFIG_LUDLC_MAX_PAYLOAD_SIZE];
static uint8_t frame[LUDLC_COBS_FRAME_MAX(LUDLC_MAX_CSUM_SIZE)];
static uint8_t got[LUDLC_FRAME_BUF_SIZE];

/**
 * @brief Stuff @p len octets of @p raw into @p out, delimiter included.
 *
 * Passed as a header and a payload rather than one buffer, both because that
 * is how a link presents a packet and because a group spanning the two is a
 * case a single buffer would never reach.
 *
 * @return Octets written, or -1 if @p cap was too small for the frame.
 */
static ssize_t stuff(uint8_t *out, size_t cap, const uint8_t *raw, size_t len)
{
	struct ludlc_framing_state *fs = ludlc_framing_create(
						&ludlc_framing_cobs);
	/* Halved, so that a group spanning the join is covered too. */
	ludlc_payload_size_t hdr_sz = (ludlc_payload_size_t)((len + 1) / 2);
	size_t n;
	bool left;

	ludlc_enc_init(fs);
	ludlc_enc_prep(fs, raw, hdr_sz, raw + hdr_sz,
		      (ludlc_payload_size_t)(len - hdr_sz), false);

	n = ludlc_enc_fill(fs, &none, out, cap);

	/* Still holding octets: the frame did not fit in @p cap. */
	left = ludlc_enc_busy(fs);
	ludlc_framing_destroy(fs);

	return left ? -1 : (ssize_t)n;
}

/**
 * @brief Unstuff into @p out the @p len octets of @p enc, delimiter excluded.
 *
 * The decoder only accepts or rejects a frame once the delimiter arrives, and
 * bare data is shorter than the smallest packet a link accepts. So the result
 * is read out just before the delimiter, where everything is unstuffed and
 * nothing has been judged yet.
 *
 * @return Octets recovered.
 */
static ssize_t unstuff(uint8_t *out, ludlc_frame_size_t cap,
		       const uint8_t *enc, size_t len)
{
	struct ludlc_framing_state *fs = ludlc_framing_create(
						&ludlc_framing_cobs);
	struct ludlc_connection conn;
	ludlc_payload_size_t sz;
	bool done;

	memset(&conn, 0, sizeof(conn));

	ludlc_dec_init(fs);
	ludlc_dec_prep(fs, &none, out, cap);
	ludlc_dec_feed(fs, &none, &conn, enc, len, &done);

	sz = ludlc_dec_size(fs);
	ludlc_framing_destroy(fs);

	return (ssize_t)sz;
}

/**
 * @brief The vectors from the COBS paper, which firmware is as likely to be
 * written from as from this code.
 */
ZTEST(ludlc_cobs, test_published_vectors)
{
	static const struct {
		uint8_t len;
		uint8_t raw[8];
		uint8_t elen;
		uint8_t enc[8];
	} v[] = {
		{ 1, {0x00},                   2, {0x01, 0x01} },
		{ 2, {0x00, 0x00},             3, {0x01, 0x01, 0x01} },
		{ 3, {0x11, 0x22, 0x33},       4, {0x04, 0x11, 0x22, 0x33} },
		{ 4, {0x11, 0x00, 0x00, 0x00}, 5, {0x02, 0x11, 0x01, 0x01, 0x01} },
		{ 4, {0x00, 0x11, 0x00, 0x00}, 5, {0x01, 0x02, 0x11, 0x01, 0x01} },
		{ 2, {0x11, 0x00},             3, {0x02, 0x11, 0x01} },
	};
	uint8_t out[32], back[32];

	for (unsigned int i = 0; i < ARRAY_SIZE(v); i++) {
		ssize_t n = stuff(out, sizeof(out), v[i].raw, v[i].len);
		ssize_t m;

		/* The encoder ends the frame itself: one octet more. */
		zassert_equal(n, v[i].elen + 1,
			      "vector %u encoded to %zd octets, wanted %u",
			      i, n, v[i].elen + 1);
		zassert_mem_equal(out, v[i].enc, v[i].elen,
				  "vector %u encoded to the wrong octets", i);
		zassert_equal(out[n - 1], LUDLC_COBS_DELIMITER,
			      "vector %u does not end on the delimiter", i);

		m = unstuff(back, sizeof(back), out, (size_t)n - 1);
		zassert_equal(m, v[i].len, "vector %u decoded to %zd octets, "
			      "wanted %u", i, m, v[i].len);
		zassert_mem_equal(back, v[i].raw, v[i].len,
				  "vector %u came back different", i);
	}
}

/**
 * @brief Exact encodings where a group fills up next to a zero.
 *
 * The one place two COBS implementations differ without either looking wrong.
 * A full group ends on no zero while a short one does, so a zero arriving just
 * as a group fills is accounted for either by the closing group or by an empty
 * one after it, and only one of those decodes back to the original data.
 *
 * The lengths are exact, not bounds: a different grouping of the same data
 * gives a different length. The same numbers appear in tests/posix/ludlc_cobs,
 * so the two suites cannot drift apart here while both still round-trip
 * against themselves.
 */
ZTEST(ludlc_cobs, test_zero_boundaries)
{
	static const struct {
		const char	*what;
		/**< Non-zero octets before the zero. */
		uint16_t	pre;
		/**< Non-zero octets after it. */
		uint16_t	post;
		/**< Octets the encoding must come to, delimiter excluded. */
		uint16_t	elen;
	} zb[] = {
		{ "a full group then a zero",      254, 0,   257 },
		{ "a group one short then a zero", 253, 0,   255 },
		{ "a zero then a full group",      0,   254, 256 },
		{ "a full group either side",      254, 254, 511 },
	};
	static uint8_t raw[1024], enc[2048], back[1024];

	for (unsigned int t = 0; t < ARRAY_SIZE(zb); t++) {
		size_t len = (size_t)zb[t].pre + 1 + zb[t].post;
		ssize_t n, m;

		/* A narrow size type cannot report a packet this large. */
		if (len > DATA_MAX) {
			continue;
		}

		memset(raw, 0xa5, zb[t].pre);
		raw[zb[t].pre] = 0;
		memset(raw + zb[t].pre + 1, 0x5a, zb[t].post);

		n = stuff(enc, sizeof(enc), raw, len);
		zassert_equal(n, zb[t].elen + 1,
			      "%s: encoded to %zd octets, wanted %u",
			      zb[t].what, n, zb[t].elen + 1);

		m = unstuff(back, sizeof(back), enc, (size_t)n - 1);
		zassert_equal(m, (ssize_t)len, "%s: got %zd back, wanted %zu",
			      zb[t].what, m, len);
		zassert_mem_equal(back, raw, len, "%s: came back different",
				  zb[t].what);
	}
}

/**
 * @brief What @p len octets with no zero among them encode to, exactly.
 *
 * One code octet per group, groups of at most LUDLC_COBS_BLOCK_MAX octets, and
 * no empty group after one ending flush with the data. The delimiter is not
 * counted.
 */
static size_t packed_len(size_t len)
{
	if (!len) {
		return 1;
	}

	return len + (len + LUDLC_COBS_BLOCK_MAX - 1) / LUDLC_COBS_BLOCK_MAX;
}

/**
 * @brief The lengths either side of where a group fills up.
 *
 * A group holds at most LUDLC_COBS_BLOCK_MAX octets, so these are where the
 * encoder decides whether another group follows, the one decision it can get
 * wrong unnoticed. A frame always carries a header, so none of them is
 * empty.
 */
static const uint16_t boundary_lens[] = {
	1, 2, 253, 254, 255, 256, 507, 508, 509, 1000
};

ZTEST(ludlc_cobs, test_exact_encoded_length)
{
	static uint8_t raw[1024], enc[2048];

	for (unsigned int i = 0; i < ARRAY_SIZE(raw); i++) {
		/* Anything but a zero, which would end a group early. */
		raw[i] = (uint8_t)(1 + (i % 255));
	}

	for (unsigned int t = 0; t < ARRAY_SIZE(boundary_lens); t++) {
		uint16_t len = boundary_lens[t];
		ssize_t n;

		if (len > DATA_MAX) {
			continue;
		}

		n = stuff(enc, sizeof(enc), raw, len);

		zassert_equal(n, (ssize_t)packed_len(len) + 1,
			      "%u octets encoded to %zd, wanted %zu",
			      len, n, packed_len(len) + 1);
	}
}

ZTEST(ludlc_cobs, test_round_trip)
{
	static uint8_t raw[1024], enc[2048], back[1024];

	for (unsigned int t = 0; t < ARRAY_SIZE(boundary_lens); t++) {
		uint16_t len = boundary_lens[t];

		if (len > DATA_MAX) {
			continue;
		}

		/* All non-zero, then ever more zero-ridden, then all zero. */
		for (unsigned int zeroes = 0; zeroes <= 8; zeroes++) {
			ssize_t n, m;

			for (unsigned int i = 0; i < len; i++) {
				if (zeroes == 8) {
					raw[i] = 0;
				} else if (zeroes &&
					   (prng() % 8) < zeroes) {
					raw[i] = 0;
				} else {
					raw[i] = 1 + prng() % 255;
				}
			}

			n = stuff(enc, sizeof(enc), raw, len);

			zassert_true(n <= (ssize_t)
					  LUDLC_COBS_ENCODED_MAX(len),
				     "len %u zeroes %u: %zd octets exceeds the "
				     "%zu the bound promises", len, zeroes, n,
				     (size_t)LUDLC_COBS_ENCODED_MAX(len));

			/* The whole point: no zero survives inside a frame. */
			for (ssize_t i = 0; i < n - 1; i++) {
				zassert_not_equal(enc[i], 0,
						  "len %u zeroes %u: a zero "
						  "survived at %zd",
						  len, zeroes, i);
			}

			m = unstuff(back, sizeof(back), enc, (size_t)n - 1);
			zassert_equal(m, len,
				      "len %u zeroes %u: got %zd back",
				      len, zeroes, m);
			zassert_mem_equal(back, raw, len,
					  "len %u zeroes %u: came back "
					  "different", len, zeroes);
		}
	}
}

/**
 * @brief The frame must not depend on how much room the transport had.
 *
 * The encoder writes into the transport's own buffer and resumes where it
 * stopped when that buffer fills. Carrying a group across such a break is
 * where a bug would show, so write the same frame in chunks of every size
 * that matters and compare against the whole.
 */
ZTEST(ludlc_cobs, test_encoded_in_pieces)
{
	static const size_t chunks[] = { 1, 2, 3, 253, 254, 255, 256, 509 };
	static uint8_t raw[1024], whole[2048], part[2048];
	size_t len = MIN(sizeof(raw), DATA_MAX);
	ssize_t n;

	for (unsigned int i = 0; i < len; i++) {
		/* Zeroes among the data, since those are what end a group. */
		raw[i] = (i % 17) ? (uint8_t)(1 + prng() % 255) : 0;
	}

	n = stuff(whole, sizeof(whole), raw, len);
	zassert_true(n > 0, "the reference frame did not fit, got %zd", n);

	for (unsigned int t = 0; t < ARRAY_SIZE(chunks); t++) {
		size_t at = chunks[t];
		ludlc_payload_size_t hdr_sz = (ludlc_payload_size_t)
						((len + 1) / 2);
		struct ludlc_framing_state *fs = ludlc_framing_create(
						&ludlc_framing_cobs);
		size_t out = 0;

		ludlc_enc_init(fs);
		ludlc_enc_prep(fs, raw, hdr_sz, raw + hdr_sz,
			      (ludlc_payload_size_t)(len - hdr_sz), false);

		while (ludlc_enc_busy(fs) && out < sizeof(part)) {
			size_t room = sizeof(part) - out;

			out += ludlc_enc_fill(fs, &none, part + out,
					      MIN(at, room));
		}

		ludlc_framing_destroy(fs);

		zassert_equal(out, (size_t)n,
			      "%zu octets at a time stuffed to %zu, wanted %zd",
			      at, out, n);
		zassert_mem_equal(part, whole, (size_t)n,
				  "%zu octets at a time built a different "
				  "frame", at);
	}
}

/**
 * @brief Encode one whole frame, checksum included, into @c frame.
 *
 * @return Octets the frame came to, delimiter included, or -1.
 */
static ssize_t encode(const void *hdr, ludlc_payload_size_t hdr_sz,
		      ludlc_payload_size_t pay_sz)
{
	struct ludlc_framing_state *fs = ludlc_framing_create(
						&ludlc_framing_cobs);
	size_t n;
	bool left;

	ludlc_enc_init(fs);
	ludlc_enc_prep(fs, hdr, hdr_sz, pay, pay_sz, false);

	n = ludlc_enc_fill(fs, &alg, frame, sizeof(frame));

	left = ludlc_enc_busy(fs);
	ludlc_framing_destroy(fs);

	return left ? -1 : (ssize_t)n;
}

/**
 * @brief Feed a whole frame in and say what came back.
 *
 * @return Octets of packet, checksum excluded, or -1 if none completed.
 */
static ssize_t decode(struct ludlc_connection *conn, ludlc_frame_size_t cap,
		      const uint8_t *buf, size_t len)
{
	struct ludlc_framing_state *fs = ludlc_framing_create(
						&ludlc_framing_cobs);
	ssize_t ret = -1;
	size_t i = 0;

	ludlc_dec_init(fs);
	ludlc_dec_prep(fs, &alg, got, cap);

	while (i < len) {
		bool done;

		i += ludlc_dec_feed(fs, &alg, conn, &buf[i], len - i, &done);
		if (done) {
			ret = (ssize_t)ludlc_dec_size(fs);
			break;
		}
	}

	ludlc_framing_destroy(fs);

	return ret;
}

/** @brief The whole frame path: checksum in, checksum verified out. */
ZTEST(ludlc_cobs, test_whole_frame)
{
	/* The real header rather than a few arbitrary octets, so the frame is
	 * one a link could carry. It contains a zero, which is the point of
	 * the stuffing.
	 */
	static const ludlc_packet_hdr_t hdr = {
		.id = { .tx_id = 0xde, .ack_id = 0x00 },
		.chan = 0xad,
	};
	struct ludlc_connection conn;
	size_t small;
	ssize_t n, m;

	memset(&conn, 0, sizeof(conn));
#if CONFIG_LUDLC_MULTI_CONN
	conn.cfg.max_payload = CONFIG_LUDLC_MAX_PAYLOAD_SIZE;
#endif

	/* Multiples of seven, so the payload has a zero in it somewhere. */
	for (unsigned int i = 0; i < sizeof(pay); i++) {
		pay[i] = (uint8_t)((i * 7) & 0xff);
	}

	n = encode(&hdr, sizeof(hdr), (ludlc_payload_size_t)sizeof(pay));
	zassert_true(n > 0, "frame encode refused, got %zd", n);
	if (n <= 0) {
		/* Unreachable: the assert above fails the test. Repeated for
		 * the compiler, which does not know that and would otherwise
		 * see frame[-2] below.
		 */
		return;
	}

	zassert_equal(frame[n - 1], LUDLC_COBS_DELIMITER,
		      "frame does not end on the delimiter");

	m = decode(&conn, sizeof(got), frame, (size_t)n);
	zassert_equal(m, (ssize_t)(sizeof(hdr) + sizeof(pay)),
		      "frame decode returned %zd", m);
	zassert_mem_equal(got, &hdr, sizeof(hdr),
			  "header came back different");
	zassert_mem_equal(got + sizeof(hdr), pay, sizeof(pay),
			  "payload came back different");

	/* A transfer that stops short of the delimiter has not delivered a
	 * frame: the transport keeps reading instead of passing up what it
	 * has so far.
	 */
	m = decode(&conn, sizeof(got), frame, (size_t)n - 1);
	zassert_true(m < 0, "a frame missing its delimiter decoded to %zd", m);

	/* A frame larger than the buffer is dropped rather than written past
	 * the end of it, which the guard octets below check.
	 */
	{
		ludlc_frame_size_t cap = (ludlc_frame_size_t)(sizeof(hdr) + 4);

		memset(got, 0xcd, sizeof(got));
		m = decode(&conn, cap, frame, (size_t)n);
		zassert_true(m < 0, "an oversized frame decoded to %zd", m);

		for (size_t i = cap; i < sizeof(got); i++) {
			zassert_equal(got[i], 0xcd,
				      "the decoder wrote %zu octets past its "
				      "buffer", i - cap + 1);
		}
	}

	/* One bit flipped anywhere in the frame must be caught, either by the
	 * framing (a flip that makes a zero) or by the checksum, depending on
	 * where it lands.
	 *
	 * Done on a short frame because a CRC only detects every single-bit
	 * error while the frame is shorter than the polynomial's period, a few
	 * kilobytes for CRC-16. A longer frame would alias and say nothing
	 * about the framing. What is under test is that the checksum covers
	 * the right octets.
	 */
	small = MIN(sizeof(pay), 16);

	n = encode(&hdr, sizeof(hdr), (ludlc_payload_size_t)small);
	zassert_true(n > 0, "short frame encode refused, got %zd", n);

	for (ssize_t i = 0; i < n - 1; i++) {
		uint8_t save = frame[i];

		frame[i] ^= 0x01;
		zassert_true(decode(&conn, sizeof(got), frame, (size_t)n) < 0,
			     "a bit flipped at %zd went unnoticed", i);
		frame[i] = save;
	}

	/* An exchange happens whenever either end has something to send, so
	 * the other end often has nothing: a transfer of pure padding is
	 * silence and must not be counted against the link.
	 */
	memset(&conn, 0, sizeof(conn));
#if CONFIG_LUDLC_MULTI_CONN
	conn.cfg.max_payload = CONFIG_LUDLC_MAX_PAYLOAD_SIZE;
#endif

	memset(frame, 0, sizeof(frame));
	zassert_true(decode(&conn, sizeof(got), frame, sizeof(frame)) < 0,
		     "an all-zero transfer decoded to something");

	if (IS_ENABLED(CONFIG_LUDLC_STATS)) {
		zassert_equal(conn.stats.dropped, 0,
			      "silence counted as %u dropped",
			      (unsigned int)conn.stats.dropped);
	}
}

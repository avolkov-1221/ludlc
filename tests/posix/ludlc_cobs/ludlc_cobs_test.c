// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only)
/**
 * @file ludlc_cobs_test.c
 *
 * @brief Checks the COBS framing in ludlc_cobs.c against what it promises.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * The framing is a state machine a transport drives from its RX and TX
 * callbacks, so it can be exercised on the build host with no link, transport
 * or peer. A mistake here is also expensive to find later, since a frame that
 * encodes wrong looks from the far end exactly like a bad cable.
 *
 * What is checked, in order:
 *
 *   - the published COBS vectors, so that a peer written from the standard
 *     rather than from this code interoperates;
 *   - the exact encoding where a group fills up next to a zero, which is the
 *     one place two COBS implementations differ without either looking wrong;
 *   - a round trip over every mix of zero and non-zero data, at the lengths
 *     where a run ends and a new group starts, which is where COBS fails if
 *     it fails at all;
 *   - that the frame comes out the same however the transport splits up the
 *     writing of it, the encoder buffering nothing between calls;
 *   - a whole frame, checksum included: that it survives the trip, that one
 *     bit flipped anywhere in it does not, that a frame too large for the
 *     buffer is dropped rather than written past it, and that an empty
 *     transfer is read as silence rather than counted against the link.
 *
 * This build carries COBS alone, so every call below reaches ludlc_cobs.c
 * directly with no ops table in between, as in a port that carries one
 * framing only.
 *
 * Built once per width of ludlc_payload_size_t, since the narrow ones promote
 * to int and hide sign and overflow mistakes that the wide ones do not, and
 * again at either end of what a payload may be: one build whose frames are
 * longer than its own size type can say, and one whose frames are a megabyte.
 * See the CMakeLists.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ludlc_private.h>
#include <ludlc_framing.h>

static int fails;

/**
 * @def DATA_MAX
 * @brief The most data these tests can push through the framing.
 *
 * A decoded packet size is reported in ludlc_payload_size_t, so a build with a
 * narrow one cannot report more than this however the frame was split on the
 * way in. Lengths above it are skipped on the narrow builds.
 */
#define DATA_MAX	((size_t)((ludlc_payload_size_t)-1))

/**
 * @brief A zero-width checksum, so that a frame is its data and nothing else.
 *
 * The vectors and boundaries below are about the stuffing, and a checksum on
 * the end would only add octets to stuff. The frame tests use a real one.
 */
static ludlc_csum_t sum_none(ludlc_csum_t c, uint8_t b)
{
	(void)b;

	return c;
}

static const struct ludlc_csum none = {
	.name = "none",
	.octets = 0,
	.csum = sum_none,
};

static void show(const char *tag, const uint8_t *b, ssize_t n)
{
	ssize_t i;

	printf("%s (%zd):", tag, n);
	for (i = 0; i < n; i++)
		printf(" %02x", b[i]);
	printf("\n");
}

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
static void vectors(void)
{
	static const struct {
		int len;
		uint8_t raw[8];
		int elen;
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
	unsigned int i;

	for (i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
		ssize_t n = stuff(out, sizeof(out), v[i].raw,
				  (size_t)v[i].len);
		ssize_t m;

		/* The encoder ends the frame itself: one octet more. */
		if (n != v[i].elen + 1 ||
		    memcmp(out, v[i].enc, (size_t)v[i].elen)) {
			printf("FAIL vector %u encode\n", i);
			show("  want", v[i].enc, v[i].elen);
			show("  got ", out, n);
			fails++;
			continue;
		}

		if (out[n - 1] != LUDLC_COBS_DELIMITER) {
			printf("FAIL vector %u did not end on the delimiter\n",
			       i);
			fails++;
			continue;
		}

		m = unstuff(back, sizeof(back), out, (size_t)n - 1);
		if (m != v[i].len || memcmp(back, v[i].raw, (size_t)m)) {
			printf("FAIL vector %u decode\n", i);
			show("  want", v[i].raw, v[i].len);
			show("  got ", back, m < 0 ? 0 : m);
			fails++;
		}
	}
}

/**
 * @brief What @p len octets with no zero among them encode to, exactly.
 *
 * One code octet per group, groups of at most LUDLC_COBS_BLOCK_MAX octets, and
 * no empty group after one ending flush with the data. This is the exact
 * length rather than a bound, and the one a peer written from the standard
 * arrives at. Pinned here because a spare octet would still decode and so go
 * unnoticed. The delimiter is not counted.
 */
static size_t packed_len(size_t len)
{
	if (!len)
		return 1;

	return len + (len + LUDLC_COBS_BLOCK_MAX - 1) / LUDLC_COBS_BLOCK_MAX;
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
 * gives a different length. Written out rather than derived, and repeated in
 * tests/zephyr/ludlc_cobs, so the two suites cannot drift apart here while
 * both still round-trip against themselves.
 */
static void zero_boundaries(void)
{
	static const struct {
		const char	*what;
		/**< Non-zero octets before the zero. */
		size_t		pre;
		/**< Non-zero octets after it. */
		size_t		post;
		/**< Octets the encoding must come to, delimiter excluded. */
		size_t		elen;
	} zb[] = {
		{ "a full group then a zero",      254, 0,   257 },
		{ "a group one short then a zero", 253, 0,   255 },
		{ "a zero then a full group",      0,   254, 256 },
		{ "a full group either side",      254, 254, 511 },
	};
	static uint8_t raw[1024], enc[2048], back[1024];
	unsigned int t;
	size_t i;

	for (t = 0; t < sizeof(zb) / sizeof(zb[0]); t++) {
		size_t len = zb[t].pre + 1 + zb[t].post;
		ssize_t n, m;

		/* A narrow size type cannot report a packet this large. */
		if (len > DATA_MAX)
			continue;

		for (i = 0; i < zb[t].pre; i++)
			raw[i] = 0xa5;

		raw[zb[t].pre] = 0;

		for (i = 0; i < zb[t].post; i++)
			raw[zb[t].pre + 1 + i] = 0x5a;

		n = stuff(enc, sizeof(enc), raw, len);
		if (n != (ssize_t)zb[t].elen + 1) {
			printf("FAIL %s: encoded to %zd octets, wanted %zu\n",
			       zb[t].what, n, zb[t].elen + 1);
			fails++;
			continue;
		}

		m = unstuff(back, sizeof(back), enc, (size_t)n - 1);
		if (m != (ssize_t)len || memcmp(back, raw, len)) {
			printf("FAIL %s: got %zd back, wanted %zu\n",
			       zb[t].what, m, len);
			fails++;
		}
	}
}

/**
 * @brief Round trip everything, with the run-length boundaries well covered.
 *
 * A group holds at most LUDLC_COBS_BLOCK_MAX octets, so the lengths either side
 * of that and of twice it are where the encoder decides whether another group
 * follows. That is the one decision it can get wrong unnoticed.
 */
static void roundtrip(void)
{
	static const size_t lens[] = {
		1, 2, 253, 254, 255, 256, 507, 508, 509, 1000
	};
	static uint8_t raw[1024], enc[2048], back[1024];
	unsigned int t;
	size_t i;

	for (t = 0; t < sizeof(lens) / sizeof(lens[0]); t++) {
		size_t len = lens[t];
		int zeroes;

		if (len > DATA_MAX)
			continue;

		/* All non-zero, then ever more zero-ridden, then all zero. */
		for (zeroes = 0; zeroes <= 8; zeroes++) {
			ssize_t n, m;

			for (i = 0; i < len; i++) {
				if (zeroes == 8)
					raw[i] = 0;
				else if (zeroes && (rand() % 8) < zeroes)
					raw[i] = 0;
				else
					raw[i] = (uint8_t)(1 + rand() % 255);
			}

			n = stuff(enc, sizeof(enc), raw, len);

			if (n > (ssize_t)LUDLC_COBS_ENCODED_MAX(len)) {
				printf("FAIL len %zu zeroes %d: %zd octets "
				       "exceeds the %zu the bound promises\n",
				       len, zeroes, n,
				       (size_t)LUDLC_COBS_ENCODED_MAX(len));
				fails++;
			}

			if (zeroes == 0 &&
			    n != (ssize_t)packed_len(len) + 1) {
				printf("FAIL len %zu: encoded to %zd octets, "
				       "wanted %zu\n",
				       len, n, packed_len(len) + 1);
				fails++;
			}

			/* The whole point: no zero survives inside a frame. */
			for (i = 0; i + 1 < (size_t)n; i++) {
				if (enc[i] == 0) {
					printf("FAIL len %zu zeroes %d: a zero "
					       "survived at %zu\n",
					       len, zeroes, i);
					fails++;
					break;
				}
			}

			m = unstuff(back, sizeof(back), enc, (size_t)n - 1);
			if (m != (ssize_t)len || memcmp(back, raw, len)) {
				printf("FAIL len %zu zeroes %d: got %zd back\n",
				       len, zeroes, m);
				fails++;
			}
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
static void pieces(void)
{
	static const size_t chunks[] = { 1, 2, 3, 253, 254, 255, 256, 509 };
	static uint8_t raw[1024], whole[2048], part[2048];
	size_t len = sizeof(raw) > DATA_MAX ? DATA_MAX : sizeof(raw);
	unsigned int t;
	size_t i;
	ssize_t n;

	for (i = 0; i < len; i++) {
		/* Zeroes among the data, since those are what end a group. */
		raw[i] = (i % 17) ? (uint8_t)(1 + rand() % 255) : 0;
	}

	n = stuff(whole, sizeof(whole), raw, len);
	if (n < 0) {
		printf("FAIL the reference frame did not fit\n");
		fails++;
		return;
	}

	for (t = 0; t < sizeof(chunks) / sizeof(chunks[0]); t++) {
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
					      at < room ? at : room);
		}

		ludlc_framing_destroy(fs);

		if (out != (size_t)n || memcmp(whole, part, out)) {
			printf("FAIL %zu octets at a time built a different "
			       "frame: %zu octets against %zd\n", at, out, n);
			fails++;
		}
	}
}

/**
 * @brief A checksum to frame against: CRC-8/ATM, seeded zero, residual zero.
 *
 * Written out here rather than taken from a library, since what is under test
 * is the framing around a checksum and not the checksum itself.
 */
static ludlc_csum_t sum8(ludlc_csum_t c, uint8_t b)
{
	int i;

	c ^= b;
	for (i = 0; i < 8; i++)
		c = (c & 0x80) ? (ludlc_csum_t)(((c << 1) ^ 0x07) & 0xff)
			       : (ludlc_csum_t)((c << 1) & 0xff);

	return c;
}

/*
 * Static rather than on the stack: a build whose payload is a megabyte would
 * otherwise need three of them there, which is the cost this framing exists
 * to avoid.
 */
static uint8_t pay[CONFIG_LUDLC_MAX_PAYLOAD_SIZE];
static uint8_t frame[LUDLC_COBS_FRAME_MAX(LUDLC_MAX_CSUM_SIZE)];
static uint8_t got[LUDLC_FRAME_BUF_SIZE];

static const struct ludlc_csum alg = {
	.name = "crc8",
	.octets = 1,
	.csum = sum8,
};

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
static void frames(void)
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
	unsigned int i;
	ssize_t n, m;

	memset(&conn, 0, sizeof(conn));
#if CONFIG_LUDLC_MULTI_CONN
	conn.cfg.max_payload = CONFIG_LUDLC_MAX_PAYLOAD_SIZE;
#endif

	/* Multiples of seven, so the payload has a zero in it somewhere. */
	for (i = 0; i < sizeof(pay); i++)
		pay[i] = (uint8_t)((i * 7) & 0xff);

	n = encode(&hdr, sizeof(hdr), (ludlc_payload_size_t)sizeof(pay));
	if (n < 0) {
		printf("FAIL frame encode refused\n");
		fails++;
		return;
	}

	if (frame[n - 1] != LUDLC_COBS_DELIMITER) {
		printf("FAIL frame does not end on the delimiter\n");
		fails++;
	}

	m = decode(&conn, sizeof(got), frame, (size_t)n);
	if (m != (ssize_t)(sizeof(hdr) + sizeof(pay))) {
		printf("FAIL frame decode returned %zd, wanted %zu\n",
		       m, sizeof(hdr) + sizeof(pay));
		fails++;
		return;
	}

	if (memcmp(got, &hdr, sizeof(hdr)) ||
	    memcmp(got + sizeof(hdr), pay, sizeof(pay))) {
		printf("FAIL frame came back different\n");
		fails++;
	}

	/* A transfer that stops short of the delimiter has not delivered a
	 * frame: the transport keeps reading instead of passing up what it
	 * has so far.
	 */
	m = decode(&conn, sizeof(got), frame, (size_t)n - 1);
	if (m >= 0) {
		printf("FAIL a frame missing its delimiter decoded to %zd\n",
		       m);
		fails++;
	}

	/* A frame larger than the buffer is dropped rather than written past
	 * the end of it, which the guard octets below check.
	 */
	{
		ludlc_frame_size_t cap = (ludlc_frame_size_t)(sizeof(hdr) + 4);

		memset(got, 0xcd, sizeof(got));
		m = decode(&conn, cap, frame, (size_t)n);
		if (m >= 0) {
			printf("FAIL an oversized frame decoded to %zd\n", m);
			fails++;
		}

		for (i = cap; i < sizeof(got); i++) {
			if (got[i] != 0xcd) {
				printf("FAIL the decoder wrote %zu octets "
				       "past its buffer\n",
				       (size_t)(i - cap) + 1);
				fails++;
				break;
			}
		}
	}

	/* One bit flipped anywhere in the frame must be caught, either by the
	 * framing (a flip that makes a zero) or by the checksum, depending on
	 * where it lands.
	 *
	 * Done on a short frame because a CRC only detects every single-bit
	 * error while the frame is shorter than the polynomial's period, a few
	 * dozen octets for one octet of CRC. A longer frame would alias and
	 * say nothing about the framing. What is under test is that the
	 * checksum covers the right octets.
	 */
	small = sizeof(pay) < 16 ? sizeof(pay) : 16;

	n = encode(&hdr, sizeof(hdr), (ludlc_payload_size_t)small);
	if (n < 0) {
		printf("FAIL short frame encode refused\n");
		fails++;
		return;
	}

	for (i = 0; i < (unsigned int)n - 1; i++) {
		uint8_t save = frame[i];

		frame[i] ^= 0x01;
		m = decode(&conn, sizeof(got), frame, (size_t)n);
		if (m >= 0) {
			printf("FAIL bit flip at %u went unnoticed\n", i);
			fails++;
		}
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
	m = decode(&conn, sizeof(got), frame, sizeof(frame));
	if (m >= 0) {
		printf("FAIL an all-zero transfer decoded to %zd\n", m);
		fails++;
	}
#if CONFIG_LUDLC_STATS
	if (conn.stats.dropped) {
		printf("FAIL silence counted as %u dropped\n",
		       conn.stats.dropped);
		fails++;
	}
#endif
}

int main(void)
{
	/* Fixed, so that a failure can be reproduced from the report alone. */
	srand(12345);

	printf("ludlc cobs: payload %u octets in %zu bits, frame %zu octets\n",
	       (unsigned int)CONFIG_LUDLC_MAX_PAYLOAD_SIZE,
	       sizeof(ludlc_payload_size_t) * 8,
	       (size_t)LUDLC_COBS_FRAME_MAX(LUDLC_MAX_CSUM_SIZE));

	vectors();
	zero_boundaries();
	roundtrip();
	pieces();
	frames();

	printf(fails ? "%d FAILURES\n" : "all passed\n", fails);

	return fails ? EXIT_FAILURE : EXIT_SUCCESS;
}

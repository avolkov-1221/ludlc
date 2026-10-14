// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file ludlc_framing/src/main.c
 *
 * @brief Runs the framings in the environment a board builds them in.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * The same codecs tests/posix/ludlc_framing covers, built here against
 * Zephyr's own CRC and whatever widths the module's Kconfig settles on, and
 * run on the scheduler rather than on a host. A framing is header-only and
 * has no RTOS in it, so what this adds is the build and the board, not
 * another implementation.
 *
 * Which framing a link runs is settled when the link is opened, not by the
 * build: one image may carry a UART speaking the escaping and another
 * speaking the stuffing. Nothing here asks for a framing in prj.conf, and
 * every framing this build carries is exercised from the one binary.
 *
 * What is checked, in order:
 *
 *   - that the octets the stuffing puts out an octet at a time are the octets
 *     ludlc_cobs_encode() writes in one go, which tests/zephyr/ludlc_cobs
 *     pins to the published vectors: pulling a frame out is a way of sending
 *     it, not another wire format;
 *   - that a packet written in comes back out, whichever framing carried it;
 *   - that a transport which stops pulling part way and comes back builds
 *     exactly the same frame;
 *   - that a frame cut short is never handed over, and one with nowhere to
 *     land is counted rather than written past the buffer;
 *   - that the zero padding an SPI transfer ends with is not another frame;
 *   - that links running different framings side by side do not disturb one
 *     another.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

#include <ludlc_private.h>
#include <ludlc_framing.h>

ZTEST_SUITE(ludlc_framing, NULL, NULL, NULL, NULL, NULL);

/** @brief The checksum the other suites frame against: CRC-8/ATM. */
static ludlc_csum_t sum8(ludlc_csum_t c, uint8_t b)
{
	uint8_t v = (uint8_t)c;

	return crc8((const uint8_t *)&b, 1, 0x07, v, false);
}

static const struct ludlc_csum alg = {
	.name = "crc8",
	.octets = 1,
	.csum = sum8,
};

/**
 * @brief A link as ludlc_connection_init() would leave it.
 *
 * The framings are driven directly here. The only setting they take from the
 * link is the payload it carries.
 */
static void conn_reset(struct ludlc_connection *conn)
{
	memset(conn, 0, sizeof(*conn));
#if CONFIG_LUDLC_MULTI_CONN
	conn->cfg.max_payload = CONFIG_LUDLC_MAX_PAYLOAD_SIZE;
#endif
}

static const ludlc_packet_hdr_t hdr = {
	.id = { .tx_id = 0xde, .ack_id = 0x00 },
	.chan = 0xad,
};

static const struct ludlc_framing *const all[] = {
	&ludlc_framing_hdlc,
	&ludlc_framing_cobs,
	&ludlc_framing_raw,
};

/*
 * Static, as in the other suites: nothing here gathers a frame, and a build
 * whose payload is large would otherwise want several of these on a stack.
 */
static uint8_t pay[CONFIG_LUDLC_MAX_PAYLOAD_SIZE];
static uint8_t wire[LUDLC_COBS_FRAME_MAX(LUDLC_MAX_CSUM_SIZE) + 8];
static uint8_t other[LUDLC_COBS_FRAME_MAX(LUDLC_MAX_CSUM_SIZE) + 8];
static uint8_t got[LUDLC_FRAME_BUF_SIZE];
static struct ludlc_connection conn;

/**
 * @brief Room for one frame of the side by side packet, whatever wraps it.
 *
 * The stuffing adds a code octet per group and a delimiter, and the escaping
 * may double every octet it sends; twice the frame and a little over covers
 * either.
 */
#define SIDE_MAX	(2 * (300 + 32) + 8)

/** @brief What the decoder handed over, if it handed anything over. */
struct rx {
	size_t size;
	unsigned int count;
};

static void on_packet(struct rx *r, size_t size)
{
	r->size = size;
	r->count++;
}

static void fill_payload(void)
{
	size_t i;

	/* Multiples of seven, so there are zero octets to stuff around. */
	for (i = 0; i < sizeof(pay); i++) {
		pay[i] = (uint8_t)((i * 7) & 0xff);
	}
}

/**
 * @brief Prime @p enc with one packet of @p pay_sz octets.
 *
 * The run ends with one more boundary, which is what a transport asks for
 * when nothing follows the frame.
 */
static void encoder_load(struct ludlc_framing_state *fs, size_t pay_sz)
{
	ludlc_enc_init(fs);
	ludlc_enc_prep(fs, &hdr, sizeof(hdr), pay,
		      (ludlc_payload_size_t)pay_sz, true);
}

/**
 * @brief Pull one whole frame out of @p f into @p buf.
 *
 * @return Octets the frame came to, or -ENOSPC if it outgrew @p cap.
 */
static ssize_t encode(const struct ludlc_framing *f, size_t pay_sz,
		      uint8_t *buf, size_t cap)
{
	struct ludlc_framing_state *fs = ludlc_framing_create(f);
	size_t len = 0;
	bool left;

	encoder_load(fs, pay_sz);

	len = ludlc_enc_fill(fs, &alg, buf, cap);

	/* Still busy means the frame did not fit in @p cap. */
	left = ludlc_enc_busy(fs);
	ludlc_framing_destroy(fs);

	return left ? -ENOSPC : (ssize_t)len;
}

/**
 * @brief Feed @p len octets of @p frame in, into a buffer of @p cap.
 *
 * As a transport would: a framing without a delimiter is given the announced
 * length as its capacity, one longer than @p cap is refused before anything
 * is fed, and what follows the frame is padding.
 */
static void decode(const struct ludlc_framing *f, const uint8_t *frame,
		   size_t len, size_t announced, size_t cap, struct rx *r)
{
	struct ludlc_framing_state *fs;
	size_t i;

	if (f->needs_wire_len) {
		if (announced > cap) {
			LUDLC_INC_STATS(&conn, dropped);
			return;
		}
		cap = announced;
	}

	fs = ludlc_framing_create(f);
	ludlc_dec_init(fs);
	ludlc_dec_prep(fs, &alg, got, (ludlc_frame_size_t)cap);

	for (i = 0; i < len;) {
		bool done;

		i += ludlc_dec_feed(fs, &alg, &conn, &frame[i], len - i,
				    &done);
		if (!done) {
			continue;
		}

		on_packet(r, ludlc_dec_size(fs));
		ludlc_dec_prep(fs, &alg, got,
			       f->needs_wire_len ? 0 : (ludlc_frame_size_t)cap);
	}

	ludlc_framing_destroy(fs);
}

/**
 * @brief What the stuffing announces is what it then sends.
 *
 * A transport that must state a frame's length before building it calls
 * wire_len(), which counts the stuffing without performing it. That is a
 * second implementation of the same rules, written as a walk over the data
 * rather than a state machine, and the two must agree exactly. The lengths
 * below fall on a group boundary, which is where they would not.
 */
ZTEST(ludlc_framing, test_cobs_sends_what_it_announced)
{
	static const size_t lens[] = { 0, 1, 7, 253, 254, 255, 256, 509, 510 };
	unsigned int t;

	fill_payload();

	for (t = 0; t < ARRAY_SIZE(lens); t++) {
		ssize_t len, said;

		if (lens[t] > sizeof(pay)) {
			continue;
		}

		len = encode(&ludlc_framing_cobs, lens[t], wire, sizeof(wire));
		zassert_true(len > 0, "%zu octets did not encode: %zd",
			     lens[t], len);

		zassert_equal(wire[len - 1], LUDLC_COBS_DELIMITER,
			      "%zu octets: the frame does not end on the "
			      "delimiter", lens[t]);

		/* One code octet per 253 data octets, so the announcement
		 * covers the frame and the transfer may pad the rest.
		 */
		said = ludlc_framing_cobs.wire_len(&alg, &hdr, sizeof(hdr),
						   pay,
						   (ludlc_payload_size_t)
							lens[t]);
		zassert_true(said >= len,
			     "%zu octets announced as %zd, sent as %zd",
			     lens[t], said, len);
	}
}

/** @brief A packet in and the same packet out, for each framing. */
ZTEST(ludlc_framing, test_roundtrip)
{
	static const size_t lens[] = { 0, 1, 253, 254, 255, 512 };
	unsigned int i, t;

	fill_payload();

	for (i = 0; i < ARRAY_SIZE(all); i++) {
		for (t = 0; t < ARRAY_SIZE(lens); t++) {
			const struct ludlc_framing *f = all[i];
			struct rx r = { 0 };
			ssize_t len;

			if (lens[t] > sizeof(pay)) {
				continue;
			}

			conn_reset(&conn);

			len = encode(f, lens[t], wire, sizeof(wire));
			zassert_true(len > 0, "%s: %zu octets: %zd", f->name,
				     lens[t], len);

			decode(f, wire, (size_t)len, (size_t)len, sizeof(got),
			       &r);

			zassert_equal(r.count, 1, "%s: %zu octets: %u packets",
				      f->name, lens[t], r.count);
			zassert_equal(r.size, sizeof(hdr) + lens[t],
				      "%s: %zu octets came back as %zu",
				      f->name, lens[t], r.size);
			zassert_mem_equal(got, &hdr, sizeof(hdr),
					  "%s: the header changed", f->name);
			zassert_mem_equal(got + sizeof(hdr), pay, lens[t],
					  "%s: the payload changed", f->name);
		}
	}
}

/**
 * @brief A transport that stops pulling part way, 16 octets at a time.
 *
 * A fifo fills up at any time, so the encoder must keep the whole frame state
 * between one pull and the next. The octets pulled in chunks must match the
 * ones pulled in a single call. Once the frame is out, the encoder releases
 * the packet instead of offering it again.
 */
ZTEST(ludlc_framing, test_paused_transport)
{
	const size_t want = MIN(sizeof(pay), 600);
	unsigned int i;

	fill_payload();

	for (i = 0; i < ARRAY_SIZE(all); i++) {
		const struct ludlc_framing *f = all[i];
		struct ludlc_framing_state *fs;
		size_t drained = 0;
		ssize_t len;

		len = encode(f, want, wire, sizeof(wire));
		zassert_true(len > 0, "%s: %zd", f->name, len);

		fs = ludlc_framing_create(f);
		encoder_load(fs, want);

		while (ludlc_enc_busy(fs)) {
			zassert_true(drained + 16 <= sizeof(other),
				     "%s: took more than the frame", f->name);

			drained += ludlc_enc_fill(fs, &alg, other + drained,
						  16);
		}

		zassert_equal(drained, (size_t)len,
			      "%s: %zu octets against %zd", f->name, drained,
			      len);
		zassert_mem_equal(wire, other, drained,
				  "%s: a different wire form", f->name);

		/* The frame is out. A further call must not restart it. */
		zassert_equal(ludlc_enc_fill(fs, &alg, other,
					     sizeof(other)), 0,
			      "%s: the encoder took the frame up again",
			      f->name);
		zassert_false(ludlc_enc_busy(fs),
			      "%s: the encoder took the frame up again",
			      f->name);

		ludlc_framing_destroy(fs);
	}
}

/**
 * @brief Every framing at once, on independent links.
 *
 * The framing belongs to the link, so an image may carry one of each. The
 * encoders run in lockstep, one octet each per turn, and every octet goes
 * straight into the decoder of the link it came from. Anything shared
 * between them shows up as a wrong frame on a link that never touched it.
 */
ZTEST(ludlc_framing, test_side_by_side)
{
	const size_t want = MIN(sizeof(pay), 300);
	static uint8_t alone[ARRAY_SIZE(all)][SIDE_MAX];
	static uint8_t pkt[ARRAY_SIZE(all)][LUDLC_FRAME_BUF_SIZE];
	static struct ludlc_connection link[ARRAY_SIZE(all)];
	struct ludlc_framing_state *fs[ARRAY_SIZE(all)];
	struct rx r[ARRAY_SIZE(all)];
	size_t len[ARRAY_SIZE(all)];
	size_t sent[ARRAY_SIZE(all)];
	unsigned int i;
	bool any;

	fill_payload();

	for (i = 0; i < ARRAY_SIZE(all); i++) {
		ssize_t one = encode(all[i], want, alone[i], sizeof(alone[i]));

		zassert_true(one > 0, "%s: %zd", all[i]->name, one);

		len[i] = (size_t)one;
		sent[i] = 0;

		conn_reset(&link[i]);
		memset(&r[i], 0, sizeof(r[i]));

		fs[i] = ludlc_framing_create(all[i]);
		encoder_load(fs[i], want);
		ludlc_dec_init(fs[i]);
		ludlc_dec_prep(fs[i], &alg, pkt[i],
			       (ludlc_frame_size_t)(all[i]->needs_wire_len ?
						    len[i] : sizeof(pkt[i])));
	}

	do {
		any = false;

		for (i = 0; i < ARRAY_SIZE(all); i++) {
			uint8_t c;
			bool done;

			if (!ludlc_enc_busy(fs[i])) {
				continue;
			}

			any = true;
			ludlc_enc_fill(fs[i], &alg, &c, 1);

			zassert_true(sent[i] < len[i],
				     "%s: interleaved, the frame outgrew the "
				     "%zu octets it takes alone",
				     all[i]->name, len[i]);
			zassert_equal(c, alone[i][sent[i]],
				      "%s: interleaved, octet %zu differs",
				      all[i]->name, sent[i]);
			sent[i]++;

			ludlc_dec_feed(fs[i], &alg, &link[i], &c, 1, &done);
			if (done) {
				on_packet(&r[i], ludlc_dec_size(fs[i]));
			}
		}
	} while (any);

	for (i = 0; i < ARRAY_SIZE(all); i++) {
		zassert_equal(sent[i], len[i],
			      "%s: interleaved, %zu octets against %zu alone",
			      all[i]->name, sent[i], len[i]);
		zassert_equal(r[i].count, 1, "%s: interleaved, %u packets",
			      all[i]->name, r[i].count);
		zassert_equal(r[i].size, sizeof(hdr) + want,
			      "%s: interleaved, came back as %zu",
			      all[i]->name, r[i].size);
		zassert_mem_equal(pkt[i], &hdr, sizeof(hdr),
				  "%s: interleaved, the header changed",
				  all[i]->name);
		zassert_mem_equal(pkt[i] + sizeof(hdr), pay, want,
				  "%s: interleaved, the payload changed",
				  all[i]->name);
	}

	for (i = 0; i < ARRAY_SIZE(all); i++) {
		ludlc_framing_destroy(fs[i]);
	}
}

/** @brief A frame that stopped part way is not a frame. */
ZTEST(ludlc_framing, test_truncated)
{
	const size_t want = MIN(sizeof(pay), 300);
	unsigned int i;

	fill_payload();

	for (i = 0; i < ARRAY_SIZE(all); i++) {
		const struct ludlc_framing *f = all[i];
		ssize_t len;
		size_t at;

		conn_reset(&conn);

		len = encode(f, want, wire, sizeof(wire));
		zassert_true(len > 0, "%s: %zd", f->name, len);

		for (at = 1; at < (size_t)len; at++) {
			struct rx r = { 0 };

			decode(f, wire, at, (size_t)len, sizeof(got), &r);
			zassert_equal(r.count, 0,
				      "%s: %zu octets of %zd completed",
				      f->name, at, len);
		}
	}
}

/** @brief A frame with nowhere to land is counted, not written. */
ZTEST(ludlc_framing, test_jabber)
{
	const size_t want = MIN(sizeof(pay), 64);
	unsigned int i;

	fill_payload();

	for (i = 0; i < ARRAY_SIZE(all); i++) {
		const struct ludlc_framing *f = all[i];
		struct rx r = { 0 };
		ssize_t len;

		/* Raw has no delimiter to overrun: the transport refuses a
		 * length longer than its buffer before the decoder is armed.
		 */
		if (f->needs_wire_len) {
			continue;
		}

		conn_reset(&conn);

		len = encode(f, want, wire, sizeof(wire));
		zassert_true(len > 0, "%s: %zd", f->name, len);

		decode(f, wire, (size_t)len, (size_t)len,
		       sizeof(hdr) + want / 2, &r);

		zassert_equal(r.count, 0, "%s: a frame past the buffer was "
			      "accepted", f->name);

		if (IS_ENABLED(CONFIG_LUDLC_STATS)) {
			zassert_true(conn.stats.jabber > 0,
				     "%s: and not counted", f->name);
		}
	}
}

/**
 * @brief The zeros an SPI transfer pads with are not another frame.
 *
 * The stuffed form ends with a zero and the padding is more of them, so a
 * decoder sees a run of frame ends with nothing between them. One packet is
 * the right answer; two is a link acknowledging something nobody sent.
 */
ZTEST(ludlc_framing, test_padding_is_not_a_frame)
{
	const size_t want = MIN(sizeof(pay), 32);
	struct rx r = { 0 };
	ssize_t len;
	size_t pad;

	fill_payload();
	conn_reset(&conn);

	len = encode(&ludlc_framing_cobs, want, wire, sizeof(wire));
	zassert_true(len > 0, "%zd", len);

	pad = MIN(sizeof(wire) - (size_t)len, 32);
	memset(wire + len, LUDLC_COBS_DELIMITER, pad);

	decode(&ludlc_framing_cobs, wire, (size_t)len + pad, (size_t)len,
	       sizeof(got), &r);

	zassert_equal(r.count, 1, "%zu octets of padding gave %u packets", pad,
		      r.count);
	zassert_equal(r.size, sizeof(hdr) + want, "%zu octets came back",
		      r.size);
}

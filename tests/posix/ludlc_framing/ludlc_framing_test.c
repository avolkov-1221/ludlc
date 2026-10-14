// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file ludlc_framing_test.c
 *
 * @brief Checks the three framings against what a transport is promised.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * A framing hands out one encoded octet per call and takes one received octet
 * per call, and neither side gathers a frame: a link carrying a megabyte holds
 * no more than one carrying eight octets. Everything a transport is entitled
 * to assume about that is what this checks:
 *
 *   - what each framing says about itself, since a transport picks one on
 *     those two flags alone;
 *   - that a packet pulled out and fed back in arrives as it went in,
 *     whichever framing carried it and whatever the payload was;
 *   - that a transport which stops pulling part way through a frame and comes
 *     back later builds the same wire form, and that one which keeps pulling
 *     past the end gets idle octets rather than the same frame again;
 *   - that the raw wire form is the frame - the header, the payload and the
 *     checksum, nothing between them - which is what a peer written from
 *     ludlc_spi_proto.h will send, and that octets past the announced length
 *     are the transport's padding rather than the frame's;
 *   - that a flipped bit, a transfer cut short and a frame with nowhere to
 *     land are all refused, and counted where the link keeps counts;
 *   - that the same packet through each framing arrives identical, the
 *     choice being invisible above the callbacks.
 *
 * Built once per width of ludlc_payload_size_t, as tests/posix/ludlc_cobs is
 * and for the same reason: the encoder adds a payload size to a header size
 * and a checksum width, and only the narrow builds show what that promotes
 * to. See the CMakeLists.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ludlc_private.h>
#include <ludlc_framing.h>
#include <ludlc_spi_proto.h>

static int fails;

/**
 * @brief A checksum to frame against: CRC-8/ATM, seeded zero, residual zero.
 *
 * The same one tests/posix/ludlc_cobs frames against, and written out here
 * for the same reason: what is being tested is the framing around a checksum
 * and not the checksum.
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

/** @brief The header a link would send, with a zero octet in it. */
static const ludlc_packet_hdr_t hdr = {
	.id = { .tx_id = 0xde, .ack_id = 0x00 },
	.chan = 0xad,
};

/** @brief Every framing a link may run, in the order they were written. */
static const struct ludlc_framing *const all[] = {
	&ludlc_framing_hdlc,
	&ludlc_framing_cobs,
	&ludlc_framing_raw,
};

/*
 * Static rather than on the stack: a build whose payload is a megabyte would
 * need several of these there, and nothing in the framing gathers a frame
 * anyway.
 */
static uint8_t pay[CONFIG_LUDLC_MAX_PAYLOAD_SIZE];
static uint8_t wire[LUDLC_COBS_FRAME_MAX(LUDLC_MAX_CSUM_SIZE) * 2];
static uint8_t other[LUDLC_COBS_FRAME_MAX(LUDLC_MAX_CSUM_SIZE) * 2];
static uint8_t got[LUDLC_FRAME_BUF_SIZE];

/** @brief Enough for the packets the comparisons send, and no more. */
#define KEPT_MAX	512

/** @brief The packet the links in side_by_side() carry, at most. */
#define SIDE_PAYLOAD	300

/**
 * @brief Room for one such frame, whatever wraps it.
 *
 * The stuffing adds a code octet per group and a delimiter, and the escaping
 * may double every octet it sends; twice the frame and a little over covers
 * either.
 */
#define SIDE_MAX	(2 * (SIDE_PAYLOAD + 32) + 8)

/**
 * @struct rx
 * @brief What the decoder handed over, if it handed anything over.
 */
struct rx {
	size_t size;
	unsigned int count;
	/**< Where to keep the packet, for a caller comparing two of them. */
	uint8_t *kept;
};

/** @brief The decoder completed a frame: what a transport does about it. */
static void on_packet(struct rx *r, const void *pkt, size_t size)
{
	r->size = size;
	r->count++;

	if (r->kept && size <= KEPT_MAX)
		memcpy(r->kept, pkt, size);
}

/**
 * @brief Prime @p enc with one packet of @p pay_sz octets.
 *
 * The run ends with one more boundary, which is what a transport asks for
 * when nothing follows the frame.
 */
static void encoder_load(struct ludlc_framing_state *fs,
			 ludlc_payload_size_t pay_sz)
{
	ludlc_enc_init(fs);
	ludlc_enc_prep(fs, &hdr, sizeof(hdr), pay, pay_sz, true);
}

/**
 * @brief Pull one whole frame out of @p f into @p buf.
 *
 * @return Octets the frame came to, or -1 if it outgrew @p cap.
 */
static ssize_t encode(const struct ludlc_framing *f,
		      ludlc_payload_size_t pay_sz, uint8_t *buf, size_t cap)
{
	struct ludlc_framing_state *fs = ludlc_framing_create(f);
	size_t len = 0;
	bool left;

	encoder_load(fs, pay_sz);

	len = ludlc_enc_fill(fs, &alg, buf, cap);

	/* Still busy means the frame did not fit in @p cap. */
	left = ludlc_enc_busy(fs);
	ludlc_framing_destroy(fs);

	return left ? -1 : (ssize_t)len;
}

/**
 * @brief Feed @p len octets of @p frame in and say what came back.
 *
 * As a transport would: a framing without a delimiter is given the announced
 * length as its capacity, one longer than @p cap is refused before anything
 * is fed, and what follows the frame is padding.
 */
static void decode(const struct ludlc_framing *f, const uint8_t *frame,
		   size_t len, size_t announced, size_t cap,
		   struct ludlc_connection *conn, struct rx *r)
{
	struct ludlc_framing_state *fs;
	size_t i;

	if (f->needs_wire_len) {
		if (announced > cap) {
			LUDLC_INC_STATS(conn, dropped);
			return;
		}
		cap = announced;
	}

	fs = ludlc_framing_create(f);
	ludlc_dec_init(fs);
	ludlc_dec_prep(fs, &alg, got, (ludlc_frame_size_t)cap);

	for (i = 0; i < len;) {
		bool done;

		i += ludlc_dec_feed(fs, &alg, conn, &frame[i], len - i,
				    &done);
		if (!done)
			continue;

		on_packet(r, got, ludlc_dec_size(fs));
		ludlc_dec_prep(fs, &alg, got,
			       f->needs_wire_len ? 0 : (ludlc_frame_size_t)cap);
	}

	ludlc_framing_destroy(fs);
}

/** @brief What a transport asks the framings about themselves. */
static void flags(void)
{
	unsigned int i;

	if (!ludlc_framing_raw.wire_len ||
	    !ludlc_framing_raw.needs_wire_len) {
		printf("FAIL raw must announce its length and be told one\n");
		fails++;
	}

	if (!ludlc_framing_cobs.wire_len ||
	    ludlc_framing_cobs.needs_wire_len) {
		printf("FAIL cobs must announce its length and delimit\n");
		fails++;
	}

	if (ludlc_framing_hdlc.wire_len ||
	    ludlc_framing_hdlc.needs_wire_len) {
		printf("FAIL hdlc must do neither\n");
		fails++;
	}

	for (i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		if (ludlc_framing_lookup(all[i]->name) != all[i]) {
			printf("FAIL \"%s\" does not name its framing\n",
			       all[i]->name);
			fails++;
		}
	}
}

/**
 * @brief A packet through a framing and back, at the lengths it may go wrong.
 *
 * Empty, a single octet, either side of a COBS group, and the largest this
 * build allows. The lengths a stuffing group cares about are checked for all
 * three: what only one of them groups by is exactly what the other two should
 * be indifferent to, and this says so.
 */
static void roundtrip(const struct ludlc_framing *f)
{
	static const size_t lens[] = { 0, 1, 7, 253, 254, 255, 256, 1024 };
	unsigned int t;

	for (t = 0; t <= sizeof(lens) / sizeof(lens[0]); t++) {
		size_t want = t < sizeof(lens) / sizeof(lens[0]) ? lens[t] :
								   sizeof(pay);
		struct ludlc_connection conn;
		struct rx r = { 0 };
		ludlc_payload_size_t pay_sz;
		ssize_t len, said;

		if (want > sizeof(pay))
			continue;

		pay_sz = (ludlc_payload_size_t)want;
		conn_reset(&conn);

		len = encode(f, pay_sz, wire, sizeof(wire));
		if (len < 0) {
			printf("FAIL %s: %zu octets did not encode\n",
			       f->name, want);
			fails++;
			continue;
		}

		/*
		 * What a framing says a frame will come to is what a transport
		 * announces before writing it, so it has to be what the frame
		 * then comes to.
		 */
		if (!f->wire_len) {
			continue;
		}

		said = f->wire_len(&alg, &hdr, sizeof(hdr), pay, pay_sz);
		/* COBS announces one code octet per 253 data octets, which
		 * covers a zero in the checksum, so the frame may be shorter
		 * and the transfer pads the rest.
		 */
		if (said < len || (f != &ludlc_framing_cobs && said != len)) {
			printf("FAIL %s: %zu octets announced as %zd, sent as "
			       "%zd\n", f->name, want, said, len);
			fails++;
			continue;
		}

		decode(f, wire, (size_t)len, (size_t)len, sizeof(got), &conn,
		       &r);

		if (r.count != 1 || r.size != sizeof(hdr) + want ||
		    memcmp(got, &hdr, sizeof(hdr)) ||
		    memcmp(got + sizeof(hdr), pay, want)) {
			printf("FAIL %s: %zu octets came back as %u packets "
			       "of %zu\n", f->name, want, r.count, r.size);
			fails++;
		}
	}
}

/** @brief The raw wire form is the frame, with nothing added to it. */
static void raw_is_the_frame(void)
{
	const size_t want = sizeof(pay) < 64 ? sizeof(pay) : 64;
	ssize_t len = encode(&ludlc_framing_raw, (ludlc_payload_size_t)want,
			     wire, sizeof(wire));

	if (len != (ssize_t)(sizeof(hdr) + want + alg.octets)) {
		printf("FAIL raw: %zu octets went out as %zd\n", want, len);
		fails++;
		return;
	}

	if (memcmp(wire, &hdr, sizeof(hdr)) ||
	    memcmp(wire + sizeof(hdr), pay, want)) {
		printf("FAIL raw: the packet is not on the wire as it "
		       "stands\n");
		fails++;
	}
}

/**
 * @brief A transport that stops pulling part way, and one that pulls too far.
 *
 * The encoder keeps the whole frame state, so a transport whose fifo filled
 * up can come back to exactly the octets it would have got. Once the frame is
 * out, the encoder releases the packet: pulling again gives the idle line
 * rather than the same frame twice.
 */
static void paused(const struct ludlc_framing *f)
{
	const size_t want = sizeof(pay) < 300 ? sizeof(pay) : 300;
	struct ludlc_framing_state *fs;
	struct ludlc_connection conn;
	struct rx r = { 0 };
	size_t drained = 0;
	ssize_t len;
	size_t i;

	conn_reset(&conn);

	len = encode(f, (ludlc_payload_size_t)want, wire, sizeof(wire));
	if (len < 0) {
		printf("FAIL %s: pausing: nothing encoded\n", f->name);
		fails++;
		return;
	}

	/* The same frame again, 16 octets at a time, as a fifo would take it. */
	fs = ludlc_framing_create(f);
	encoder_load(fs, (ludlc_payload_size_t)want);

	while (ludlc_enc_busy(fs)) {
		if (drained + 16 > sizeof(other)) {
			printf("FAIL %s: pulling 16 at a time ran past the "
			       "frame\n", f->name);
			fails++;
			goto out;
		}

		drained += ludlc_enc_fill(fs, &alg, other + drained, 16);
	}

	if (drained != (size_t)len || memcmp(wire, other, drained)) {
		printf("FAIL %s: pulled 16 at a time came to %zu against "
		       "%zd\n", f->name, drained, len);
		fails++;
		goto out;
	}

	/* The frame is out. A further call must not restart it. */
	i = ludlc_enc_fill(fs, &alg, other, sizeof(other));

	if (i || ludlc_enc_busy(fs)) {
		printf("FAIL %s: the encoder took the frame up again, %zu "
		       "octets of it\n", f->name, i);
		fails++;
	}

	/* And fed back an octet at a time, which a UART IRQ does. */
	decode(f, wire, (size_t)len, (size_t)len, sizeof(got), &conn, &r);

	if (r.count != 1 || r.size != sizeof(hdr) + want) {
		printf("FAIL %s: fed an octet at a time gave %u packets of "
		       "%zu\n", f->name, r.count, r.size);
		fails++;
	}

out:
	ludlc_framing_destroy(fs);
}

/**
 * @brief What follows a frame in a transfer belongs to the transport.
 *
 * An exchange is as long as the longer of the two frames in it, so the end
 * with the shorter one pads. The announcement is what says where the frame
 * stopped, and the padding - or anything else the far end clocked out - must
 * not reach the decoder as part of it.
 */
static void padding(void)
{
	const size_t want = sizeof(pay) < 32 ? sizeof(pay) : 32;
	struct ludlc_connection conn;
	struct rx r = { 0 };
	ssize_t len;
	size_t pad;

	conn_reset(&conn);

	len = encode(&ludlc_framing_raw, (ludlc_payload_size_t)want,
		     wire, sizeof(wire));
	if (len < 0) {
		printf("FAIL padding: nothing encoded\n");
		fails++;
		return;
	}

	/* Padding, and then something that is not padding. */
	pad = sizeof(wire) - (size_t)len < 64 ? sizeof(wire) - (size_t)len : 64;
	memset(wire + len, LUDLC_SPI_PAD, pad / 2);
	memset(wire + len + pad / 2, 0xa5, pad - pad / 2);

	decode(&ludlc_framing_raw, wire, (size_t)len + pad, (size_t)len,
	       sizeof(got), &conn, &r);

	if (r.count != 1 || r.size != sizeof(hdr) + want) {
		printf("FAIL %zu octets of padding gave %u packets of %zu\n",
		       pad, r.count, r.size);
		fails++;
	}
}

/**
 * @brief Every bit of a raw frame flipped in turn, and none of them accepted.
 *
 * Only the raw framing: with nothing between the octets of the frame, a
 * flipped bit is a flipped bit and the checksum is what has to catch it. In
 * the other two it may land in a code octet or make a delimiter, which ends
 * the frame somewhere else and is a different thing to check.
 */
static void damage(void)
{
	const size_t want = sizeof(pay) < 16 ? sizeof(pay) : 16;
	struct ludlc_connection conn;
	ssize_t len;
	size_t i;

	conn_reset(&conn);

	len = encode(&ludlc_framing_raw, (ludlc_payload_size_t)want,
		     wire, sizeof(wire));
	if (len < 0) {
		printf("FAIL damage: nothing encoded\n");
		fails++;
		return;
	}

	for (i = 0; i < (size_t)len * 8; i++) {
		struct rx r = { 0 };
		uint8_t save = wire[i / 8];

		wire[i / 8] ^= (uint8_t)(1U << (i % 8));
		decode(&ludlc_framing_raw, wire, (size_t)len, (size_t)len,
		       sizeof(got), &conn, &r);
		if (r.count) {
			printf("FAIL bit %zu flipped went unnoticed\n", i);
			fails++;
		}
		wire[i / 8] = save;
	}

#if CONFIG_LUDLC_STATS
	if ((size_t)conn.stats.bad_csum != (size_t)len * 8) {
		printf("FAIL %zd flipped bits counted as %u bad checksums\n",
		       len * 8, conn.stats.bad_csum);
		fails++;
	}
#endif
}

/**
 * @brief A frame that stopped part way is not a frame.
 *
 * Whatever ends one - a boundary, a delimiter or an announcement running out
 * - never arrives, so nothing may be handed over from what did.
 */
static void truncated(const struct ludlc_framing *f)
{
	const size_t want = sizeof(pay) < 16 ? sizeof(pay) : 16;
	struct ludlc_connection conn;
	ssize_t len;
	size_t i;

	conn_reset(&conn);

	len = encode(f, (ludlc_payload_size_t)want, wire, sizeof(wire));
	if (len < 0) {
		printf("FAIL %s: truncation: nothing encoded\n", f->name);
		fails++;
		return;
	}

	for (i = 1; i < (size_t)len; i++) {
		struct rx r = { 0 };

		decode(f, wire, i, (size_t)len, sizeof(got), &conn, &r);
		if (r.count) {
			printf("FAIL %s: %zu octets of %zd completed a "
			       "frame\n", f->name, i, len);
			fails++;
		}
	}
}

/**
 * @brief A frame with nowhere to land is counted and dropped, not written.
 *
 * The buffer belongs to the transport and the framing is told how much of it
 * there is; a frame that outgrows it is jabber, and the octets that would
 * have overrun it are counted away instead.
 */
static void jabber(const struct ludlc_framing *f)
{
	const size_t want = sizeof(pay) < 64 ? sizeof(pay) : 64;
	struct ludlc_connection conn;
	struct rx r = { 0 };
	ssize_t len;

	/* Raw has no delimiter to overrun: the transport refuses a length
	 * longer than its buffer before the decoder is armed.
	 */
	if (f->needs_wire_len)
		return;

	conn_reset(&conn);

	len = encode(f, (ludlc_payload_size_t)want, wire, sizeof(wire));
	if (len < 0) {
		printf("FAIL %s: jabber: nothing encoded\n", f->name);
		fails++;
		return;
	}

	decode(f, wire, (size_t)len, (size_t)len, sizeof(hdr) + want / 2, &conn,
	       &r);

	if (r.count) {
		printf("FAIL %s: a frame past the buffer was accepted\n",
		       f->name);
		fails++;
	}

#if CONFIG_LUDLC_STATS
	if (!conn.stats.jabber) {
		printf("FAIL %s: a frame past the buffer was not counted\n",
		       f->name);
		fails++;
	}
#endif
}

/** @brief Lengths a transport may announce that cannot be honoured. */
static void bounds(void)
{
	const size_t want = sizeof(pay) < 16 ? sizeof(pay) : 16;
	struct ludlc_connection conn;
	struct rx r = { 0 };
	ssize_t len;

	conn_reset(&conn);

	len = encode(&ludlc_framing_raw, (ludlc_payload_size_t)want,
		     wire, sizeof(wire));
	if (len < 0) {
		printf("FAIL bounds: nothing encoded\n");
		fails++;
		return;
	}

	/*
	 * Shorter than a PING and its checksum: nothing a link could have
	 * sent, whoever announced it.
	 */
	decode(&ludlc_framing_raw, wire, (size_t)len,
	       ludlc_frame_min_sz(&alg) - 1, sizeof(got), &conn, &r);
	if (r.count) {
		printf("FAIL a frame below the minimum was accepted\n");
		fails++;
	}
#if CONFIG_LUDLC_STATS
	if (!conn.stats.dropped) {
		printf("FAIL a frame below the minimum was not counted\n");
		fails++;
	}
#endif

	/*
	 * A transport that says nothing - a stream of octets, which this
	 * framing is not for - must not have a frame invented for it.
	 */
	r.count = 0;
	decode(&ludlc_framing_raw, wire, (size_t)len, 0, sizeof(got), &conn,
	       &r);
	if (r.count) {
		printf("FAIL a frame arrived where none was announced\n");
		fails++;
	}
}

/**
 * @brief The framing is the transport's business, not the packet's.
 *
 * The same packet through each of them, and the same packet out of each of
 * them. What differs is what it cost on the wire: stuffing and escaping both
 * pay for delimiting, and raw pays nothing because the transport delimits.
 */
static void same_packet(void)
{
	const size_t want = sizeof(pay) < 300 ? sizeof(pay) : 300;
	static uint8_t kept[KEPT_MAX];
	static uint8_t first[KEPT_MAX];
	size_t first_size = 0;
	ssize_t raw_len = 0;
	unsigned int i;

	for (i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		const struct ludlc_framing *f = all[i];
		struct ludlc_connection conn;
		struct rx r = { .kept = kept };
		ssize_t len;

		conn_reset(&conn);

		len = encode(f, (ludlc_payload_size_t)want, wire, sizeof(wire));
		if (len < 0) {
			printf("FAIL %s: comparison: nothing encoded\n",
			       f->name);
			fails++;
			return;
		}

		if (f == &ludlc_framing_raw)
			raw_len = len;
		else if (raw_len && len <= raw_len) {
			printf("FAIL %s: %zu octets cost nothing to delimit: "
			       "%zd against %zd\n", f->name, want, len,
			       raw_len);
			fails++;
		}

		decode(f, wire, (size_t)len, (size_t)len, sizeof(got), &conn,
		       &r);

		if (r.count != 1 || r.size != sizeof(hdr) + want) {
			printf("FAIL %s: comparison gave %u packets of %zu\n",
			       f->name, r.count, r.size);
			fails++;
			return;
		}

		if (!first_size) {
			first_size = r.size;
			memcpy(first, kept, r.size);
		} else if (r.size != first_size ||
			   memcmp(first, kept, r.size)) {
			printf("FAIL %s delivered a different packet than "
			       "%s\n", f->name, all[0]->name);
			fails++;
		}
	}
}

/**
 * @brief Every framing at once, on independent links.
 *
 * The framing belongs to the link, not to the build: one image may run a UART
 * on HDLC and another on COBS at the same time. Stepping the encoders and
 * decoders in lockstep, one octet each per turn, catches anything shared
 * between them, such as a static buffer or a count left in a descriptor.
 */
static void side_by_side(void)
{
	const size_t want = sizeof(pay) < SIDE_PAYLOAD ? sizeof(pay) :
							 SIDE_PAYLOAD;
	const unsigned int links = sizeof(all) / sizeof(all[0]);
	static uint8_t kept[LUDLC_FRAMING_COUNT][KEPT_MAX];
	static uint8_t alone[LUDLC_FRAMING_COUNT][SIDE_MAX];
	struct ludlc_connection conn[LUDLC_FRAMING_COUNT];
	struct ludlc_framing_state *fs[LUDLC_FRAMING_COUNT] = { NULL };
	struct rx r[LUDLC_FRAMING_COUNT];
	static uint8_t pkt[LUDLC_FRAMING_COUNT][LUDLC_FRAME_BUF_SIZE];
	size_t len[LUDLC_FRAMING_COUNT];
	size_t sent[LUDLC_FRAMING_COUNT];
	unsigned int i;
	bool any;

	/* What each framing comes to on its own, to compare against. */
	for (i = 0; i < links; i++) {
		ssize_t one = encode(all[i], (ludlc_payload_size_t)want,
				     alone[i], sizeof(alone[i]));

		if (one < 0) {
			printf("FAIL %s: side by side: nothing encoded\n",
			       all[i]->name);
			fails++;
			goto out;
		}

		len[i] = (size_t)one;
		sent[i] = 0;

		conn_reset(&conn[i]);
		memset(&r[i], 0, sizeof(r[i]));
		r[i].kept = kept[i];

		fs[i] = ludlc_framing_create(all[i]);
		encoder_load(fs[i], (ludlc_payload_size_t)want);
		ludlc_dec_init(fs[i]);
		ludlc_dec_prep(fs[i], &alg, pkt[i],
			       (ludlc_frame_size_t)(all[i]->needs_wire_len ?
						    len[i] : sizeof(pkt[i])));
	}

	/* One octet per link per turn, straight from each encoder into the
	 * decoder of the link it belongs to.
	 */
	do {
		any = false;

		for (i = 0; i < links; i++) {
			uint8_t c;
			bool done;

			if (!ludlc_enc_busy(fs[i]))
				continue;

			any = true;
			ludlc_enc_fill(fs[i], &alg, &c, 1);

			if (sent[i] >= len[i]) {
				printf("FAIL %s: interleaved, the frame "
				       "outgrew the %zu octets it takes "
				       "alone\n", all[i]->name, len[i]);
				fails++;
				goto out;
			}

			if (c != alone[i][sent[i]]) {
				printf("FAIL %s: interleaved, octet %zu is "
				       "0x%02x against 0x%02x alone\n",
				       all[i]->name, sent[i],
				       (unsigned int)c,
				       (unsigned int)alone[i][sent[i]]);
				fails++;
				goto out;
			}

			sent[i]++;

			ludlc_dec_feed(fs[i], &alg, &conn[i], &c, 1, &done);
			if (done)
				on_packet(&r[i], pkt[i],
					  ludlc_dec_size(fs[i]));
		}
	} while (any);

	for (i = 0; i < links; i++) {
		if (sent[i] != len[i]) {
			printf("FAIL %s: interleaved, %zu octets against %zu "
			       "alone\n", all[i]->name, sent[i], len[i]);
			fails++;
			continue;
		}

		if (r[i].count != 1 || r[i].size != sizeof(hdr) + want ||
		    memcmp(kept[i], &hdr, sizeof(hdr)) ||
		    memcmp(kept[i] + sizeof(hdr), pay, want)) {
			printf("FAIL %s: interleaved, %u packets of %zu\n",
			       all[i]->name, r[i].count, r[i].size);
			fails++;
		}
	}

out:
	for (i = 0; i < links; i++)
		ludlc_framing_destroy(fs[i]);
}

/** @brief States held at once, past any pool a port is likely to declare. */
#define POOL_AT_ONCE	8

/**
 * @brief That an allocator hands out one state at a time and takes it back.
 *
 * A claim which reports a slot free without marking it busy gives the next
 * caller the same state, and nothing above notices: a link encoding through a
 * state another link also holds still encodes. So this holds several at once
 * and looks for a repeat, then releases them and takes the same number again,
 * which is what says releasing one cleared anything.
 */
static void pool(void)
{
	const unsigned int links = sizeof(all) / sizeof(all[0]);
	struct ludlc_framing_state *fs[POOL_AT_ONCE];
	unsigned int i, round;

	for (i = 0; i < links; i++) {
		unsigned int before = 0;

		for (round = 0; round < 2; round++) {
			unsigned int got, j, k, dups = 0;

			for (got = 0; got < POOL_AT_ONCE; got++) {
				fs[got] = ludlc_framing_create(all[i]);
				if (!fs[got])
					break;
			}

			if (!got) {
				printf("FAIL %s: pool: no state at all\n",
				       all[i]->name);
				fails++;
				break;
			}

			for (j = 1; j < got; j++) {
				for (k = 0; k < j; k++) {
					if (fs[j] != fs[k])
						continue;

					dups++;
					break;
				}
			}

			if (dups) {
				printf("FAIL %s: pool: %u of %u states are "
				       "an earlier one over again\n",
				       all[i]->name, dups, got);
				fails++;
			}

			if (round && got != before) {
				printf("FAIL %s: pool: %u states once "
				       "released against %u before\n",
				       all[i]->name, got, before);
				fails++;
			}

			before = got;

			for (j = 0; j < got; j++)
				ludlc_framing_destroy(fs[j]);
		}
	}
}

int main(void)
{
	unsigned int i;
	size_t n;

	/* Multiples of seven, so the payload has zero octets in it. */
	for (n = 0; n < sizeof(pay); n++)
		pay[n] = (uint8_t)((n * 7) & 0xff);

	printf("ludlc framing: payload %u octets in %zu bits, frame %zu "
	       "octets\n", (unsigned int)CONFIG_LUDLC_MAX_PAYLOAD_SIZE,
	       sizeof(ludlc_payload_size_t) * 8, (size_t)LUDLC_FRAME_BUF_SIZE);

	flags();

	for (i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		roundtrip(all[i]);
		paused(all[i]);
		truncated(all[i]);
		jabber(all[i]);
	}

	raw_is_the_frame();
	padding();
	damage();
	bounds();
	same_packet();
	side_by_side();
	pool();

	printf(fails ? "%d FAILURES\n" : "all passed\n", fails);

	return fails ? EXIT_FAILURE : EXIT_SUCCESS;
}

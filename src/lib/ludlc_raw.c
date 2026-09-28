// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only)
/**
 * @file ludlc_raw.c
 *
 * @brief The frame unchanged: header, payload, then the checksum.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * Nothing is added and nothing is escaped, so there is no delimiter to
 * resynchronise on. Only a transport that carries one frame per transfer can
 * run this, since it has to supply the frame length itself.
 */

#ifdef __KERNEL__
#include <linux/export.h>
#include <linux/string.h>
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#endif

#include <ludlc_framing.h>
#include <ludlc_private.h>

/**
 * @enum raw_enc_fsm_state
 * @brief Internal states for the encoder FSM.
 */
enum {
	renc_idle = 0, /* Must be 0 */
	renc_data,
	renc_csum,	/* Checksum octets, not folded back into the sum. */
};

/**
 * @enum raw_dec_fsm_state
 * @brief Internal states for the decoder FSM.
 */
enum {
	rdec_idle = 0, /* Must be 0 */
	rdec_data,	/* Announced frame, header through checksum. */
};

/**
 * @struct ludlc_renc_state
 * @brief The encoder's state between calls.
 *
 * The frame is the packet itself, so nothing is staged here. The members
 * point into the caller's packet and count how much of it has gone out.
 */
struct ludlc_renc_state {
	/**< Pointer to the packet header to send. */
	const void	*hdr;
	/**< Pointer to the packet payload to send. */
	const void	*payload;

	/**< Total size of the header. */
	ludlc_payload_size_t	hdr_size;
	/**< Total size of the payload. */
	ludlc_payload_size_t	payload_size;

	/**< Next octet of the current run: header or payload. */
	const uint8_t		*ptr;

	/**< Octets still to copy from @c ptr, or checksum octets still to send. */
	size_t		sz;

	/**< Current FSM state (see above). */
	uint8_t		state;

	ludlc_csum_t	csum;
};

/**
 * @struct ludlc_rdec_state
 * @brief The decoder's state between calls.
 *
 * Octets go straight into the caller's buffer. Only the length the transport
 * announced has to be tracked.
 */
struct ludlc_rdec_state {
	/** @brief Buffer to store the incoming packet. */
	uint8_t			*payload;

	/**< Current size of the incoming packet. */
	ludlc_frame_size_t	size;
	/**< Running checksum of the incoming packet. */
	ludlc_csum_t		csum;

	/**< Current FSM state (see above). */
	uint8_t			state;

	/**< Octets of the announced frame still to come. */
	size_t			sz;
};

/**
 * @struct raw_state
 * @brief What @ref ludlc_create_framing_raw returns, seen from this file.
 *
 * The link only sees a @ref ludlc_framing_state handle on it, which keeps
 * both state machines private here.
 */
struct raw_state {
	struct ludlc_framing_state base;
	struct ludlc_renc_state	enc;
	struct ludlc_rdec_state	dec;
};

/** @brief The state behind @p fs, which this framing allocated. */
static inline struct raw_state *fs_to_raw(struct ludlc_framing_state *fs)
{
	return LUDLC_CONTAINER_OF(fs, struct raw_state, base);
}

/** @brief Same, for the operations that only read it. */
static inline const struct raw_state *
fs_to_raw_const(const struct ludlc_framing_state *fs)
{
	return LUDLC_CONTAINER_OF(fs, struct raw_state, base);
}

/*
 * The framing's entry points, reached through ludlc_framing_raw_ops or, in a
 * build carrying raw alone, called by name.
 */

void ludlc_raw_enc_init(struct ludlc_framing_state *fs)
{
	struct ludlc_renc_state *enc = &fs_to_raw(fs)->enc;

	enc->state = renc_idle;
	enc->hdr = NULL;
	enc->hdr_size = 0;
	enc->payload_size = 0;
}

void ludlc_raw_enc_prep(struct ludlc_framing_state *fs,
		       const void *hdr, ludlc_payload_size_t hdr_sz,
		       const void *payload, ludlc_payload_size_t pay_sz,
		       bool send_eof)
{
	struct ludlc_renc_state *enc = &fs_to_raw(fs)->enc;

	/* The transport delimits the frame, so nothing is appended. */
	(void)send_eof;

	enc->hdr = hdr;
	enc->hdr_size = hdr_sz;
	enc->payload = payload;
	enc->payload_size = pay_sz;
}

bool ludlc_raw_enc_busy(const struct ludlc_framing_state *fs)
{
	const struct ludlc_renc_state *enc = &fs_to_raw_const(fs)->enc;

	return enc->state != renc_idle || enc->hdr_size != 0;
}

size_t ludlc_raw_enc_fill(struct ludlc_framing_state *fs,
			  const struct ludlc_csum *csum,
			  uint8_t *dst, size_t dst_len)
{
	struct ludlc_renc_state *enc = &fs_to_raw(fs)->enc;
	size_t n = 0;

	while (n < dst_len && ludlc_raw_enc_busy(fs)) {
		size_t len;

		switch (enc->state) {
		case renc_idle:
		default:
			/* Busy while idle means a packet is queued. Without a
			 * header there is nothing to send, so the reset below
			 * drops it.
			 */
			if (!enc->hdr_size || !enc->hdr)
				break;

			if (ludlc_csum_on(csum))
				enc->csum = ludlc_csum_tx_init(csum);

			enc->ptr = enc->hdr;
			enc->sz = enc->hdr_size;
			enc->state = renc_data;
			fallthrough;

		case renc_data:
			len = dst_len - n;
			if (len > enc->sz)
				len = enc->sz;

			if (ludlc_csum_on(csum)) {
				size_t i;

				for (i = 0; i < len; i++) {
					uint8_t c = *enc->ptr++;

					dst[n++] = c;
					enc->csum = ludlc_csum_tx(csum,
							enc->csum, c);
				}
			} else {
				memcpy(&dst[n], enc->ptr, len);
				enc->ptr += len;
				n += len;
			}

			enc->sz -= len;
			if (enc->sz) {
				continue;
			}

			if (enc->payload_size) {
				enc->ptr = enc->payload;
				enc->sz = enc->payload_size;
				enc->payload_size = 0;
				continue;
			}

			if (ludlc_csum_on(csum)) {
				enc->state = renc_csum;
				enc->csum = ludlc_csum_tx_final(csum, enc->csum);
				enc->sz = ludlc_csum_octets(csum);
				continue;
			}
			break;

		case renc_csum:
			/* Least significant octet first, shifted out of the
			 * value rather than read from its storage, so the
			 * wire form does not depend on the host's byte order.
			 */
			dst[n++] = enc->csum & 0xff;
			enc->csum >>= 8;
			if (--enc->sz) {
				continue;
			}
			break;
		}

		/* Frame sent: release the packet so it is not sent twice. */
		ludlc_raw_enc_init(fs);
	}

	return n;
}

void ludlc_raw_dec_init(struct ludlc_framing_state *fs)
{
	struct ludlc_rdec_state *dec = &fs_to_raw(fs)->dec;

	dec->state = rdec_idle;
	dec->size = 0;
}

void ludlc_raw_dec_prep(struct ludlc_framing_state *fs,
			const struct ludlc_csum *csum, void *buf,
			ludlc_frame_size_t cap)
{
	struct ludlc_rdec_state *dec = &fs_to_raw(fs)->dec;

	/* @p cap is the announced frame length: the transport refused one
	 * longer than its buffer, so what arrives always fits.
	 */
	dec->payload = buf;
	dec->size = 0;
	dec->sz = cap;
	dec->state = cap ? rdec_data : rdec_idle;
	if (ludlc_csum_on(csum))
		dec->csum = ludlc_csum_rx_init(csum);
}

size_t ludlc_raw_dec_feed(struct ludlc_framing_state *fs,
			  const struct ludlc_csum *csum,
			  struct ludlc_connection *conn,
			  const uint8_t *src, size_t len, bool *done)
{
	struct ludlc_rdec_state *dec = &fs_to_raw(fs)->dec;
	size_t take = len < dec->sz ? len : dec->sz;

	*done = false;

	switch (dec->state) {
	case rdec_data:
		if (ludlc_csum_on(csum)) {
			size_t i;

			for (i = 0; i < take; i++) {
				uint8_t c = src[i];

				dec->csum = ludlc_csum_rx(csum, dec->csum, c);
				dec->payload[dec->size++] = c;
			}
		} else {
			memcpy(dec->payload + dec->size, src, take);
			dec->size += take;
		}

		dec->sz -= take;
		if (dec->sz)
			return take;

		/* The announced length is reached, valid frame or not. */
		dec->state = rdec_idle;

		if (dec->size < ludlc_frame_min_sz(csum)) {
			LUDLC_INC_STATS(conn, dropped);
		} else if (ludlc_csum_on(csum) &&
			   !ludlc_csum_rx_ok(csum, dec->csum)) {
			LUDLC_INC_STATS(conn, bad_csum);
		} else {
			if (ludlc_csum_on(csum))
				dec->size -= ludlc_csum_octets(csum);
			LUDLC_INC_STATS(conn, rx_packet);
			*done = true;
		}
		return take;

	default:
		/* Nothing was announced: the octets belong to the transport. */
		return len;
	}
}

ludlc_payload_size_t ludlc_raw_dec_size(const struct ludlc_framing_state *fs)
{
	return (ludlc_payload_size_t)fs_to_raw_const(fs)->dec.size;
}

static const struct ludlc_framing_ops ludlc_framing_raw_ops = {
	LUDLC_FRAMING_OPS(raw)
};

static ludlc_frame_size_t
framing_raw_encoded_max(const struct ludlc_connection *conn,
			const struct ludlc_csum *csum)
{
	return ludlc_conn_frame_sz(conn, csum);
}

static ssize_t framing_raw_wire_len(const struct ludlc_csum *csum,
				    const void *hdr,
				    ludlc_payload_size_t hdr_sz,
				    const void *payload,
				    ludlc_payload_size_t pay_sz)
{
	ssize_t sz = ludlc_csum_on(csum) ? ludlc_csum_octets(csum) : 0;

	(void)hdr;
	(void)payload;

	sz += (ssize_t)hdr_sz + pay_sz;
	return sz;
}

const struct ludlc_framing ludlc_framing_raw = {
	.name = "raw",
	.ops = &ludlc_framing_raw_ops,
	/* The frame is its own wire form, so its length is known up front. */
	.needs_wire_len = true,
	.encoded_max = framing_raw_encoded_max,
	.wire_len = framing_raw_wire_len,
};
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(ludlc_framing_raw);
#endif

/**
 * @def CONFIG_LUDLC_STATIC_FRAMING_NUM
 * @brief Framing objects preallocated per framing.
 *
 * All a target without a heap can hand out. A port sets this to the number of
 * links it runs. At zero, every object comes from @c ludlc_platform_alloc().
 * A build with one link and one framing needs one object, and gets it by
 * default.
 */
#ifndef CONFIG_LUDLC_STATIC_FRAMING_NUM
#if !CONFIG_LUDLC_MULTI_CONN && LUDLC_FRAMING_COUNT == 1
#define CONFIG_LUDLC_STATIC_FRAMING_NUM	1
#else
#define CONFIG_LUDLC_STATIC_FRAMING_NUM	0
#endif
#endif

#if CONFIG_LUDLC_STATIC_FRAMING_NUM

static struct raw_state g_raw_framing[CONFIG_LUDLC_STATIC_FRAMING_NUM];
static LUDLC_DECLARE_BITSET(g_raw_framing_busy,
			    CONFIG_LUDLC_STATIC_FRAMING_NUM);

static struct raw_state *raw_framing_get(void)
{
	const int slot = ludlc_bitset_claim(g_raw_framing_busy,
					    CONFIG_LUDLC_STATIC_FRAMING_NUM);

	return slot < 0 ? NULL : &g_raw_framing[slot];
}

static bool raw_framing_put(const struct raw_state *state)
{
	if (state < &g_raw_framing[0] ||
	    state >= &g_raw_framing[CONFIG_LUDLC_STATIC_FRAMING_NUM]) {
		return false;
	}

	ludlc_platform_clear_bit(state - g_raw_framing, g_raw_framing_busy);

	return true;
}

#else /* nothing preallocated */

static inline struct raw_state *raw_framing_get(void)
{
	return NULL;
}

static inline bool raw_framing_put(const struct raw_state *state)
{
	(void)state;

	return false;
}

#endif /* CONFIG_LUDLC_STATIC_FRAMING_NUM */

struct ludlc_framing_state *ludlc_create_framing_raw(void)
{
	struct raw_state *state = raw_framing_get();

	if (!state) {
		state = ludlc_platform_alloc(sizeof(*state));
		if (!state) {
			return NULL;
		}
	}

	/* A reused slot still holds the last link's frame, and the allocator
	 * returns anything, so clear both state machines.
	 */
	memset(state, 0, sizeof(*state));
#if LUDLC_FRAMING_COUNT	> 1
	state->base.fops = &ludlc_framing_raw;
#endif
	return &state->base;
}
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(ludlc_create_framing_raw);
#endif

void ludlc_destroy_framing_raw(struct ludlc_framing_state *fs)
{
	struct raw_state *state;

	if (!fs) {
		return;
	}

	state = fs_to_raw(fs);

	if (!raw_framing_put(state)) {
		ludlc_platform_free(state);
	}
}
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(ludlc_destroy_framing_raw);
#endif

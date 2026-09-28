// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only)
/**
 * @file ludlc_cobs.c
 *
 * @brief Consistent Overhead Byte Stuffing for LuDLC frames, checksum
 * included.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * A group's code octet precedes the octets it counts, so the encoder scans
 * ahead in the caller's packet instead of stuffing into a buffer first. The
 * frame is read twice, once for the checksum and once on the way out, and no
 * copy of it is kept.
 */

#ifdef __KERNEL__
#include <linux/export.h>
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stdint.h>
#endif

#include <ludlc_framing.h>
#include <ludlc_private.h>

/**
 * @def CENC_FULL_F
 * @brief The open encoder group is full, in the shared framing flags.
 *
 * A full group ends on no delimiter, so the next octet starts the following
 * group. A group that stopped short ended on a delimiter and accounts for it.
 */
#define CENC_FULL_F	LUDLC_FS_LAST_PUBLIC_BIT
/**
 * @def CDEC_STARTED_F
 * @brief A group has been read, so a delimiter precedes the next one.
 */
#define CDEC_STARTED_F	(LUDLC_FS_LAST_PUBLIC_BIT + 1)
/**
 * @def CDEC_OVER_F
 * @brief The frame outgrew the buffer: drop everything up to the delimiter.
 *
 * The encoder writes @ref CENC_FULL_F and the decoder these two, so a link
 * running both directions at once keeps each in its own bit of one word.
 */
#define CDEC_OVER_F	(LUDLC_FS_LAST_PUBLIC_BIT + 2)
/**
 * @def CENC_ARMED_F
 * @brief The running sum has been turned into its wire value.
 */
#define CENC_ARMED_F	(LUDLC_FS_LAST_PUBLIC_BIT + 4)
/**
 * @def CENC_DELIM_F
 * @brief The open group stopped on a delimiter, which is still to be skipped.
 */
#define CENC_DELIM_F	(LUDLC_FS_LAST_PUBLIC_BIT + 5)
/**
 * @def CENC_FLAGS_MASK
 * @brief The encoder's bits, cleared together when a frame starts over.
 */
#define CENC_FLAGS_MASK	((1U << CENC_FULL_F) | (1U << CENC_ARMED_F) | \
			 (1U << CENC_DELIM_F))
/**
 * @def CDEC_FLAGS_MASK
 * @brief The decoder's bits, cleared together when a frame starts over.
 *
 * Encoder and decoder each clear only their own bits, so a link running both
 * directions at once never wipes the other's state.
 */
#define CDEC_FLAGS_MASK	((1U << CDEC_STARTED_F) | (1U << CDEC_OVER_F))

/**
 * @enum cobs_enc_fsm_state
 * @brief Internal states for the encoder FSM.
 */
enum {
	cenc_idle = 0, /* Must be 0 */
	cenc_code,
	cenc_data,
};

/**
 * @struct ludlc_cenc_state
 * @brief State machine structure for the COBS encoder.
 *
 * A group's code octet precedes the octets it counts, so the encoder looks
 * ahead from @c ptr rather than stuff into a buffer first. The packet
 * must stay valid until the encoder goes idle.
 */
struct ludlc_cenc_state {
	/**< Pointer to the packet header to send. */
	const void	*hdr;
	/**< Pointer to the packet payload to send. */
	const void	*payload;

	/**< Next octet of the current run: header or payload. */
	const uint8_t	*ptr;

	/**< Total size of the header. */
	ludlc_payload_size_t	hdr_size;
	/**< Total size of the payload still to switch to. */
	ludlc_payload_size_t	payload_size;

	/**< Octets left in the segment @c ptr points at. */
	size_t		sz;

	/**
	 * @brief The frame's checksum.
	 *
	 * The running sum while the body is scanned, then the wire value.
	 * Its octets are shifted out least significant first.
	 */
	ludlc_csum_t	csum;

	/**< Checksum octets not yet sent. Armed once the body has been summed. */
	uint8_t		csum_left;

	/**< Data octets still to send from the group that was opened. */
	uint8_t		run;

	/**< Current FSM state (see above). */
	uint8_t		state;
};

/**
 * @struct ludlc_cdec_state
 * @brief State machine structure for the COBS decoder.
 *
 * De-stuffing needs no lookahead, so octets go directly into the caller's
 * packet buffer as they arrive.
 */
struct ludlc_cdec_state {
	/** @brief Buffer to store the de-stuffed incoming packet. */
	uint8_t			*payload;

	ludlc_frame_size_t	payload_cap;
	/**< Current size of the incoming packet. */
	ludlc_frame_size_t	size;
	/**< Running checksum of the incoming packet. */
	ludlc_csum_t		csum;
	/**< The code octet of the group being read. */
	uint8_t			code;
	/**< Data octets still expected in that group. */
	uint8_t			left;
};

/**
 * @struct cobs_state
 * @brief What @ref ludlc_create_framing_cobs returns, seen from this file.
 *
 * The link only sees a @ref ludlc_framing_state handle on it, which keeps
 * both state machines private here.
 */
struct cobs_state {
	struct ludlc_framing_state base;
	struct ludlc_cenc_state	   enc;
	struct ludlc_cdec_state	   dec;
};

/** @brief The state behind @p fs, which this framing allocated. */
static inline struct cobs_state *fs_to_cobs(struct ludlc_framing_state *fs)
{
	return LUDLC_CONTAINER_OF(fs, struct cobs_state, base);
}

/** @brief Same, for the operations that only read it. */
static inline const struct cobs_state *
fs_to_cobs_const(const struct ludlc_framing_state *fs)
{
	return LUDLC_CONTAINER_OF(fs, struct cobs_state, base);
}

/** @brief True while a packet is loaded or partly sent. */
static bool cobs_busy(const struct ludlc_cenc_state *enc_state)
{
	return enc_state->state != cenc_idle || enc_state->hdr_size != 0;
}

static void cobs_dec_reset(struct ludlc_framing_state *fs,
			   const struct ludlc_csum *csum_alg)
{
	struct ludlc_cdec_state *dec_state = &fs_to_cobs(fs)->dec;

	dec_state->size = 0;
	if (ludlc_csum_on(csum_alg))
		dec_state->csum = ludlc_csum_rx_init(csum_alg);
	dec_state->code = 0;
	dec_state->left = 0;
	ludlc_platform_clear_mask(CDEC_FLAGS_MASK, &fs->flags);
}

static void cobs_store_byte(struct ludlc_framing_state *fs,
			    const struct ludlc_csum *csum_alg, uint8_t c)
{
	struct ludlc_cdec_state *dec_state = &fs_to_cobs(fs)->dec;

	if (dec_state->size < dec_state->payload_cap) {
		dec_state->payload[dec_state->size++] = c;
		if (ludlc_csum_on(csum_alg))
			dec_state->csum = ludlc_csum_rx(csum_alg,
							dec_state->csum, c);
	} else {
		/* Frame is longer than this link can carry (jabber) */
		ludlc_platform_set_bit(CDEC_OVER_F, &fs->flags);
	}
}

/*
 * The framing's entry points, reached through ludlc_framing_cobs_ops or, in a
 * build carrying COBS alone, called by name.
 */

void ludlc_cobs_enc_init(struct ludlc_framing_state *fs)
{
	struct ludlc_cenc_state *enc = &fs_to_cobs(fs)->enc;

	enc->state = cenc_idle;
	enc->hdr_size = 0;
	enc->payload_size = 0;
	enc->csum_left = 0;
	ludlc_platform_clear_mask(CENC_FLAGS_MASK, &fs->flags);
}

void ludlc_cobs_enc_prep(struct ludlc_framing_state *fs,
			const void *hdr, ludlc_payload_size_t hdr_sz,
			const void *payload, ludlc_payload_size_t pay_sz,
			bool send_eof)
{
	struct ludlc_cenc_state *enc = &fs_to_cobs(fs)->enc;

	/* Every frame already ends on the delimiter. */
	(void)send_eof;

	enc->hdr = hdr;
	enc->hdr_size = hdr_sz;
	enc->payload = payload;
	enc->payload_size = pay_sz;
}

bool ludlc_cobs_enc_busy(const struct ludlc_framing_state *fs)
{
	return cobs_busy(&fs_to_cobs_const(fs)->enc);
}

size_t ludlc_cobs_enc_fill(struct ludlc_framing_state *fs,
			   const struct ludlc_csum *csum, uint8_t *dst,
			   size_t cap)
{
	struct ludlc_cenc_state *enc = &fs_to_cobs(fs)->enc;
	size_t n = 0;

	while (n < cap && cobs_busy(enc)) {
		switch (enc->state) {
		case cenc_idle:
		default:
			if (!enc->hdr_size || !enc->hdr) {
				ludlc_cobs_enc_init(fs);
				break;
			}

			enc->ptr = enc->hdr;
			enc->sz = enc->hdr_size;
			enc->csum_left = 0;
			if (ludlc_csum_on(csum))
				enc->csum = ludlc_csum_tx_init(csum);
			enc->state = cenc_code;
			fallthrough;

		case cenc_code: {
			const uint8_t *p = enc->ptr;
			size_t sz = enc->sz;
			const uint8_t *pay = enc->payload;
			size_t pay_sz = enc->payload_size;
			uint8_t cleft = enc->csum_left;
			uint8_t run;
			bool delim = false;

			/* Open a group from the cursor and fold each body
			 * octet into the checksum as the scan passes it.
			 */
			for (run = 0; run < LUDLC_COBS_BLOCK_MAX; run++) {
				uint8_t c;
				bool body = true;

				if (sz) {
					c = *p++;
					sz--;
				} else if (pay_sz) {
					c = *pay++;
					pay_sz--;
				} else {
					if (!ludlc_platform_test_bit(
							CENC_ARMED_F, &fs->flags)) {
						if (ludlc_csum_on(csum))
							enc->csum =
								ludlc_csum_tx_final(
									csum, enc->csum);
						cleft = ludlc_csum_on(csum) ?
							ludlc_csum_octets(csum) : 0;
						enc->csum_left = cleft;
						ludlc_platform_set_bit(
							CENC_ARMED_F, &fs->flags);
					}
					if (!cleft)
						break;
					c = (uint8_t)(enc->csum >>
						      (8U * (enc->csum_left - cleft)));
					cleft--;
					body = false;
				}

				if (body && ludlc_csum_on(csum))
					enc->csum = ludlc_csum_tx(csum,
								  enc->csum, c);
				if (c == LUDLC_COBS_DELIMITER) {
					delim = true;
					break;
				}
			}

			if (delim)
				ludlc_platform_set_bit(CENC_DELIM_F, &fs->flags);
			else
				ludlc_platform_clear_bit(CENC_DELIM_F,
							 &fs->flags);
			if (run == LUDLC_COBS_BLOCK_MAX)
				ludlc_platform_set_bit(CENC_FULL_F, &fs->flags);
			else
				ludlc_platform_clear_bit(CENC_FULL_F,
							 &fs->flags);
			enc->run = run;
			enc->state = cenc_data;
			dst[n++] = (uint8_t)(run + 1);
			break;
		}

		case cenc_data:
			while (enc->run && n < cap) {
				size_t len;

				if (!enc->sz && enc->payload_size) {
					enc->ptr = enc->payload;
					enc->sz = enc->payload_size;
					enc->payload_size = 0;
				}
				if (!enc->sz) {
					if (!enc->csum_left &&
					    ludlc_platform_test_bit(CENC_ARMED_F,
								    &fs->flags) &&
					    ludlc_csum_on(csum))
						enc->csum_left =
							ludlc_csum_octets(csum);
					if (!enc->csum_left)
						break;
					dst[n++] = (uint8_t)enc->csum;
					enc->csum >>= 8;
					enc->csum_left--;
					enc->run--;
					continue;
				}

				len = enc->run;
				if (len > enc->sz)
					len = enc->sz;
				if (len > cap - n)
					len = cap - n;
				{
					size_t i;

					for (i = 0; i < len; i++)
						dst[n + i] = enc->ptr[i];
				}
				enc->ptr += len;
				enc->sz -= len;
				enc->run -= len;
				n += len;
			}

			/* The caller's buffer is full. The group ends on the
			 * next call, when there is room for what follows.
			 */
			if (enc->run || n >= cap)
				break;

			/* The group stopped on a zero: that octet is not
			 * sent. A full group has no such octet. Either way
			 * another group follows while anything remains.
			 */
			if (ludlc_platform_test_bit(CENC_DELIM_F, &fs->flags)) {
				if (!enc->sz && enc->payload_size) {
					enc->ptr = enc->payload;
					enc->sz = enc->payload_size;
					enc->payload_size = 0;
				}
				if (enc->sz) {
					enc->ptr++;
					enc->sz--;
				} else if (enc->csum_left) {
					enc->csum >>= 8;
					enc->csum_left--;
				}
				enc->state = cenc_code;
				break;
			}

			if (ludlc_platform_test_bit(CENC_FULL_F, &fs->flags) &&
			    (enc->sz || enc->payload_size || enc->csum_left ||
			     (!ludlc_platform_test_bit(CENC_ARMED_F,
						       &fs->flags) &&
			      ludlc_csum_on(csum) && ludlc_csum_octets(csum)))) {
				enc->state = cenc_code;
				break;
			}

			dst[n++] = LUDLC_COBS_DELIMITER;
			ludlc_cobs_enc_init(fs);
			break;
		}
	}

	return n;
}

void ludlc_cobs_dec_init(struct ludlc_framing_state *fs)
{
	struct ludlc_cdec_state *dec = &fs_to_cobs(fs)->dec;

	dec->size = 0;
}

void ludlc_cobs_dec_prep(struct ludlc_framing_state *fs,
			 const struct ludlc_csum *csum, void *buf,
			 ludlc_frame_size_t cap)
{
	struct ludlc_cdec_state *dec = &fs_to_cobs(fs)->dec;

	dec->payload = buf;
	dec->payload_cap = cap;
	cobs_dec_reset(fs, csum);
}

size_t ludlc_cobs_dec_feed(struct ludlc_framing_state *fs,
			   const struct ludlc_csum *csum,
			   struct ludlc_connection *conn,
			   const uint8_t *src, size_t len, bool *done)
{
	struct ludlc_cdec_state *dec = &fs_to_cobs(fs)->dec;
	size_t n;

	*done = false;

	for (n = 0; n < len; n++) {
		uint8_t c = src[n];

		if (c != LUDLC_COBS_DELIMITER) {
			if (ludlc_platform_test_bit(CDEC_OVER_F, &fs->flags))
				continue;

			if (dec->left) {
				cobs_store_byte(fs, csum, c);
				dec->left--;
				continue;
			}

			/* A code octet. The previous group ended on a
			 * delimiter unless it was full, and that delimiter is
			 * only known to be part of the data now that a group
			 * follows it.
			 */
			if (ludlc_platform_test_bit(CDEC_STARTED_F,
						    &fs->flags) &&
			    dec->code != 0xff) {
				cobs_store_byte(fs, csum, LUDLC_COBS_DELIMITER);
			}

			dec->code = c;
			dec->left = c - 1;
			ludlc_platform_set_bit(CDEC_STARTED_F, &fs->flags);
			continue;
		}

		/* The delimiter ends the frame, valid or not. */
		if (ludlc_platform_test_bit(CDEC_OVER_F, &fs->flags)) {
			LUDLC_INC_STATS(conn, jabber);
		} else if (dec->left) {
			/* The last code octet claimed more octets than
			 * arrived. The checksum does not cover code octets, so
			 * it would accept the data that did arrive.
			 */
			LUDLC_INC_STATS(conn, dropped);
		} else if (dec->size >= ludlc_frame_min_sz(csum)) {
			/* The checksum covers its own octets, so an intact
			 * frame always leaves the same residual.
			 */
			if (!ludlc_csum_on(csum) ||
			    ludlc_csum_rx_ok(csum, dec->csum)) {
				LUDLC_INC_STATS(conn, rx_packet);
				if (ludlc_csum_on(csum))
					dec->size -= ludlc_csum_octets(csum);
				*done = true;
				return n;
			}

			LUDLC_INC_STATS(conn, bad_csum);
		} else if (dec->size) {
			/* Packet was too short */
			LUDLC_INC_STATS(conn, dropped);
		}

		cobs_dec_reset(fs, csum);
	}

	return n;
}

ludlc_payload_size_t ludlc_cobs_dec_size(const struct ludlc_framing_state *fs)
{
	return (ludlc_payload_size_t)fs_to_cobs_const(fs)->dec.size;
}

static const struct ludlc_framing_ops ludlc_framing_cobs_ops = {
	LUDLC_FRAMING_OPS(cobs)
};

static ludlc_frame_size_t
framing_cobs_encoded_max(const struct ludlc_connection *conn,
			 const struct ludlc_csum *csum)
{
	return ludlc_cobs_frame_sz(conn, csum);
}

static ssize_t framing_cobs_wire_len(const struct ludlc_csum *csum_alg,
				     const void *hdr,
				     ludlc_payload_size_t hdr_sz,
				     const void *payload,
				     ludlc_payload_size_t pay_sz)
{
	/* One less than a full group, so a zero in the checksum is already
	 * inside the overhead. The encoding is never longer than this.
	 */
	const size_t group = LUDLC_COBS_BLOCK_MAX - 1;
	size_t n = (size_t)hdr_sz + pay_sz;

	(void)hdr;
	(void)payload;

	if (ludlc_csum_on(csum_alg))
		n += ludlc_csum_octets(csum_alg);
	if (!n)
		return 1;

	return (ssize_t)(n + (n + group - 1) / group + 1);
}

const struct ludlc_framing ludlc_framing_cobs = {
	.name = "cobs",
	.ops = &ludlc_framing_cobs_ops,
	/* The stuffing is counted up front, see framing_cobs_wire_len(). */
	.needs_wire_len = false,
	.encoded_max = framing_cobs_encoded_max,
	.wire_len = framing_cobs_wire_len,
};
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(ludlc_framing_cobs);
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

static struct cobs_state g_cobs_framing[CONFIG_LUDLC_STATIC_FRAMING_NUM];
static LUDLC_DECLARE_BITSET(g_cobs_framing_busy,
			    CONFIG_LUDLC_STATIC_FRAMING_NUM);

static struct cobs_state *cobs_framing_get(void)
{
	const int slot = ludlc_bitset_claim(g_cobs_framing_busy,
					    CONFIG_LUDLC_STATIC_FRAMING_NUM);

	return slot < 0 ? NULL : &g_cobs_framing[slot];
}

static bool cobs_framing_put(const struct cobs_state *state)
{
	if (state < &g_cobs_framing[0] ||
	    state >= &g_cobs_framing[CONFIG_LUDLC_STATIC_FRAMING_NUM]) {
		return false;
	}

	ludlc_platform_clear_bit(state - g_cobs_framing, g_cobs_framing_busy);

	return true;
}

#else /* nothing preallocated */

static inline struct cobs_state *cobs_framing_get(void)
{
	return NULL;
}

static inline bool cobs_framing_put(const struct cobs_state *state)
{
	(void)state;

	return false;
}

#endif /* CONFIG_LUDLC_STATIC_FRAMING_NUM */

struct ludlc_framing_state *ludlc_create_framing_cobs(void)
{
	struct cobs_state *state = cobs_framing_get();

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
	state->base.fops = &ludlc_framing_cobs;
#endif
	return &state->base;
}
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(ludlc_create_framing_cobs);
#endif

void ludlc_destroy_framing_cobs(struct ludlc_framing_state *fs)
{
	struct cobs_state *state;

	if (!fs) {
		return;
	}

	state = fs_to_cobs(fs);

	if (!cobs_framing_put(state)) {
		ludlc_platform_free(state);
	}
}
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(ludlc_destroy_framing_cobs);
#endif

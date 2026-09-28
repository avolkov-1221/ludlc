// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only)
/**
 * @file ludlc_hdlc.c
 *
 * @brief Byte stuffing and framing for LuDLC packets on a byte stream.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * A start octet delimits the frame and an escape octet protects both reserved
 * values. The framing computes the checksum, so neither state machine below
 * needs the connection except for its counters. Both work in place: the
 * encoder reads the caller's packet and writes into the transport's buffer,
 * and the decoder writes into the buffer the transport supplied.
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

/* Every port compiles all three framings and picks between them with the
 * CONFIG_LUDLC_FRAMING_* values, so one left out of a build compiles to
 * nothing rather than being dropped from the source list.
 */

/**
 * @def ENC_SEND_EOF_F
 * @brief Flag indicating an EOF (SOF) should be sent after the packet.
 */
#define ENC_SEND_EOF_F LUDLC_FS_LAST_PUBLIC_BIT

/**
 * @struct ludlc_henc_state
 * @brief State machine structure for the serial encoder.
 */
struct ludlc_henc_state {
	/**< Pointer to the packet header to send. */
	const void	*hdr;
	/**< Pointer to the packet payload to send. */
	const void	*payload;
	/**< Internal pointer to the current octet being sent. */
	const uint8_t	*ptr;

	/**< Total size of the header. */
	ludlc_payload_size_t	hdr_size;
	/**< Total size of the payload. */
	ludlc_payload_size_t	payload_size;
	/**< Remaining octets in the current segment. */
	ludlc_payload_size_t	sz;

	/**< Running checksum of the outgoing packet. */
	ludlc_csum_t	csum;

	/**< Current FSM state (see below). */
	uint8_t	state;
	/**< State to return to after sending an escaped octet. */
	uint8_t	esc_state;
	/**< The escaped octet (data ^ MASK) to be sent. */
	uint8_t esc_data;
};

/**
 * @struct ludlc_hdec_state
 * @brief State machine structure for the serial decoder.
 */
struct ludlc_hdec_state {
	/**< Current size of the incoming packet. */
	ludlc_frame_size_t	size;
	/**< Running checksum of the incoming packet. */
	ludlc_csum_t		csum;

	/**< Current FSM state (see below). */
	uint8_t			state;

	/** @brief Buffer to store the de-escaped incoming packet. */
	uint8_t			*payload;

	ludlc_frame_size_t	payload_cap;
};

/**
 * @struct hdlc_state
 * @brief What @ref ludlc_create_framing_hdlc returns, seen from this file.
 *
 * The link only sees a @ref ludlc_framing_state handle on it, which keeps
 * both state machines private here.
 */
struct hdlc_state {
	struct ludlc_framing_state base;
	struct ludlc_henc_state	enc;
	struct ludlc_hdec_state	dec;
};

/** @brief The state behind @p fs, which this framing allocated. */
static inline struct hdlc_state *fs_to_hdlc(struct ludlc_framing_state *fs)
{
	return LUDLC_CONTAINER_OF(fs, struct hdlc_state, base);
}

/** @brief Same, for the operations that only read it. */
static inline const struct hdlc_state *
fs_to_hdlc_const(const struct ludlc_framing_state *fs)
{
	return LUDLC_CONTAINER_OF(fs, struct hdlc_state, base);
}

/**
 * @enum hdlc_dec_fsm_state
 * @brief Internal states for the decoder FSM.
 */
enum {
	dec_idle = 0, /* Must be 0 */
	dec_wait_sof,
	dec_esc,
	dec_payload,
};

/**
 * @enum hdlc_enc_fsm_state
 * @brief Internal states for the encoder FSM.
 */
enum {
	enc_idle = 0, /* Must be 0 */
	enc_data,
	enc_csum,
	enc_check_send_eof,
	enc_esc,
};

/*
 * The framing's entry points, reached through ludlc_framing_hdlc_ops or, in a
 * build carrying HDLC alone, called by name. See ludlc_framing.h for the
 * contract of each.
 */

void ludlc_hdlc_enc_init(struct ludlc_framing_state *fs)
{
	struct ludlc_henc_state *enc_state = &fs_to_hdlc(fs)->enc;

	enc_state->state = enc_idle;
	enc_state->hdr_size = 0;
	enc_state->payload_size = 0;
	ludlc_platform_clear_bit(ENC_SEND_EOF_F, &fs->flags);
}

void ludlc_hdlc_enc_prep(struct ludlc_framing_state *fs,
			const void *hdr, ludlc_payload_size_t hdr_sz,
			const void *payload, ludlc_payload_size_t pay_sz,
			bool send_eof)
{
	struct ludlc_henc_state *enc = &fs_to_hdlc(fs)->enc;

	enc->hdr = hdr;
	enc->hdr_size = hdr_sz;
	enc->payload = payload;
	enc->payload_size = pay_sz;

	if (send_eof) {
		ludlc_platform_set_bit(ENC_SEND_EOF_F, &fs->flags);
	} else {
		ludlc_platform_clear_bit(ENC_SEND_EOF_F, &fs->flags);
	}
}

bool ludlc_hdlc_enc_busy(const struct ludlc_framing_state *fs)
{
	const struct ludlc_henc_state *enc_state = &fs_to_hdlc_const(fs)->enc;

	return enc_state->state != enc_idle || enc_state->hdr_size != 0;
}

size_t ludlc_hdlc_enc_fill(struct ludlc_framing_state *fs,
			   const struct ludlc_csum *csum_alg,
			   uint8_t *dst, size_t dst_sz)
{
	size_t n;
	struct ludlc_henc_state *enc = &fs_to_hdlc(fs)->enc;

	for (n = 0; n < dst_sz && ludlc_hdlc_enc_busy(fs); ) {
		uint8_t c;

		switch (enc->state) {
		case enc_idle:
		default:
			/* Ready to start a new packet. The field is wider
			 * than the FSM, so a value outside it lands here
			 * too and is taken as idle.
			 */
			if (enc->hdr_size && enc->hdr) {
				enc->state = enc_data;
				enc->ptr = enc->hdr;
				enc->sz = enc->hdr_size;
				if (ludlc_csum_on(csum_alg)) {
					enc->csum =
						ludlc_csum_tx_init(csum_alg);
				}
			}

			dst[n++] = LUDLC_SOF; /* Start of new packet, or idle */
			continue;

		case enc_data:
			/* Sending header or payload data */
			c = *enc->ptr++;

			if (ludlc_csum_on(csum_alg)) {
				enc->csum = ludlc_csum_tx(csum_alg,
							  enc->csum, c);
			}

			if (--enc->sz) {
				break; /* More data in this segment */
			}

			/* Segment finished */
			if (enc->payload_size) {
				/* Header done: the current octet still has to
				 * go out, so arm the payload and fall through
				 * to emit it rather than continue.
				 */
				enc->sz = enc->payload_size;
				enc->ptr = enc->payload;
				enc->payload_size = 0;
				break;
			}

			/* Payload finished (or was zero),
			 * move to checksum (if any)
			 */
			if (ludlc_csum_on(csum_alg)) {
				enc->state = enc_csum;
				enc->csum = ludlc_csum_tx_final(csum_alg,
								enc->csum);
				enc->sz = ludlc_csum_octets(csum_alg);
			} else {
				enc->state = enc_check_send_eof;
			}
			break;

		case enc_csum:
			/* Least significant octet first, shifted out of
			 * the value rather than read from its storage,
			 * so the wire form does not depend on the
			 * host's byte order.
			 */
			c = enc->csum & 0xff;
			enc->csum >>= 8;
			if (--enc->sz == 0) { /* Checksum finished */
				enc->state = enc_check_send_eof;
			}
			break;

		case enc_check_send_eof:
			/* Check and send an optional final SOF to mark
			 * end of packet
			 */
			if (ludlc_platform_test_and_clear_bit(
					ENC_SEND_EOF_F, &fs->flags)) {
				dst[n++] = LUDLC_SOF;
			}

			ludlc_hdlc_enc_init(fs);
			continue;

		case enc_esc: /* Second octet of an escape sequence */
			/* Restore previous state */
			enc->state = enc->esc_state;
			/* Send the escaped octet */
			dst[n++] = enc->esc_data;
			continue;
		}

		/* Check if the selected octet needs escaping */
		if ((c == LUDLC_SOF) || (c == LUDLC_ESC)) {
			/* Save current state */
			enc->esc_state = enc->state;
			enc->state = enc_esc;
			enc->esc_data = c ^ LUDLC_MASK;
			/* Send the escape octet first */
			c = LUDLC_ESC;
		}

		dst[n++] = c;
	}

	return n;
}

void ludlc_hdlc_dec_init(struct ludlc_framing_state *fs)
{
	struct ludlc_hdec_state *dec = &fs_to_hdlc(fs)->dec;

	dec->state = dec_idle;
	dec->size = 0;
}

void ludlc_hdlc_dec_prep(struct ludlc_framing_state *fs,
			 const struct ludlc_csum *csum, void *buf,
			 ludlc_frame_size_t cap)
{
	struct ludlc_hdec_state *dec = &fs_to_hdlc(fs)->dec;

	dec->payload = buf;
	dec->payload_cap = cap;
	if (ludlc_csum_on(csum)) {
		dec->csum = ludlc_csum_rx_init(csum);
	}
	dec->size = 0;
}

/**
 * @brief Process one received data: sync on SOF, de-escape, checksum.
 *
 * @param fs Pointer to the decoder state structure.
 * @param csum Checksum algorithm this link runs.
 * @param conn The link, for its counters.
 * @param src The raw octet received from the serial interface.
 * @return handles data when @p dec_state holds a whole, verified packet.
 */
size_t ludlc_hdlc_dec_feed(struct ludlc_framing_state *fs,
			   const struct ludlc_csum *csum,
			   struct ludlc_connection *conn,
			   const uint8_t *src, size_t len,
			   bool *done)
{
	struct ludlc_hdec_state *dec = &fs_to_hdlc(fs)->dec;
	size_t n;

	for (n = 0; n < len;) {
		uint8_t c = src[n++];

		if (c == LUDLC_SOF) {
			dec->state = dec_wait_sof;
		}

		switch (dec->state) {
		case dec_idle:
			/* Filter out all incoming data until SOF arrived
			* (and the state will be modified in the
			* 'if' expression above)
			*/
			continue;

		case dec_wait_sof:
			/* This is a new packet, triggered by LUDLC_SOF. */

			/* First, process the *previous* one. */
			dec->state = dec_payload;
			if (dec->size >= ludlc_frame_min_sz(csum)) {
				/* the CRC([data][crc]) must be a known constant,
				 * or it's not a crc but something else
				 */
				if (ludlc_csum_on(csum) &&
				    !ludlc_csum_rx_ok(csum, dec->csum)) {
					LUDLC_INC_STATS(conn, bad_csum);
				} else {
					if (ludlc_csum_on(csum)) {
						dec->size -=
							ludlc_csum_octets(csum);
					}
					LUDLC_INC_STATS(conn, rx_packet);
					*done = true;
					return n;
				}
			} else if (dec->size) {
				/* Packet was too short */
				LUDLC_INC_STATS(conn, dropped);
			}

			dec->size = 0;
			if (ludlc_csum_on(csum)) {
				dec->csum = ludlc_csum_rx_init(csum);
			}
			continue;

		case dec_esc:
			/* This octet is an escaped data one */
			dec->state = dec_payload;
			c ^= LUDLC_MASK;
			/* Escaped octet is always literal payload data. */
			break;

		case dec_payload:
			if (c == LUDLC_ESC) {
				/* Start of an escape sequence */
				dec->state = dec_esc;
				continue;
			}
			break;
		}

		/* Store normal data octet. */
		if (dec->size < dec->payload_cap) {
			dec->payload[dec->size++] = c;
			if (ludlc_csum_on(csum)) {
				dec->csum = ludlc_csum_rx(csum, dec->csum, c);
			}
		} else {
			/* Packet is too big (jabber) */
			LUDLC_INC_STATS(conn, jabber);
			ludlc_hdlc_dec_init(fs);
		}
	}

	*done = false;
	return n;
}

ludlc_payload_size_t ludlc_hdlc_dec_size(const struct ludlc_framing_state *fs)
{
	return (ludlc_payload_size_t)fs_to_hdlc_const(fs)->dec.size;
}

static const struct ludlc_framing_ops ludlc_framing_hdlc_ops = {
	LUDLC_FRAMING_OPS(hdlc)
};

static ludlc_frame_size_t
framing_hdlc_encoded_max(const struct ludlc_connection *conn,
			 const struct ludlc_csum *csum)
{
	return ludlc_conn_frame_sz(conn, csum);
}

const struct ludlc_framing ludlc_framing_hdlc = {
	.name = "hdlc",
	.ops = &ludlc_framing_hdlc_ops,
	.needs_wire_len = false,
	.encoded_max = framing_hdlc_encoded_max,
	/* No wire_len: each escaped octet adds one, and which octets those
	 * are is only known once the data has been walked.
	 */
};

#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(ludlc_framing_hdlc);
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

static struct hdlc_state g_hdlc_framing[CONFIG_LUDLC_STATIC_FRAMING_NUM];
static LUDLC_DECLARE_BITSET(g_hdlc_framing_busy,
			    CONFIG_LUDLC_STATIC_FRAMING_NUM);

static struct hdlc_state *hdlc_framing_get(void)
{
	const int slot = ludlc_bitset_claim(g_hdlc_framing_busy,
					    CONFIG_LUDLC_STATIC_FRAMING_NUM);

	return slot < 0 ? NULL : &g_hdlc_framing[slot];
}

static bool hdlc_framing_put(const struct hdlc_state *state)
{
	if (state < &g_hdlc_framing[0] ||
	    state >= &g_hdlc_framing[CONFIG_LUDLC_STATIC_FRAMING_NUM]) {
		return false;
	}

	ludlc_platform_clear_bit(state - g_hdlc_framing, g_hdlc_framing_busy);

	return true;
}

#else /* nothing preallocated */

static inline struct hdlc_state *hdlc_framing_get(void)
{
	return NULL;
}

static inline bool hdlc_framing_put(const struct hdlc_state *state)
{
	(void)state;

	return false;
}

#endif /* CONFIG_LUDLC_STATIC_FRAMING_NUM */

struct ludlc_framing_state *ludlc_create_framing_hdlc(void)
{
	struct hdlc_state *state = hdlc_framing_get();

	if (!state) {
		state = ludlc_platform_alloc(sizeof(*state));
		if (!state) {
			return NULL;
		}
	}

	/* A reused slot still holds the last link's data, so clear all. */
	memset(state, 0, sizeof(*state));

#if LUDLC_FRAMING_COUNT	> 1
	state->base.fops = &ludlc_framing_hdlc;
#endif
	return &state->base;
}
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(ludlc_create_framing_hdlc);
#endif

void ludlc_destroy_framing_hdlc(struct ludlc_framing_state *fs)
{
	if (fs) {
		struct hdlc_state *state = fs_to_hdlc(fs);

		if (!hdlc_framing_put(state)) {
			ludlc_platform_free(state);
		}
	}
}
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(ludlc_destroy_framing_hdlc);
#endif

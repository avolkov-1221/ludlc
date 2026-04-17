// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file ludlc_serial_enc_impl.h
 *
 * @brief LuDLC serial (inlined) encoder/decoder implementation.
 *
 * This file provides the byte-stuffing and framing logic for sending
 * LuDLC packets over a serial byte stream.
 *
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * This file is licensed under either the Apache License, Version 2.0,
 * or the GNU General Public License, version 2 or (at your option)
 * any later version.
 */

#ifndef __LUDLC_SERIAL_ENC_IMPL_H__
#define __LUDLC_SERIAL_ENC_IMPL_H__

#include <stdbool.h>
#include <stdint.h>

/**
 * @def LUDLC_SOF
 * @brief Start Of packet sequence, aka frame boundary.
 *
 * The value 0x55 (0b01010101) is easier to find/check baudrate
 * in the bitstream by oscilloscope than the original HDLC SOF 0x7E (0b01111110).
 */
#define LUDLC_SOF 0x55

/**
 * @def LUDLC_ESC
 * @brief Escape sequence. Ditto. 0xaa is 0b10101010.
 *
 * Any octet matching @ref LUDLC_SOF or @ref LUDLC_ESC in the payload
 * is replaced by @ref LUDLC_ESC followed by the original octet
 * XORed with @ref LUDLC_MASK.
 */
#define LUDLC_ESC 0xaa

/**
 * @def LUDLC_MASK
 * @brief XOR mask used for escaping data octets.
 */
#define LUDLC_MASK 0x20

/**
 * @struct ludlc_sdec_state
 * @brief State machine structure for the serial decoder.
 */
struct ludlc_sdec_state {
	/**< Current size of the incoming packet. */
	ludlc_payload_size_t	size;
	/**< Running checksum of the incoming packet. */
	ludlc_csum_t		csum;

	/**< Current FSM state (see ludlc_serial_decode). */
	uint_fast8_t		state;

	/** @brief Buffer to store the de-escaped incoming packet. */
	uint8_t			*payload;

	ludlc_payload_size_t	payload_cap;
};

/**
 * @struct ludlc_senc_state
 * @brief State machine structure for the serial encoder.
 */
struct ludlc_senc_state {
	/**< Pointer to the packet header to send. */
	const void	*hdr;
	/**< Pointer to the packet payload to send. */
	const void	*payload;
	/**< Internal pointer to the current octet being sent. */
	const uint8_t	*ptr;

	/**< Running checksum of the outgoing packet. */
	ludlc_csum_t	csum;

	/**< Total size of the header. */
	ludlc_payload_size_t	hdr_size;
	/**< Total size of the payload. */
	ludlc_payload_size_t	payload_size;
	/**< Remaining octets in the current segment. */
	ludlc_payload_size_t	sz;
/**
 * @def ENC_SEND_HDR_F
 * @brief Flag indicating the header is being sent.
 */
#define ENC_SEND_HDR_F	(1U<<0)
/**
 * @def ENC_SEND_EOF_F
 * @brief Flag indicating an EOF (SOF) should be sent after the packet.
 */
#define ENC_SEND_EOF_F	(1U<<1)
	/**< Bitmask of encoder flags (ENC_...). */
	uint8_t		flags;

	/**< Current FSM state (see ludlc_serial_encode). */
	uint8_t	state;
	/**< State to return to after sending an escaped octet. */
	uint8_t	esc_state;
	/**< The escaped octet (data ^ MASK) to be sent. */
	uint8_t esc_data;
};

/**
 * @def LUDLC_MIN_PACKET_SZ
 * @brief Minimum valid packet size, which is a PING packet plus checksum.
 */
#define LUDLC_MIN_PACKET_SZ (sizeof(ludlc_ping_packet_t) + sizeof(ludlc_csum_t))

/**
 * @brief Initializes (or resets) the serial decoder state machine.
 *
 * @param p_state Pointer to the decoder state structure.
 */
static inline void ludlc_serial_decoder_init(struct ludlc_sdec_state *dec_state)
{
	dec_state->state = 0;
	dec_state->size  = 0;
}

/**
 * @brief Reinitializes the serial decoder state machine after packet reception.
 *
 * @param dec_state   Pointer to the decoder state structure.
 * @param new_payload Pointer to the new buffer's payload.
 */
static inline void ludlc_serial_decoder_prep(struct ludlc_connection *conn,
		struct ludlc_sdec_state *dec_state,
		void *new_payload, ludlc_payload_size_t cap)
{
	dec_state->size = 0;
	dec_state->csum = conn->csum_init_value;
	dec_state->payload = new_payload;
	dec_state->payload_cap = cap;
}

static inline void
ludlc_serial_decoder_store_byte(struct ludlc_connection *conn,
				struct ludlc_sdec_state *dec_state, uint8_t c)
{
	if (dec_state->size < dec_state->payload_cap) {
		dec_state->payload[dec_state->size++] = c;
		dec_state->csum = conn->proto->csum_byte(dec_state->csum, c);
	} else {
		/* Packet is too large (jabber) */
		dec_state->size = 0;
		dec_state->state = 0; /* dec_idle */
		LUDLC_INC_STATS(conn, jabber);
	}
}

/**
 * @brief Processes one incoming octet through the serial decoder FSM.
 *
 * This function handles packet synchronization (SOF), de-escaping (ESC),
 * checksum calculation, and forwards completed, valid packets to the
 * ludlc_receive() function.
 *
 * @param conn Pointer to the main LuDLC connection structure (for callbacks).
 * @param dec_state Pointer to the decoder state structure.
 * @param c The raw octet received from the serial interface.
 */
static inline bool ludlc_serial_decode(struct ludlc_connection *conn,
		struct ludlc_sdec_state *dec_state, uint8_t c)
{
	/**
	 * @enum ludlc_sdec_fsm_state
	 * @brief Internal states for the decoder FSM.
	 */
	enum {
		dec_idle = 0, /* Must be 0 */
		dec_wait_sof,
		dec_esc,
		dec_payload,
	};

	if (c == LUDLC_SOF) {
		dec_state->state = dec_wait_sof;
	}

	switch(dec_state->state) {
	case dec_idle:
		/* Filter out all incoming data until SOF arrived
		 * (and the state will be modified in the
		 * 'if' expression above)
		 */
		break;
	case dec_wait_sof:
		/* This is a new packet, triggered by LUDLC_SOF. */
		/* First, process the *previous* one. */
		dec_state->state = dec_payload;
		if (dec_state->size >= LUDLC_MIN_PACKET_SZ) {
			/* the CRC([data][crc]) must be a known constant,
			 * or it's not a crc but something else
			 */
			if (dec_state->csum == conn->csum_verify_value) {
				LUDLC_INC_STATS(conn, rx_packet);
				dec_state->size -= sizeof(ludlc_csum_t);
				return true;
			} else {
				LUDLC_INC_STATS(conn, bad_csum);
			}
		} else if (dec_state->size) {
			/* Packet was too short */
			LUDLC_INC_STATS(conn, dropped);
		}

		ludlc_serial_decoder_prep(conn,
					  dec_state,
					  dec_state->payload,
					  dec_state->payload_cap);
		break;
	case dec_esc:
		/* This octet is an escaped data one */
		dec_state->state = dec_payload;
		c ^= LUDLC_MASK;
		/* Escaped octet is always literal payload data. */
		ludlc_serial_decoder_store_byte(conn, dec_state, c);
		break;
	case dec_payload:
		if (c == LUDLC_ESC) {
			/* Start of an escape sequence */
			dec_state->state = dec_esc;
			break;
		}
		/* Store normal data octet. */
		ludlc_serial_decoder_store_byte(conn, dec_state, c);
		break;
	}

	return false;
}

/**
 * @brief Retrieves the next octet to be transmitted from the encoder FSM.
 *
 * This function is called repeatedly by the transport layer to get the
 * next octet to send. It manages the state machine for sending SOF,
 * header, payload, checksum (with escaping), and an optional EOF.
 *
 * @param conn Pointer to the main LuDLC connection structure (for checksum).
 * @param enc_state Pointer to the encoder state structure.
 * @return The next octet to be written to the serial interface.
 */
static inline uint8_t ludlc_serial_encode(struct ludlc_connection *conn,
		struct ludlc_senc_state *enc_state)
{
	/**
	 * @enum ludlc_senc_fsm_state
	 * @brief Internal states for the encoder FSM.
	 */
	enum {
		enc_idle = 0, /* Must be 0 */
		enc_payload,
		enc_csum,
		enc_eof,
		enc_esc
	};
	uint8_t c = 0;

	switch (enc_state->state) {
	case enc_idle:
		/* Ready to start a new packet */
		if (!enc_state->hdr_size || !enc_state->hdr) {
			return LUDLC_SOF; /* Send idle SOFs if no data */
		}

		enc_state->csum = conn->csum_init_value;
		enc_state->state = enc_payload;
		enc_state->flags |= ENC_SEND_HDR_F;

		enc_state->sz  = enc_state->hdr_size;
		enc_state->ptr = enc_state->hdr;

		return LUDLC_SOF; /* Start of new packet */

	case enc_payload:
		/* Sending header or payload data */
		c = *enc_state->ptr++;
		enc_state->csum = conn->proto->csum_byte(enc_state->csum, c);

		if (--enc_state->sz != 0)
			break; /* More data in this segment */

		/* Segment finished */
		if (enc_state->flags & ENC_SEND_HDR_F) {
			/* Header finished, move to payload */
			enc_state->flags &= ~ENC_SEND_HDR_F;
			enc_state->sz = enc_state->payload_size;
			enc_state->ptr = enc_state->payload;
		}

		if (!enc_state->sz) {
			/* Payload finished (or was zero), move to checksum */
			enc_state->state = enc_csum;
			enc_state->csum = conn->csum_to_wire ?
				conn->csum_to_wire(enc_state->csum) :
				LUDLC_CSUM_HTON(enc_state->csum);
			enc_state->ptr = (const void*)&enc_state->csum;
			enc_state->sz = sizeof(enc_state->csum);
		}
		break;

	case enc_csum:
		/* Sending checksum bytes */
		c = *enc_state->ptr++;
		if (--enc_state->sz == 0) {
			/* Checksum finished */
			if (enc_state->flags & ENC_SEND_EOF_F) {
				enc_state->state = enc_eof;
			} else {
				enc_state->state = enc_idle;
			}
		}
		break;

	case enc_eof:
		/* Send an optional final SOF to mark end of packet */
		enc_state->state = enc_idle;
		return LUDLC_SOF;

	case enc_esc: /* Second octet of an escape sequence */
		/* Restore previous state */
		enc_state->state = enc_state->esc_state;
		/* Send the escaped octet */
		return enc_state->esc_data;
	}

	/* Check if the selected octet needs escaping */
	if ((c == LUDLC_SOF) || (c == LUDLC_ESC)) {
		/* Save current state */
		enc_state->esc_state = enc_state->state;
		enc_state->state = enc_esc;
		enc_state->esc_data = c ^ LUDLC_MASK;
		/* Send the escape octet first */
		c = LUDLC_ESC;
	}
	return c;
}

/**
 * @brief Checks if the serial encoder FSM is in the idle state.
 *
 * @param p_state Pointer to the encoder state structure.
 * @return true if the encoder is idle (ready for a new packet), false otherwise.
 */
static inline bool ludlc_serial_encoder_idle(struct ludlc_senc_state *p_state)
{
	return p_state->state == 0;
}

/**
 * @brief Configures the encoder to send (or not send) an EOF marker.
 *
 * An EOF marker is just an extra @ref LUDLC_SOF byte sent *after* the
 * checksum to explicitly delimit the end of the frame.
 *
 * @param p_state Pointer to the encoder state structure.
 * @param send true to enable sending an EOF, false to disable.
 */
static inline void ludlc_serial_encoder_send_eof(
		struct ludlc_senc_state *p_state,
		bool send)
{
	if (send)
		p_state->flags |= ENC_SEND_EOF_F;
	else
		p_state->flags &= ~ENC_SEND_EOF_F;
}

/**
 * @brief Initializes (or resets) the serial encoder state machine.
 *
 * @param p_state Pointer to the encoder state structure.
 */
static inline void ludlc_serial_encoder_init(struct ludlc_senc_state *p_state)
{
	p_state->state = 0;
	p_state->flags = 0;
	p_state->hdr_size = 0;
}

/**
 * @brief Checks if the serial encoder FSM is in the idle state.
 *
 * @param p_state Pointer to the encoder state structure.
 * @return true if the encoder is idle (ready for a new packet), false otherwise.
 */
static inline bool ludlc_serial_encoder_packet_sz(
		struct ludlc_senc_state *enc_state)
{

	return enc_state->hdr_size + enc_state->payload_size;
}

#endif /* __LUDLC_SERIAL_ENC_IMPL_H__ */

// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only)
/**
 * @file ludlc_private.h
 *
 * @brief Internal definitions for the LuDLC protocol implementation.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 */

#ifndef __LUDLC_PRIVATE_H__
#define __LUDLC_PRIVATE_H__

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#endif

#include <ludlc_platform_api.h>
#include <ludlc_packet.h>
#include <ludlc_logger.h>

/* Borrowed from kernel */
/*
 * Add the pseudo keyword 'fallthrough' so case statement blocks
 * must end with any of these keywords:
 *   break;
 *   fallthrough;
 *   continue;
 *   goto <label>;
 *   return [expression];
 *
 *  gcc: https://gcc.gnu.org/onlinedocs/gcc/Statement-Attributes.html#Statement-Attributes
 */
#ifdef __has_attribute
# if __has_attribute(__fallthrough__)
#  define fallthrough			__attribute__((__fallthrough__))
# endif
#endif
#ifndef fallthrough
# define fallthrough			do {} while (0)	/* fallthrough */
#endif

/**
 * @enum ludlc_conn_state
 * @brief Defines the states of the LuDLC connection finite state machine (FSM).
 */
enum ludlc_conn_state
{
	/** Initial and final state, no connection. */
	LUDLC_STATE_DISCONNECTED = 0,
	/** Sent first handshake packet, awaiting reply. */
	LUDLC_STATE_WAIT_CONN_1  = 1,
	/** Received first reply, sent second, awaiting final. */
	LUDLC_STATE_WAIT_CONN_2  = 2,
	/** Handshake complete, connection established. */
	LUDLC_STATE_CONNECTED    = 3,
	/** State to signal termination (not part of handshake). */
	LUDLC_STATE_TERMINATE    = 0xff,
};

/**
 * @struct ludlc_stats
 * @brief A structure to hold connection statistics (if enabled).
 */
struct ludlc_stats {
	uint32_t rx_packet;  /**< Count of valid received packets. */
	uint32_t tx_packet;  /**< Count of transmitted packets. */
	uint32_t bad_csum;   /**< Count of packets received with bad csum. */
	uint32_t jabber;   /**< Count of excessively large/malformed packets. */
	uint32_t on_conn;    /**< Connection established counter. */
	uint32_t on_disconn; /**< Connection lost/disconnected counter. */
	uint32_t dropped;    /**< Count of dropped packets. */
	uint32_t overrun;    /**< Count of rx overruns. */
};

#ifndef CONFIG_LUDLC_STATS
#define CONFIG_LUDLC_STATS 1
#endif
#if CONFIG_LUDLC_STATS
/**
 * @def LUDLC_DECLARE_STATS(var)
 * @brief Declares the statistics structure within a connection object.
 */
#define LUDLC_DECLARE_STATS(var)	struct ludlc_stats	var
/**
 * @def LUDLC_INC_STATS(state, var)
 * @brief Increments a specific statistic counter.
 */
#define LUDLC_INC_STATS(state, var)	((state)->stats.var++)
#else
#define LUDLC_DECLARE_STATS(var)
#define LUDLC_INC_STATS(state, var)
#endif

/**
 * @struct packet_queue_item_status
 * @brief Used in the confirmation queue/callback to report packet status.
 *
 * @note The size of this structure is designed to match @ref ludlc_packet_hdr_t
 * to fit within the `packet_queue_item` union.
 */
struct packet_queue_item_status {
	/** 0 on success, or an error code (e.g., EPIPE, EREMOTEIO). */
	ludlc_id_t result;
	/** Reserved for alignment, matches `ack_id`. */
	ludlc_id_t reserved;
	/** The channel the packet was sent on. */
	ludlc_channel_t	 chan;
};

/**
 * @struct packet_queue_item
 * @brief Represents a packet in the transmit window (sliding window queue).
 */
struct packet_queue_item {
	/** Pointer to the payload data buffer. */
	const void		*payload;
	/** Size of the payload data. */
	ludlc_payload_size_t	size;
	union {
		/** Header used for transmission. */
		ludlc_packet_hdr_t 	hdr;
		/** Status used for confirmation. */
		struct packet_queue_item_status status;
	};
	/** Time-To-Live (remaining retries). */
	uint8_t			ttl;
};

/**
 * @struct ludlc_connection
 * @brief Main structure representing a single LuDLC connection.
 *
 * This structure holds the complete state, callbacks, timers, and queues
 * required to manage one LuDLC link.
 */
struct ludlc_connection {
	/** User event callbacks. */
	const struct ludlc_conn_cb	*cb;
	/** User context pointer for callbacks. */
	void				*user_ctx;

	/** Connection-wide mutex lock. */
	LUDLC_DECLARE_LOCK(lock);

	/** Timer for sending keep-alive pings. */
	ludlc_platform_timer_t		ping_timer;
	/** Watchdog timer for connection timeout. */
	ludlc_platform_timer_t		wd_timer;

/** @def LUDLC_CONN_NO_NAK_F
 * @brief Clear when the peer is to be told a packet went missing.
 *
 * Active low, as @ref LUDLC_CONN_U_IDLE_F explains. */
#define LUDLC_CONN_NO_NAK_F		0
/** @def LUDLC_CONN_INITED_F
 * @brief Atomic flag bit indicating the connection is inited. */
#define LUDLC_CONN_INITED_F		1
/** @def LUDLC_CONN_DISC_AFTER_TX_F
 * @brief The unnumbered packet is the core's own DISCONNECT.
 *
 * Marks the packet as belonging to the core rather than a caller, so that
 * draining it ends the session instead of confirming a buffer. */
#define LUDLC_CONN_DISC_AFTER_TX_F	2
/** @def LUDLC_CONN_U_IDLE_F
 * @brief Clear while an unnumbered packet waits to be sent.
 *
 * Active low, so that taking it is one @ref ludlc_platform_test_and_set_bit.
 * Not every target has an instruction that tests and clears in one go, and a
 * test plus a clear is two operations. Starts set; see
 * @ref LUDLC_CONN_IDLE_MASK. */
#define LUDLC_CONN_U_IDLE_F		3
/** @def LUDLC_CONN_U_DONE_F
 * @brief Clear while the transport is still reading the unnumbered packet.
 *
 * The transport reads the caller's buffer after the lock is released, so
 * the slot stays in use until the next frame is requested. The previous
 * frame has been sent by then, and the buffer can be returned. */
#define LUDLC_CONN_U_DONE_F		4
/** @def LUDLC_CONN_BUS_OUT_F
 * @brief A disconnect is failing the queue, and the sequence is not reset yet.
 *
 * The reset waits until every failed packet has been reported, which may be
 * several passes after the sweep. A packet accepted in between would be
 * dropped by it without a confirmation, so the queue is closed for the whole
 * span. See @ref ludlc_seq_reset_if_drained. */
#define LUDLC_CONN_BUS_OUT_F		5
/** @def LUDLC_CONN_U_CONFIRM_F
 * @brief The unnumbered packet's confirmation is waiting in @c u_confirm.
 *
 * It has no sequence number and no window slot, so it is reported from a slot
 * of its own rather than from the range the numbered packets use. */
#define LUDLC_CONN_U_CONFIRM_F		6

	/** @brief The flags above, read and written under @c lock.
	 *
	 * The active-low ones are taken with a single
	 * @ref ludlc_platform_test_and_set_bit; the rest are plain set and
	 * clear, the lock being what keeps the word consistent. */
	ludlc_platform_atomic_t		flags;

	/**
	 * @brief Protocol settings for this link, its own copy.
	 *
	 * Filled at init from the caller's cfg, or from
	 * @ref ludlc_conn_cfg_default. Read through the ludlc_conn_*()
	 * accessors below, never through a borrowed pointer: the caller's
	 * cfg may be a compound literal that is out of scope by the time a
	 * packet is sent. A single-connection build resolves most of these at
	 * compile time, leaving only what a port still selects per link.
	 */
	struct ludlc_conn_cfg		cfg;

#if CONFIG_LUDLC_MULTI_CONN
	/**
	 * @brief The sliding window packet queue.
	 *
	 * @ref ludlc_conn_window() slots, allocated at init. A pointer rather
	 * than an array because the window size is only known at runtime
	 * here. A confirmed packet stays in its slot until its callback has
	 * run; see @c last_reported.
	 */
	struct packet_queue_item	*packets_q;
#else
	/**
	 * @brief The sliding window packet queue.
	 *
	 * Sized for the build's largest window; this link uses only the first
	 * @ref ludlc_conn_window() slots.
	 */
	struct packet_queue_item	packets_q[CONFIG_LUDLC_WINDOW];
#endif

	/** Pre-allocated header for control packets (PING, Handshake). */
	ludlc_packet_hdr_t		ctrl_packet;

	/** @brief The pending unnumbered packet, see the flags above.
	 *
	 * A single item rather than a queue, because it uses no window slot.
	 * Its payload is the caller's buffer, as for a queued packet. */
	struct packet_queue_item	u_packet;

#ifdef CONFIG_LUDLC_MT
	/** @brief The unnumbered packet's confirmation, waiting to be run.
	 *
	 * A copy, not a reference: @c u_packet is reused for the next
	 * unnumbered packet as soon as this one is resolved. Only one can be
	 * outstanding. Valid while @ref LUDLC_CONN_U_CONFIRM_F is set. */
	struct packet_queue_item	u_confirm;
#endif

	/** Connection statistics. */
	LUDLC_DECLARE_STATS(stats);

	/** @brief The platform specific extension. */
	struct ludlc_platform_connection pconn;

	/** ID of the last packet enqueued by the user. */
	ludlc_id_t	last_queued;
	/** ID of the last packet actually sent on the wire. */
	ludlc_id_t	last_sent;
	/** ID of the last packet acknowledged by the peer. */
	ludlc_id_t	last_ack;
	/** ID of the last valid, in-sequence packet received. */
	ludlc_id_t	last_received;

#ifdef CONFIG_LUDLC_MT
	/** @brief ID of the last packet whose confirmation has been reported.
	 *
	 * Everything in <tt>(last_reported, last_ack]</tt> is resolved and
	 * waiting for its callback, still in the window slot it was sent
	 * from; @ref ludlc_confirm_unlocked walks that range. A range works
	 * because every path resolves packets in sequence from @c last_ack
	 * upwards, so nothing has to be stored to know what is pending. A
	 * build that reports inline has no gap and no counter; see
	 * @ref ludlc_conn_last_reported. */
	ludlc_id_t	last_reported;

/** @def LUDLC_REPORT_BUSY_F
 * @brief Set while a thread is reporting confirmations.
 *
 * Not needed for the range, which is read and advanced under the lock, but
 * for the order the callbacks run in: two threads each holding a batch would
 * race. A separate word from @c flags, which is written whole under the lock
 * and would lose it. Never waited on. */
#define LUDLC_REPORT_BUSY_F		0

	/** @ref LUDLC_REPORT_BUSY_F lives here. */
	ludlc_platform_atomic_t		report_busy;
#endif /* CONFIG_LUDLC_MT */

	/** Current connection FSM state (see @ref ludlc_conn_state). */
	uint8_t		conn_state;
};

/**
 * @brief Processes a received LuDLC packet from the transport layer.
 * @see ludlc_receive() implementation in ludlc.c
 */
int ludlc_receive(struct ludlc_connection *conn,
			ludlc_packet_t *packet, size_t pkt_sz,
			ludlc_timestamp_t tstamp);

/**
 * @brief Retrieves the next packet to be sent by the transport layer.
 * @see ludlc_get_packet_to_send() implementation in ludlc.c
 */
int ludlc_get_packet_to_send(struct ludlc_connection *conn,
		const void **hdr, ludlc_payload_size_t *hdr_sz,
		const void **payload, ludlc_payload_size_t *pay_sz);

/**
 * @brief Handles transport-side disconnect/timeout events in core.
 *
 * This API performs a state-safe disconnect transition, fails queued packets,
 * and notifies user callbacks once (outside lock).
 *
 * @param conn The connection to disconnect.
 *
 * @note This helper is idempotent and safe to call on repeated transport
 * error/timeout notifications.
 */
void ludlc_handle_disconnect(struct ludlc_connection *conn);

/** @brief Restarts the watchdog for another period. */
void ludlc_watchdog_rearm(struct ludlc_connection *conn);

/**
 * @name Per-connection protocol settings
 *
 * Macros rather than functions because with one connection per build there is
 * nothing to read: each expands to the CONFIG_* constant it was built with,
 * so a window mask becomes an immediate and the connection stores no settings
 * at all. With @ref CONFIG_LUDLC_MULTI_CONN the same expressions become one
 * load from the link's own copy.
 *
 * Each takes the connection, evaluates it once, and ignores it where the
 * answer is a constant, which is what the cast to void is for.
 * @{
 */

#if CONFIG_LUDLC_MULTI_CONN

/** @brief Largest payload this link carries, in octets. Never zero. */
#define ludlc_conn_payload_lim(conn)	((conn)->cfg.max_payload)
/** @brief This link's window size, in packets (a power of two). */
#define ludlc_conn_window(conn)		((unsigned int)(conn)->cfg.window)
/** @brief Mask for indexing @c packets_q with this link's window. */
#define ludlc_conn_window_mask(conn)	((unsigned int)(conn)->cfg.window - 1U)
/** @brief Keepalive period for this link, in microseconds. */
#define ludlc_conn_ping_time(conn)	((unsigned long)(conn)->cfg.ping_time)
/** @brief TTL this link applies where the caller names none. */
#define ludlc_conn_default_ttl(conn)	((uint8_t)(conn)->cfg.default_ttl)

#else /* one connection: the settings are the build's constants */

#define ludlc_conn_payload_lim(conn)					\
	((void)(conn), (ludlc_payload_size_t)CONFIG_LUDLC_MAX_PAYLOAD_SIZE)
#define ludlc_conn_window(conn)						\
	((void)(conn), (unsigned int)CONFIG_LUDLC_WINDOW)
#define ludlc_conn_window_mask(conn)					\
	((void)(conn), (unsigned int)LUDLC_WINDOW_MASK)
#define ludlc_conn_ping_time(conn)					\
	((void)(conn), (unsigned long)CONFIG_LUDLC_PING_TIME)
#define ludlc_conn_default_ttl(conn)					\
	((void)(conn), (uint8_t)LUDLC_DEFAULT_TTL)

#endif /* CONFIG_LUDLC_MULTI_CONN */

/** @brief Watchdog period for this link: three keepalive periods. */
#define ludlc_conn_wd_time(conn)	(3UL * ludlc_conn_ping_time(conn))
/** @} */

/**
 * @name Reporting confirmations
 * @{
 */

/**
 * @def CONFIG_LUDLC_CONFIRM_BATCH
 * @brief Confirmations carried out of the lock per acquisition of it.
 *
 * Bounds the stack, not the work: @ref ludlc_confirm_unlocked copies this
 * many slots onto the reporting thread's stack, releases the lock, runs their
 * callbacks and repeats while any remain. Set it to the window for a single
 * pass, or leave it lower for a window of tens or hundreds. Costs this many
 * @c struct @c packet_queue_item of stack while draining.
 */
#ifndef CONFIG_LUDLC_CONFIRM_BATCH
#define CONFIG_LUDLC_CONFIRM_BATCH	8
#endif
#if (CONFIG_LUDLC_CONFIRM_BATCH) < 1
#error Please define CONFIG_LUDLC_CONFIRM_BATCH as one or more
#endif

/**
 * @brief ID of the last packet whose confirmation the user has seen.
 *
 * The window may not advance past it; see @ref ludlc_tx_queue_full. A build
 * that reports inline uses the last acknowledged ID and stores no counter.
 */
#ifdef CONFIG_LUDLC_MT
#define ludlc_conn_last_reported(conn)	((conn)->last_reported)
#else
#define ludlc_conn_last_reported(conn)	((conn)->last_ack)
#endif

/** @brief The active-low flags, as they stand with nothing outstanding.
 *
 * Zeroed memory reads as all of them pending, so init and the disconnect
 * sweep set them. */
#define LUDLC_CONN_IDLE_MASK						\
	((ludlc_platform_atomic_t)((1U << LUDLC_CONN_NO_NAK_F) |	\
				   (1U << LUDLC_CONN_U_IDLE_F) |	\
				   (1U << LUDLC_CONN_U_DONE_F)))

/** @brief Flags the disconnect sweep keeps when it clears the runtime ones. */
#ifdef CONFIG_LUDLC_MT
#define LUDLC_CONN_SWEEP_KEEP_MASK					\
	((1U << LUDLC_CONN_INITED_F) | (1U << LUDLC_CONN_BUS_OUT_F) |	\
	 (1U << LUDLC_CONN_U_CONFIRM_F))
#else
#define LUDLC_CONN_SWEEP_KEEP_MASK					\
	((1U << LUDLC_CONN_INITED_F) | (1U << LUDLC_CONN_BUS_OUT_F))
#endif

/** @brief Flags the disconnect sweep clears: whatever it does not keep.
 *
 * Written as what is left over so that a flag added later is cleared without
 * anyone remembering to name it here. */
#define LUDLC_CONN_SWEEP_CLEAR_MASK					\
	((ludlc_platform_atomic_t)~(LUDLC_CONN_SWEEP_KEEP_MASK |	\
				    LUDLC_CONN_IDLE_MASK))

/** @} */

/**
 * @def CONFIG_LUDLC_SINGLE_CSUM
 * @brief The build runs one checksum, which the port computes inline.
 *
 * The checksum counterpart of a build carrying a single framing, which calls
 * it by name. The escaping framing and the frame sizes below then only check
 * the struct ludlc_csum they are passed for NULL, a link running none, and
 * otherwise call the port's hooks, which on a target with a CRC unit can
 * become register accesses. Each direction has its own:
 * ludlc_platform_csum_tx_init(), ludlc_platform_csum_tx() and
 * ludlc_platform_csum_tx_final() on transmit, ludlc_platform_csum_rx_init(),
 * ludlc_platform_csum_rx() and ludlc_platform_csum_rx_ok() on receive, and
 * LUDLC_PLATFORM_CSUM_OCTETS for the width. The framing never reads the value
 * it passes between them, so one direction may own a CRC unit and keep its
 * state there while the other computes in software.
 */
#ifndef CONFIG_LUDLC_SINGLE_CSUM
#define CONFIG_LUDLC_SINGLE_CSUM	0
#endif

#if CONFIG_LUDLC_SINGLE_CSUM
#if !defined(LUDLC_PLATFORM_CSUM_OCTETS)
#error "CONFIG_LUDLC_SINGLE_CSUM: the port provides no inline checksum"
#endif

/** @brief Whether @p alg runs at all. NULL means no checksum. */
static inline bool ludlc_csum_on(const struct ludlc_csum *alg)
{
	return alg != NULL;
}

/** @brief How many octets the checksum takes on the wire, zero for none. */
static inline uint8_t ludlc_csum_octets(const struct ludlc_csum *alg)
{
	return alg ? LUDLC_PLATFORM_CSUM_OCTETS : 0;
}

/** @brief Start the checksum of a frame about to be sent. */
static inline ludlc_csum_t ludlc_csum_tx_init(const struct ludlc_csum *alg)
{
	(void)alg;
	return ludlc_platform_csum_tx_init();
}

/** @brief Add @p data, an octet being sent, to @p csum. */
static inline ludlc_csum_t ludlc_csum_tx(const struct ludlc_csum *alg,
					 ludlc_csum_t csum, uint8_t data)
{
	(void)alg;
	return ludlc_platform_csum_tx(csum, data);
}

/** @brief The checksum of the frame sent, as it goes on the wire. */
static inline ludlc_csum_t ludlc_csum_tx_final(const struct ludlc_csum *alg,
					       ludlc_csum_t csum)
{
	(void)alg;
	return ludlc_platform_csum_tx_final(csum);
}

/** @brief Start the checksum of a frame about to be received. */
static inline ludlc_csum_t ludlc_csum_rx_init(const struct ludlc_csum *alg)
{
	(void)alg;
	return ludlc_platform_csum_rx_init();
}

/** @brief Add @p data, an octet received, to @p csum. */
static inline ludlc_csum_t ludlc_csum_rx(const struct ludlc_csum *alg,
					 ludlc_csum_t csum, uint8_t data)
{
	(void)alg;
	return ludlc_platform_csum_rx(csum, data);
}

/** @brief Whether the frame received, its checksum included, is intact. */
static inline bool ludlc_csum_rx_ok(const struct ludlc_csum *alg,
				    ludlc_csum_t csum)
{
	(void)alg;
	return ludlc_platform_csum_rx_ok(csum);
}
#else
/** @brief Whether @p alg runs at all. NULL means no checksum. */
static inline bool ludlc_csum_on(const struct ludlc_csum *alg)
{
	return alg != NULL;
}

/** @brief How many octets the checksum takes on the wire, zero for none. */
static inline uint8_t ludlc_csum_octets(const struct ludlc_csum *alg)
{
	return alg ? alg->octets : 0;
}

/** @brief Start the checksum of a frame about to be sent. */
static inline ludlc_csum_t ludlc_csum_tx_init(const struct ludlc_csum *alg)
{
	return alg->init;
}

/** @brief Add @p data, an octet being sent, to @p csum. */
static inline ludlc_csum_t ludlc_csum_tx(const struct ludlc_csum *alg,
					 ludlc_csum_t csum, uint8_t data)
{
	return alg->csum(csum, data);
}

/** @brief The checksum of the frame sent, as it goes on the wire. */
static inline ludlc_csum_t ludlc_csum_tx_final(const struct ludlc_csum *alg,
					       ludlc_csum_t csum)
{
	return alg->to_wire ? alg->to_wire(csum) : csum;
}

/** @brief Start the checksum of a frame about to be received. */
static inline ludlc_csum_t ludlc_csum_rx_init(const struct ludlc_csum *alg)
{
	return alg->init;
}

/** @brief Add @p data, an octet received, to @p csum. */
static inline ludlc_csum_t ludlc_csum_rx(const struct ludlc_csum *alg,
					 ludlc_csum_t csum, uint8_t data)
{
	return alg->csum(csum, data);
}

/** @brief Whether the frame received, its checksum included, is intact. */
static inline bool ludlc_csum_rx_ok(const struct ludlc_csum *alg,
				    ludlc_csum_t csum)
{
	return csum == alg->verify;
}
#endif

/**
 * @brief Minimum useful frame: a PING packet plus the checksum.
 *
 * A protocol limit rather than a framing one: every framing rejects anything
 * shorter. The width comes from @p csum_alg, not from @ref ludlc_csum_t,
 * which is only the register the checksum is computed in.
 *
 * @param csum_alg Checksum this link runs, or NULL for none.
 * @return Smallest acceptable decoded frame size, in octets.
 */
static inline size_t
ludlc_frame_min_sz(const struct ludlc_csum *csum_alg)
{
	size_t sz = ludlc_csum_on(csum_alg) ? ludlc_csum_octets(csum_alg) : 0;

	return sz + sizeof(ludlc_ping_packet_t);
}

/**
 * @brief Largest frame this link puts on the wire, before any framing.
 *
 * The link's payload limit, plus a header and @p csum_alg. Only the framings
 * and the transports need it. The core works in payloads.
 *
 * @param conn The connection.
 * @param csum_alg Checksum this link runs, or NULL for none.
 * @return Octets, never zero.
 */
static inline ludlc_frame_size_t
ludlc_conn_frame_sz(const struct ludlc_connection *conn,
		    const struct ludlc_csum *csum_alg)
{
	return (ludlc_frame_size_t)
		LUDLC_FRAME_SIZE(ludlc_conn_payload_lim(conn),
				 ludlc_csum_octets(csum_alg));
}

/**
 * @brief The payload a cfg asks for, resolved as ludlc_connection_init() would.
 *
 * A port reports this size when the link fails to open, since there is then
 * no connection to read it back from.
 *
 * @param cfg The cfg, or NULL for the build's own.
 * @return Octets, never zero.
 */
static inline ludlc_payload_size_t
ludlc_cfg_max_payload(const struct ludlc_conn_cfg *cfg)
{
#if CONFIG_LUDLC_MULTI_CONN
	if (cfg && cfg->max_payload)
		return cfg->max_payload;
#else
	(void)cfg;
#endif

	return (ludlc_payload_size_t)CONFIG_LUDLC_MAX_PAYLOAD_SIZE;
}

struct ludlc_framing_state;

#endif /* __LUDLC_PRIVATE_H__ */

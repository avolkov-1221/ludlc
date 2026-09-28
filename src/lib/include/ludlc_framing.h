// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only)
/**
 * @file ludlc_framing.h
 *
 * @brief How a link turns packets into octets and back.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * A framing is the escaping, the stuffing or nothing at all. Which one a link
 * runs is fixed when the link is opened, so two links in one image may run
 * different ones. The choice is a pointer to a @ref ludlc_framing, which a
 * transport keeps beside its connection and passes to @ref ludlc_enc_init and
 * @ref ludlc_dec_init.
 *
 * No frame is buffered here. The encoder fills the buffer the transport was
 * going to send anyway and the decoder takes the octets just received, so a
 * link carrying a megabyte costs no more than one carrying eight octets.
 *
 * The state machines are in ludlc_hdlc.c, ludlc_cobs.c and ludlc_raw.c,
 * reached through @ref ludlc_framing_ops. A build carrying a single framing
 * calls them by name instead.
 */

#ifndef __LUDLC_FRAMING_H__
#define __LUDLC_FRAMING_H__

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#endif

#include <ludlc_private.h>

struct ludlc_framing;
struct ludlc_framing_ops;

/** @brief How many are built, which decides whether a lookup is needed. */
#define LUDLC_FRAMING_COUNT				\
	(CONFIG_LUDLC_FRAMING_HDLC +			\
	 CONFIG_LUDLC_FRAMING_COBS +			\
	 CONFIG_LUDLC_FRAMING_RAW)

#if LUDLC_FRAMING_COUNT == 0
#error A build needs at least one framing
#endif

/**
 * @struct ludlc_framing_state
 * @brief One link's framing state.
 *
 * A codec's encoder and decoder are private to its own .c file, so use
 * @ref ludlc_framing_create and @ref ludlc_framing_destroy to allocate one
 * and the wrappers at the end of this header to work on it.
 *
 * With several framings built the state starts with the descriptor it runs,
 * which is what those wrappers dispatch on. With one there is nothing to
 * record, so this is an opaque handle on whatever the codec keeps.
 */

#define LUDLC_FS_LAST_PUBLIC_BIT	0

struct ludlc_framing_state {
	ludlc_platform_atomic_t flags;
#if LUDLC_FRAMING_COUNT > 1
	/**< The framing this link runs. Shared by all links running it. */
	const struct ludlc_framing *fops;
#endif
};

/**
 * @def LUDLC_FRAMING_OP_LIST
 * @brief Every entry point a framing provides, with its signature.
 *
 * Each operation would otherwise be written out in three places - a prototype
 * in the codec's header, a pointer in @ref ludlc_framing_ops and an entry in
 * the codec's ops table - where a copy that disagrees with the others is only
 * caught in a build carrying more than one framing. All three are expanded
 * from here instead. @p _ is applied to each operation as (name, return type,
 * operation, parameter list), where @p name is the codec's infix.
 */
#define LUDLC_FRAMING_OP_LIST(_, name)					\
	_(name, void, enc_init, (struct ludlc_framing_state *fs))	\
	_(name, void, enc_prep,						\
	  (struct ludlc_framing_state *fs,				\
	   const void *hdr, ludlc_payload_size_t hdr_sz,		\
	   const void *payload, ludlc_payload_size_t pay_sz,		\
	   bool send_eof))						\
	_(name, bool, enc_busy, (const struct ludlc_framing_state *fs))	\
	_(name, size_t, enc_fill,					\
	  (struct ludlc_framing_state *fs, const struct ludlc_csum *csum, \
	   uint8_t *dst, size_t cap))					\
	_(name, void, dec_init, (struct ludlc_framing_state *fs))	\
	_(name, void, dec_prep,						\
	  (struct ludlc_framing_state *fs, const struct ludlc_csum *csum, \
	   void *buf, ludlc_frame_size_t cap))				\
	_(name, size_t, dec_feed,					\
	  (struct ludlc_framing_state *fs, const struct ludlc_csum *csum, \
	   struct ludlc_connection *conn,				\
	   const uint8_t *src, size_t len, bool *done))			\
	_(name, ludlc_payload_size_t, dec_size,				\
	  (const struct ludlc_framing_state *fs))

/** @brief One prototype, named for the codec: ludlc_cobs_enc_init and so on. */
#define LUDLC_FRAMING_OP_DECL(name, ret, op, params)			\
	ret ludlc_##name##_##op params;

/** @brief One @ref ludlc_framing_ops member. The codec's name is unused. */
#define LUDLC_FRAMING_OP_PTR(name, ret, op, params)			\
	ret (*op) params;

/**
 * @def LUDLC_DECLARE_FRAMING
 * @brief Declare the entry points of the framing called @p name.
 *
 * A codec header uses this instead of writing the prototypes out. Each
 * operation is documented on its wrapper in ludlc_framing.h. create() and
 * destroy() are added here rather than to the op list: they take no state.
 */
#define LUDLC_DECLARE_FRAMING(name)					\
	LUDLC_FRAMING_OP_LIST(LUDLC_FRAMING_OP_DECL, name)		\
	struct ludlc_framing_state *ludlc_create_framing_##name(void);	\
	void ludlc_destroy_framing_##name(struct ludlc_framing_state *fs);

/** @brief One ops-table entry, pointing at the codec's function. */
#define LUDLC_FRAMING_OP_INIT(name, ret, op, params)			\
	.op = ludlc_##name##_##op,

/**
 * @def LUDLC_FRAMING_OPS
 * @brief Body of the @ref ludlc_framing_ops table for the framing @p name.
 *
 * For a codec to use as the initialiser, so the table cannot be missing an
 * operation or hold one belonging to another codec.
 */
#define LUDLC_FRAMING_OPS(name)						\
	LUDLC_FRAMING_OP_LIST(LUDLC_FRAMING_OP_INIT, name)		\
	.create = ludlc_create_framing_##name,				\
	.destroy = ludlc_destroy_framing_##name,

#if CONFIG_LUDLC_FRAMING_HDLC
#include <ludlc_hdlc.h>
#endif
#if CONFIG_LUDLC_FRAMING_COBS
#include <ludlc_cobs.h>
#endif
#if CONFIG_LUDLC_FRAMING_RAW
#include <ludlc_raw.h>
#endif

/**
 * @def LUDLC_TX_FIFO_SIZE
 * @brief How many octets a transport takes from the encoder at a time.
 *
 * Unrelated to the frame size: a longer frame takes as many calls as needed.
 * A power of two, so that a kfifo can mask with it.
 */
#ifndef LUDLC_TX_FIFO_SIZE
#define LUDLC_TX_FIFO_SIZE	64
#endif

/**
 * @struct ludlc_framing_ops
 * @brief One framing's operations, for a build carrying more than one.
 *
 * Generated from @ref LUDLC_FRAMING_OP_LIST, along with each codec's
 * prototypes and ops table. Every entry expects a @ref ludlc_framing_state
 * built by its own codec. Call the wrappers below rather than these
 * pointers; that is where each operation is documented.
 */
struct ludlc_framing_ops {
	LUDLC_FRAMING_OP_LIST(LUDLC_FRAMING_OP_PTR, ops)

	/** @brief See @ref ludlc_framing_create. */
	struct ludlc_framing_state *(*create)(void);

	/** @brief See @ref ludlc_framing_destroy. */
	void (*destroy)(struct ludlc_framing_state *fs);
};

/**
 * @struct ludlc_framing
 * @brief What a transport needs to know about a framing before it runs it.
 *
 * Separate from @ref ludlc_framing_ops because a transport reads these while
 * deciding whether it can carry the framing at all, before any frame is
 * started.
 */
struct ludlc_framing {
	/** @brief The framing's name, for configfs and log messages. */
	const char *name;

	/** @brief How it encodes and decodes. */
	const struct ludlc_framing_ops *ops;

	/**
	 * @brief Worst-case encoded size of @p conn's largest frame.
	 *
	 * For sizing announce caps and decode limits, not the TX fifo. The
	 * checksum is a parameter because a transport calls this while sizing
	 * its buffers, before it has a codec to read it from.
	 */
	ludlc_frame_size_t (*encoded_max)(const struct ludlc_connection *conn,
					  const struct ludlc_csum *csum);

	/**
	 * @brief Exact encoded length of this packet, or NULL where the
	 * framing cannot tell before encoding.
	 *
	 * Computed from the packet rather than from an encoder, since a
	 * transport that announces a frame needs the length before writing
	 * it. COBS is the plaintext length, one code octet per 253 data
	 * octets, and the delimiter. Raw returns the frame size unchanged.
	 * NULL on the escaping, whose length depends on which octets need
	 * escaping: a transport such as SPI, which must announce the length
	 * up front, cannot carry such a framing and refuses it on this
	 * pointer.
	 */
	ssize_t (*wire_len)(const struct ludlc_csum *csum,
			    const void *hdr, ludlc_payload_size_t hdr_sz,
			    const void *payload, ludlc_payload_size_t pay_sz);

	/**
	 * @brief The frame has no delimiter, so the decoder must be told its
	 * length.
	 *
	 * True for @ref ludlc_framing_raw only. The transport passes the
	 * announced length as the capacity to @ref ludlc_dec_prep, having
	 * refused one longer than its buffer. A transport reading a byte
	 * stream does not know the length and refuses such a framing rather
	 * than never completing a frame.
	 */
	bool needs_wire_len;

};

/**
 * @brief HDLC-style escaping: a start octet and an escape for it.
 *
 * The default over a byte stream, and what the peers written against an
 * early LuDLC port speak. It resynchronises on a line joined mid-frame, and
 * costs one extra octet per reserved value in the frame, up to one per octet
 * in the worst case.
 */
#if CONFIG_LUDLC_FRAMING_HDLC
extern const struct ludlc_framing ludlc_framing_hdlc;
#endif

/**
 * @brief COBS: a stuffed frame terminated by a zero octet.
 *
 * It delimits like the escaping, but at a fixed cost of one octet per frame
 * plus one per 254 octets.
 */
#if CONFIG_LUDLC_FRAMING_COBS
extern const struct ludlc_framing ludlc_framing_cobs;
#endif

/**
 * @brief The frame unchanged, for a transport that delimits it.
 *
 * Header, payload and checksum, with nothing added or escaped, so the
 * transport must supply the frame length. Only a transport carrying one frame
 * per transfer can run this, since a byte stream cannot resynchronise.
 */
#if CONFIG_LUDLC_FRAMING_RAW
extern const struct ludlc_framing ludlc_framing_raw;
#endif

/** @brief Self-delimiting framings, default first, for a listing. */
#define LUDLC_FRAMING_NAMES_DELIMITED	"hdlc cobs"

/** @brief Framings whose length is known up front, default first. */
#define LUDLC_FRAMING_NAMES_ANNOUNCED	"raw cobs"

/** @brief Every name ludlc_framing_lookup() accepts, for a listing. */
#define LUDLC_FRAMING_NAMES	LUDLC_FRAMING_NAMES_DELIMITED " raw"

/* A lookup by name is only useful when there is more than one framing. */
#if LUDLC_FRAMING_COUNT > 1

/**
 * @brief Turn a name into a framing descriptor.
 *
 * @return The framing, or NULL for an unknown name.
 */
const struct ludlc_framing *ludlc_framing_lookup(const char *name);

/**
 * @brief Walk the framings this build carries.
 *
 * @param i Index, counting from zero.
 * @return The framing there, or NULL once @p i is past the last one.
 */
const struct ludlc_framing *ludlc_framing_at(unsigned int i);

#endif /* LUDLC_FRAMING_COUNT > 1 */

/**
 * @brief What a transport gets when it names no framing.
 *
 * HDLC where it is built, being the sensible default over a byte stream, and
 * otherwise the one framing this build does carry.
 */
static inline const struct ludlc_framing *ludlc_framing_default(void)
{
#if CONFIG_LUDLC_FRAMING_HDLC
	return &ludlc_framing_hdlc;
#elif CONFIG_LUDLC_FRAMING_COBS
	return &ludlc_framing_cobs;
#else
	return &ludlc_framing_raw;
#endif
}

/** @brief The name of @p framing, or of the default if it is NULL. */
static inline const char *
ludlc_framing_name(const struct ludlc_framing *framing)
{
	return framing ? framing->name : ludlc_framing_default()->name;
}

/** @brief The framing to run, or the default if @p framing is NULL. */
static inline
const struct ludlc_framing *ludlc_framing_or_default(
					const struct ludlc_framing *framing)
{
	return framing ? framing : ludlc_framing_default();
}

/*
 * How the wrappers below reach the framing, and how a transport reads its
 * descriptor. With more than one built, through the pointer the state carries.
 * With one, by the name of that framing, which makes the call direct and
 * leaves the state with nothing to carry.
 */
#if LUDLC_FRAMING_COUNT > 1

#define LUDLC_FS_OP(fs, op)	((fs)->fops->ops->op)
#define LUDLC_FRAMING_NEW(framing)					\
	ludlc_framing_or_default(framing)->ops->create()
#define LUDLC_FRAMING_DEL(fs)	((fs)->fops->ops->destroy(fs))
#define LUDLC_FS_FRAMING(fs)	((fs)->fops)

#else /* a single framing: nothing to dispatch on, so call it by name */

#if CONFIG_LUDLC_FRAMING_HDLC
#define LUDLC_FRAMING_ONLY(op)	ludlc_hdlc_##op
#define LUDLC_FRAMING_ONLY_NEW	ludlc_create_framing_hdlc
#define LUDLC_FRAMING_ONLY_DEL	ludlc_destroy_framing_hdlc
#elif CONFIG_LUDLC_FRAMING_COBS
#define LUDLC_FRAMING_ONLY(op)	ludlc_cobs_##op
#define LUDLC_FRAMING_ONLY_NEW	ludlc_create_framing_cobs
#define LUDLC_FRAMING_ONLY_DEL	ludlc_destroy_framing_cobs
#else
#define LUDLC_FRAMING_ONLY(op)	ludlc_raw_##op
#define LUDLC_FRAMING_ONLY_NEW	ludlc_create_framing_raw
#define LUDLC_FRAMING_ONLY_DEL	ludlc_destroy_framing_raw
#endif

#define LUDLC_FS_OP(fs, op)	((void)(fs), LUDLC_FRAMING_ONLY(op))
#define LUDLC_FRAMING_NEW(framing)					\
	((void)(framing), LUDLC_FRAMING_ONLY_NEW())
#define LUDLC_FRAMING_DEL(fs)	LUDLC_FRAMING_ONLY_DEL(fs)
#define LUDLC_FS_FRAMING(fs)	((void)(fs), ludlc_framing_default())

#endif /* LUDLC_FRAMING_COUNT > 1 */

/**
 * @brief Allocate the framing state for one link.
 *
 * The codec takes it from its preallocated objects, and falls back on
 * @c ludlc_platform_alloc() once those run out. A build with one link and one
 * framing preallocates the single object it needs.
 *
 * @param framing The framing to run, or NULL for the default.
 * @return The state, or NULL if none was left.
 */
static inline struct ludlc_framing_state *
ludlc_framing_create(const struct ludlc_framing *framing)
{
	return LUDLC_FRAMING_NEW(framing);
}

/**
 * @brief Release what @ref ludlc_framing_create returned.
 *
 * @param fs The state, or NULL.
 */
static inline void ludlc_framing_destroy(struct ludlc_framing_state *fs)
{
	if (fs) {
		LUDLC_FRAMING_DEL(fs);
	}
}

/**
 * @brief Bring the encoder up with no frame started.
 *
 * Call it again to drop a frame the link cannot finish, after a line break
 * for instance. Octets already on the wire leave the peer a truncated frame,
 * which it rejects on the checksum; there is nothing else to be done.
 *
 * @param fs The link's framing state.
 */
static inline void ludlc_enc_init(struct ludlc_framing_state *fs)
{
	LUDLC_FS_OP(fs, enc_init)(fs);
}

/**
 * @brief Give the encoder the packet to send next.
 *
 * Referenced, not copied: @p hdr and @p payload must stay valid until the
 * encoder is no longer busy.
 *
 * @param fs The link's framing state.
 * @param hdr The packet header.
 * @param hdr_sz Its size.
 * @param payload What the packet carries, or NULL for none.
 * @param pay_sz Its size.
 * @param send_eof Nothing follows this frame. Only the escaping acts on it,
 * appending a trailing SOF; the others delimit every frame anyway.
 */
static inline void ludlc_enc_prep(struct ludlc_framing_state *fs,
				 const void *hdr, ludlc_payload_size_t hdr_sz,
				 const void *payload,
				 ludlc_payload_size_t pay_sz, bool send_eof)
{
	LUDLC_FS_OP(fs, enc_prep)(fs, hdr, hdr_sz, payload, pay_sz, send_eof);
}

/**
 * @brief True while a packet is loaded and not yet fully on the wire.
 *
 * The state machines are idle both before a frame starts and after it ends,
 * so this is what a transport waits on rather than the state itself.
 */
static inline bool ludlc_enc_busy(const struct ludlc_framing_state *fs)
{
	return LUDLC_FS_OP(fs, enc_busy)(fs);
}

/**
 * @brief Write up to @p cap encoded octets into @p dst.
 *
 * Stops at the end of the frame or when @p dst fills. A transport left with
 * room to spare is between frames and pads the rest itself.
 *
 * @param fs The link's framing state.
 * @param csum Checksum this link runs. It is computed as the frame goes
 * out, so every call of one frame must pass the same one.
 * @param dst Where to write.
 * @param cap Room in @p dst.
 * @return Octets written, zero when no packet is loaded.
 */
static inline size_t ludlc_enc_fill(struct ludlc_framing_state *fs,
				    const struct ludlc_csum *csum,
				    uint8_t *dst, size_t cap)
{
	return LUDLC_FS_OP(fs, enc_fill)(fs, csum, dst, cap);
}

/**
 * @brief Bring the decoder up with no frame started.
 *
 * Call it again to drop a half-read frame that cannot be completed, after a
 * line break for instance.
 *
 * @param fs The link's framing state.
 */
static inline void ludlc_dec_init(struct ludlc_framing_state *fs)
{
	LUDLC_FS_OP(fs, dec_init)(fs);
}

/**
 * @brief Point the decoder at the buffer for the next packet.
 *
 * Called again after every packet, once the caller has read the previous one.
 * @p cap covers a whole frame, packet and checksum, which on a narrow payload
 * type is more than a packet alone can be.
 *
 * @param fs The link's framing state.
 * @param csum Checksum this link runs. Seeded here.
 * @param buf Where to gather the packet.
 * @param cap Room in @p buf. For a framing without a delimiter (see
 * @ref ludlc_framing.needs_wire_len) it is also the frame length the
 * transport announced, and 0 where nothing was announced.
 */
static inline void ludlc_dec_prep(struct ludlc_framing_state *fs,
				  const struct ludlc_csum *csum, void *buf,
				  ludlc_frame_size_t cap)
{
	LUDLC_FS_OP(fs, dec_prep)(fs, csum, buf, cap);
}

/**
 * @brief Feed received octets in, stopping on a completed packet.
 *
 * @param fs The link's framing state.
 * @param csum Checksum this link runs. Must be the one
 * @ref ludlc_dec_prep was given, which this carries on computing.
 * @param conn The link, for its counters. A rejected frame is counted here,
 * so the caller can simply continue.
 * @param src Octets as they came off the wire.
 * @param len How many.
 * @param done Set when the last octet consumed completed a packet.
 * @return Octets consumed, which is @p len unless a packet completed part way
 * through. The caller then takes that packet, calls @ref ludlc_dec_prep for
 * the next, and feeds the rest.
 */
static inline size_t ludlc_dec_feed(struct ludlc_framing_state *fs,
				    const struct ludlc_csum *csum,
				    struct ludlc_connection *conn,
				    const uint8_t *src, size_t len, bool *done)
{
	return LUDLC_FS_OP(fs, dec_feed)(fs, csum, conn, src, len, done);
}

/**
 * @brief Size of the packet just decoded, checksum excluded.
 *
 * Narrower than the capacity given to @ref ludlc_dec_prep, which must hold a
 * whole frame. What comes back here is a packet, and the build sizes
 * ludlc_payload_size_t to hold one.
 */
static inline ludlc_payload_size_t
ludlc_dec_size(const struct ludlc_framing_state *fs)
{
	return LUDLC_FS_OP(fs, dec_size)(fs);
}

#endif /* __LUDLC_FRAMING_H__ */

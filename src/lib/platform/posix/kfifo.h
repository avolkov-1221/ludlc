/* SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only) */
/**
 * @file kfifo.h
 *
 * @brief Single-producer, single-consumer byte ring buffer for POSIX userspace.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * Modelled on the Linux kernel's kfifo, and built on C11 atomics and the
 * standard C library alone, with no Linux- or BSD-only header. One thread
 * puts while another gets, and neither takes a lock: each moves only its
 * own index.
 */

#ifndef __KFIFO_H__
#define __KFIFO_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>

struct kfifo {
	uint8_t  *data;	   /* Pointer to the data buffer */
	unsigned int size; /* Total size of the buffer (must be a power of 2) */
	atomic_uint  in;   /* 'in' index (write position, updated by producer) */
	atomic_uint  out;  /* 'out' index (read position, updated by consumer) */
};

/**
 * @brief Checks whether the FIFO is empty.
 *
 * @param fifo FIFO instance to inspect.
 * @return true if no bytes are currently queued, false otherwise.
 */
static inline bool kfifo_is_empty(struct kfifo *fifo)
{
	unsigned int in = atomic_load_explicit(&fifo->in, memory_order_acquire);
	unsigned int out =
		atomic_load_explicit(&fifo->out, memory_order_acquire);

	return in == out;
}

/**
 * @brief Returns the number of used bytes in the FIFO.
 *
 * @param fifo FIFO instance to inspect.
 * @return Number of queued bytes currently available for reading.
 */
static inline unsigned int kfifo_len(const struct kfifo *fifo)
{
	unsigned int in = atomic_load_explicit(&fifo->in, memory_order_acquire);
	unsigned int out =
		atomic_load_explicit(&fifo->out, memory_order_acquire);

	return in - out;
}

/**
 * @brief Checks if the FIFO is full.
 *
 * @param fifo FIFO instance to inspect.
 * @return true if the FIFO is full, false otherwise.
 */
static inline bool kfifo_is_full(const struct kfifo *fifo)
{
	return kfifo_len(fifo) == fifo->size;
}

/**
 * @brief Resets the FIFO to be empty.
 *
 * @note This is not thread-safe and should only be called when there is
 * no producer or consumer activity.
 *
 * @param fifo FIFO instance to reset.
 */
static inline void kfifo_reset(struct kfifo *fifo)
{
	atomic_store_explicit(&fifo->in, 0, memory_order_relaxed);
	atomic_store_explicit(&fifo->out, 0, memory_order_relaxed);
}

/**
 * @brief Initializes FIFO storage and resets queue indices.
 *
 * @param fifo FIFO object to initialize.
 * @param buf Backing byte buffer (must remain valid while FIFO is used).
 * @param size Capacity of @p buf in bytes (expected power-of-two).
 */
static inline void kfifo_init(struct kfifo *fifo, void *buf, unsigned int size)
{
	fifo->data = buf;
	fifo->size = size;
	kfifo_reset(fifo);
}

/**
 * @brief Puts data into the FIFO. To be called only from the producer thread.
 *
 * @param fifo FIFO instance to write to.
 * @param val Byte value to enqueue.
 * @return true if @p val was enqueued, false when FIFO is full.
 */
static inline bool kfifo_put(struct kfifo *fifo, uint8_t val)
{
	unsigned int in = atomic_load_explicit(&fifo->in, memory_order_relaxed);
	unsigned int out = atomic_load_explicit(&fifo->out,
			memory_order_acquire);

	if ((in - out) >= fifo->size) {
		return false;
	}

	fifo->data[in & (fifo->size - 1)] = val;

	/* Release store: the data must be written before the 'in' index
	 * becomes visible to the consumer.
	 */
	atomic_store_explicit(&fifo->in, in + 1, memory_order_release);

	return true;
}

/**
 * @brief Copies up to @p len bytes from @p src into the FIFO.
 *
 * @return How many of @p len were taken.
 */
static inline unsigned int kfifo_in(struct kfifo *fifo, const void *src,
				    unsigned int len)
{
	unsigned int in = atomic_load_explicit(&fifo->in, memory_order_relaxed);
	unsigned int out = atomic_load_explicit(&fifo->out,
			memory_order_acquire);
	unsigned int free = fifo->size - (in - out);
	unsigned int l;
	const uint8_t *p = src;

	len = LUDLC_MIN(len, free);
	l = LUDLC_MIN(len, fifo->size - (in & (fifo->size - 1)));

	memcpy(fifo->data + (in & (fifo->size - 1)), p, l);
	if (len - l)
		memcpy(fifo->data, p + l, len - l);

	atomic_store_explicit(&fifo->in, in + len, memory_order_release);

	return len;
}

/**
 * @brief Gets a single byte from the FIFO. To be called only from the
 * consumer thread.
 *
 * @param fifo FIFO instance to read from.
 * @param val_ptr Destination pointer for the dequeued byte.
 * @return true on success, false if the FIFO is empty.
 */
static inline bool kfifo_get(struct kfifo *fifo, uint8_t *val_ptr)
{
	/* Acquire load, to see the producer's latest 'in' index. */
	unsigned int in = atomic_load_explicit(&fifo->in, memory_order_acquire);
	unsigned int out = atomic_load_explicit(&fifo->out,
			memory_order_relaxed);

	if (in == out) {
		return false;
	}

	/* The acquire load on 'in' guarantees this data is valid. */
	*val_ptr = fifo->data[out & (fifo->size - 1)];

	/* Release store: publishes the freed space to the producer. */
	atomic_store_explicit(&fifo->out, out + 1, memory_order_release);

	return true;
}

/**
 * @brief Removes up to @p sz bytes from FIFO and copies them into @p dst.
 *
 * @param fifo FIFO instance to read from.
 * @param dst Destination buffer for extracted bytes.
 * @param sz Maximum number of bytes to remove.
 * @return Actual number of bytes copied to @p dst.
 */
static inline
unsigned int kfifo_out(struct kfifo *fifo, void *dst, unsigned int sz)
{
	unsigned int l;

	/* Acquire load: this is what makes the producer's data visible. */
	unsigned int in =
		atomic_load_explicit(&fifo->in, memory_order_acquire);
	unsigned int out =
		atomic_load_explicit(&fifo->out, memory_order_relaxed);

	unsigned int data_len = in - out;

	sz = LUDLC_MIN(sz, data_len);

	/* How much can be copied before the buffer wraps. */
	l = LUDLC_MIN(sz, fifo->size - (out & (fifo->size - 1)));

	/* Copy in two parts when the read wraps. */
	memcpy(dst, fifo->data + (out & (fifo->size - 1)), l);
	if (sz - l) {
		memcpy((uint8_t *)dst + l, fifo->data, sz - l);
	}

	/* Release store: publishes the freed space to the producer. */
	atomic_store_explicit(&fifo->out, out + sz, memory_order_release);

	return sz;
}

/**
 * @brief Claims the next writable region, for the producer to fill in place.
 *
 * Hands out the FIFO's own storage, so a caller that generates its bytes need
 * not stage them anywhere first. The region stops at the end of the backing
 * buffer, so a caller with more to write claims again once the wrap has been
 * published. Nothing is reserved until @ref kfifo_put_finish, which means only
 * one claim can be outstanding.
 *
 * @param fifo FIFO instance to write to.
 * @param ptr Set to the first writable byte.
 * @param n Most bytes wanted.
 * @return Bytes writable through @p ptr, 0 when the FIFO is full.
 */
static inline unsigned int kfifo_put_claim(struct kfifo *fifo, uint8_t **ptr,
					   unsigned int n)
{
	unsigned int in = atomic_load_explicit(&fifo->in, memory_order_relaxed);
	unsigned int out = atomic_load_explicit(&fifo->out,
			memory_order_acquire);
	unsigned int head = in & (fifo->size - 1);

	n = LUDLC_MIN(n, fifo->size - (in - out));
	*ptr = fifo->data + head;

	return LUDLC_MIN(n, fifo->size - head);
}

/**
 * @brief Publishes @p count bytes written through @ref kfifo_put_claim.
 *
 * @param fifo FIFO instance written to.
 * @param count Bytes written, at most what the claim returned.
 */
static inline void kfifo_put_finish(struct kfifo *fifo, unsigned int count)
{
	unsigned int in = atomic_load_explicit(&fifo->in, memory_order_relaxed);

	/* Release store: the bytes must be written before the new 'in' index
	 * becomes visible to the consumer.
	 */
	atomic_store_explicit(&fifo->in, in + count, memory_order_release);
}

/**
 * @brief Claims the next readable region, for the consumer to read in place.
 *
 * Hands out the FIFO's own storage, so a caller that passes the bytes on need
 * not copy them out first. The region stops at the end of the backing buffer,
 * so a caller wanting the rest claims again once the wrap has been released.
 * Nothing is consumed until @ref kfifo_get_finish, which means only one claim
 * can be outstanding.
 *
 * @param fifo FIFO instance to read from.
 * @param ptr Set to the first readable byte.
 * @param n Most bytes wanted.
 * @return Bytes readable through @p ptr, 0 when the FIFO is empty.
 */
static inline unsigned int kfifo_get_claim(struct kfifo *fifo, uint8_t **ptr,
					   unsigned int n)
{
	/* Acquire load: this is what makes the producer's data visible. */
	unsigned int in = atomic_load_explicit(&fifo->in, memory_order_acquire);
	unsigned int out = atomic_load_explicit(&fifo->out,
			memory_order_relaxed);
	unsigned int tail = out & (fifo->size - 1);

	n = LUDLC_MIN(n, in - out);
	*ptr = fifo->data + tail;

	return LUDLC_MIN(n, fifo->size - tail);
}

/**
 * @brief Releases @p count bytes read through @ref kfifo_get_claim.
 *
 * Also the way to drop bytes without reading them.
 *
 * @param fifo FIFO instance read from.
 * @param count Bytes consumed, at most what the claim returned.
 */
static inline void kfifo_get_finish(struct kfifo *fifo, unsigned int count)
{
	unsigned int out = atomic_load_explicit(&fifo->out,
			memory_order_relaxed);

	/* Release store: publishes the freed space to the producer. */
	atomic_store_explicit(&fifo->out, out + count, memory_order_release);
}

#endif /* __KFIFO_H__ */

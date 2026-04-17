// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/*
 * kfifo.h
 *
 * Single-producer/single-consumer byte ring buffer (Linux kernel kfifo-style)
 * for POSIX userspace without Linux- or BSD-only headers.
 *
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * This file is licensed under either the Apache License, Version 2.0,
 * or the GNU General Public License, version 2 or (at your option)
 * any later version.
 */

#ifndef __KFIFO_H__
#define __KFIFO_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>

/* The main kfifo structure */
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
	/* The FIFO is full if the number of bytes in
	 * it equals its total capacity.
	 */
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

	/* Use a release store to "publish" the write. This ensures the data is
	 * written before the 'in' index is updated and visible to the consumer.
	 */
	atomic_store_explicit(&fifo->in, in + 1, memory_order_release);

	return true;
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
	/* Use an acquire load to ensure we see the
	 * producer's latest 'in' index.
	 */
	unsigned int in = atomic_load_explicit(&fifo->in, memory_order_acquire);
	unsigned int out = atomic_load_explicit(&fifo->out,
			memory_order_relaxed);

	if (in == out) {
		return false;
	}

	/* Read the data from the buffer. The acquire load on 'in' guarantees
	 * that this data is valid.
	 */
	*val_ptr = fifo->data[out & (fifo->size - 1)];

	/* Use a release store to "publish" the newly freed space
	 * to the producer.
	 */
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

	/* Use an acquire load to get the 'in' index. This is the crucial
	 * memory barrier that ensures we see the data the producer wrote.
	 */
	unsigned int in =
		atomic_load_explicit(&fifo->in, memory_order_acquire);
	unsigned int out =
		atomic_load_explicit(&fifo->out, memory_order_relaxed);

	unsigned int data_len = in - out;

	sz = LUDLC_MIN(sz, data_len);

	/* Calculate how much data to copy before the buffer wraps around. */
	l = LUDLC_MIN(sz, fifo->size - (out & (fifo->size - 1)));

	/* Perform the copy, potentially in two parts if it wraps. */
	memcpy(dst, fifo->data + (out & (fifo->size - 1)), l);
	l = sz - l;
	if (l) {
		memcpy((uint8_t *)dst + l, fifo->data, l);
	}

	/* Use a release store to update the 'out' index. This "publishes"
	 * the newly available space to the producer thread.
	 */
	atomic_store_explicit(&fifo->out, out + sz, memory_order_release);

	return sz;
}

/**
 * @brief Advances read index by @p count bytes without copying data.
 *
 * @param fifo FIFO instance to consume from.
 * @param count Number of bytes to discard.
 */
static inline
void kfifo_skip_count(struct kfifo *fifo, unsigned int count)
{
	unsigned int out =
		atomic_load_explicit(&fifo->out, memory_order_relaxed);

	atomic_store_explicit(&fifo->out, out + count, memory_order_release);
}

/**
 * @brief Returns pointer to the next linear readable region.
 *
 * The returned length does not wrap: it is limited by contiguous data up to
 * the end of the backing buffer.
 *
 * @param fifo FIFO instance to inspect.
 * @param ptr Output pointer to contiguous readable bytes (unchanged if NULL).
 * @param n Maximum number of bytes requested.
 * @return Number of contiguous bytes available through @p ptr.
 */
static inline unsigned int kfifo_out_linear_ptr(struct kfifo *fifo,
						uint8_t **ptr, unsigned int n)
{
	/* Use an acquire load to get the 'in' index. This is the crucial
	 * memory barrier that ensures we see the data the producer wrote.
	 */
	unsigned int in =
		atomic_load_explicit(&fifo->in, memory_order_acquire);
	unsigned int out =
		atomic_load_explicit(&fifo->out, memory_order_relaxed);

	if (ptr) {
		unsigned int tail = (out & (fifo->size - 1));

		n = LUDLC_MIN(n, in - out);
		*ptr = fifo->data + tail;

		return LUDLC_MIN(n, fifo->size - tail);
	}

	return 0;
}

#endif /* __KFIFO_H__ */

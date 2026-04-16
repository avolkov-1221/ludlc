// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/*
 * kfifo.h
 *
 * Single-producer/single-consumer byte ring buffer (kfifo-style) for
 * POSIX userspace without Linux- or BSD-only headers.
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

/** Round @a n up to a multiple of @a m (unsigned arithmetic, @a m > 0). */
#define LUDLC_ROUND_UP(n, m) \
	((((n) + (m)-1U) / (m)) * (m))

#ifndef LUDLC_MIN
#define LUDLC_MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

// The main kfifo structure
struct kfifo {
	uint8_t  *data;		/* Pointer to the data buffer */
	unsigned int size;	/* Total size of the buffer (must be a power of 2) */
	atomic_uint  in;	/* 'in' index (write position, updated by producer) */
	atomic_uint  out;	/* 'out' index (read position, updated by consumer) */
};

/**
 * kfifo_is_empty - returns true if the fifo is empty
 * @param fifo The fifo to check.
 */
static inline bool kfifo_is_empty(struct kfifo *fifo)
{
	unsigned int in = atomic_load_explicit(&fifo->in, memory_order_acquire);
	unsigned int out = atomic_load_explicit(&fifo->out, memory_order_acquire);
	return in == out;
}

/**
 * @brief Returns the number of used bytes in the FIFO.
 * @param fifo The fifo to check.
 */
static inline unsigned int kfifo_len(const struct kfifo *fifo)
{
	unsigned int in = atomic_load_explicit(&fifo->in, memory_order_acquire);
	unsigned int out = atomic_load_explicit(&fifo->out, memory_order_acquire);
	return in - out;
}

/**
 * @brief Checks if the FIFO is full.
 * @param fifo The fifo to check.
 * @return true if the FIFO is full, false otherwise.
 */
static inline bool kfifo_is_full(const struct kfifo *fifo)
{
	// The FIFO is full if the number of bytes in it equals its total capacity.
	return kfifo_len(fifo) == fifo->size;
}

/**
 * @brief Resets the FIFO to be empty.
 * @note This is not thread-safe and should only be called when there is
 * no producer or consumer activity.
 * @param fifo The fifo to reset.
 */
static inline void kfifo_reset(struct kfifo *fifo)
{
	atomic_store_explicit(&fifo->in, 0, memory_order_relaxed);
	atomic_store_explicit(&fifo->out, 0, memory_order_relaxed);
}

/**
 * @brief Resets the FIFO to be empty.
 * @note This is not thread-safe and should only be called when there is
 * no producer or consumer activity.
 * @param fifo The fifo to reset.
 */
static inline void kfifo_init(struct kfifo *fifo, void *buf, unsigned int size)
{
	fifo->data = buf;
	fifo->size = size;
	kfifo_reset(fifo);
}

/**
 * @brief Puts data into the FIFO. To be called only from the producer thread.
 * @param fifo The fifo to use.
 * @param from The data to be added.
 * @param len The length of the data.
 * @return true if data has been put into the fifo, false if not.
 */
static inline bool kfifo_put(struct kfifo *fifo, uint8_t val)
{
	unsigned int in = atomic_load_explicit(&fifo->in, memory_order_relaxed);
	unsigned int out = atomic_load_explicit(&fifo->out,
			memory_order_acquire);

	if ((in - out) >= fifo->size) {
		return false; // FIFO is full
	}

	// Write the data to the buffer.
	fifo->data[in & (fifo->size - 1)] = val;

	// Use a release store to "publish" the write. This ensures the data is
	// written before the 'in' index is updated and visible to the consumer.
	atomic_store_explicit(&fifo->in, in + 1, memory_order_release);

	return true;
}

/**
 * @brief Gets a single byte from the FIFO. To be called only from the consumer thread.
 * @param fifo The fifo to use.
 * @param val_ptr A pointer to a uint8_t where the data will be stored.
 * @return true on success, false if the FIFO is empty.
 */
static inline bool kfifo_get(struct kfifo *fifo, uint8_t *val_ptr)
{
	// Use an acquire load to ensure we see the producer's latest 'in' index.
	unsigned int in = atomic_load_explicit(&fifo->in, memory_order_acquire);
	unsigned int out = atomic_load_explicit(&fifo->out,
			memory_order_relaxed);

	if (in == out) {
		return false; // FIFO is empty
	}

	// Read the data from the buffer. The acquire load on 'in' guarantees
	// that this data is valid.
	*val_ptr = fifo->data[out & (fifo->size - 1)];

	// Use a release store to "publish" the newly freed space to the producer.
	atomic_store_explicit(&fifo->out, out + 1, memory_order_release);

	return true;
}

static inline
unsigned int kfifo_out(struct kfifo *fifo, void *dst, unsigned int sz)
{
	unsigned int l;

	// 1. Use an acquire load to get the 'in' index. This is the crucial
	//    memory barrier that ensures we see the data the producer wrote.
	unsigned int in =
		atomic_load_explicit(&fifo->in, memory_order_acquire);
	unsigned int out =
		atomic_load_explicit(&fifo->out, memory_order_relaxed);

	unsigned int data_len = in - out;
	sz = LUDLC_MIN(sz, data_len);

	// 2. Calculate how much data to copy before the buffer wraps around.
	l = LUDLC_MIN(sz, fifo->size - (out & (fifo->size - 1)));

	// 3. Perform the copy, potentially in two parts if it wraps.
	memcpy(dst, fifo->data + (out & (fifo->size - 1)), l);
	l = sz - l;
	if (l)
		memcpy((uint8_t *)dst + l, fifo->data, l);

	// 4. Use a release store to update the 'out' index. This "publishes"
	//    the newly available space to the producer thread.
	atomic_store_explicit(&fifo->out, out + sz, memory_order_release);

	return sz;
}

/**
 * kfifo_skip_count - skip output data
 * @fifo: address of the fifo to be used
 * @count: count of data to skip
 */
static inline
void kfifo_skip_count(struct kfifo *fifo, unsigned int count)
{
	unsigned int out =
		atomic_load_explicit(&fifo->out, memory_order_relaxed);

	atomic_store_explicit(&fifo->out, out + count, memory_order_release);
}

/**
 * kfifo_out_linear_ptr - gets a pointer to the available data
 * @fifo: address of the fifo to be used
 * @ptr: pointer to data to store the pointer to tail
 * @n: max. number of elements to point at
 *
 * Similarly to kfifo_out_linear(), this function obtains the pointer to the
 * available data in the fifo buffer and returns the numbers of elements
 * available. It returns the available count till the end of available data or
 * till the end of the buffer. So that it can be used for linear data
 * processing (like memcpy() of @ptr with count returned).
 *
 * Note that with only one concurrent reader and one concurrent
 * writer, you don't need extra locking to use these function.
 */
static inline
unsigned int kfifo_out_linear_ptr(struct kfifo *fifo, uint8_t **ptr, unsigned int n)
{
	// 1. Use an acquire load to get the 'in' index. This is the crucial
	//    memory barrier that ensures we see the data the producer wrote.
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

/* SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only) */
/**
 * @file ludlc_posix.h
 *
 * @brief LuDLC POSIX platform-specific definitions.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * What the POSIX port needs and the C library does not provide:
 * container_of(), which the rest of the code takes from the kernel style.
 */

#ifndef __LUDLC_POSIX_H__
#define __LUDLC_POSIX_H__

/**
 * @brief Get the pointer to the containing structure.
 * @param ptr Pointer to the member.
 * @param type The type of the container structure.
 * @param member The name of the member within the structure.
 * @return Pointer to the container structure.
 */

#define container_of(ptr, type, member) ({			\
	const typeof(((type *)0)->member) *__mptr = (ptr);	\
	(type *)((char *)__mptr - offsetof(type, member));	\
})

#endif /* __LUDLC_POSIX_H__ */

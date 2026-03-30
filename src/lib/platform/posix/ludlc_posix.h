// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file ludlc_posix.h
 *
 * @brief LuDLC POSIX platform-specific data structures.
 *
 * This file defines data structures used by the POSIX platform
 * abstraction layer. It primarily defines the `ludlc_posix_connection`
 * structure, which extends the core `ludlc_connection` with
 * POSIX-specific state.
 *
 * Copyright (C) 2025 Andrey VOLKOV <andrey@volkov.fr> and LuDLC Contributors
 *
 * This file is licensed under either the Apache License, Version 2.0,
 * or the GNU General Public License, version 2 or (at your option)
 * any later version.
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

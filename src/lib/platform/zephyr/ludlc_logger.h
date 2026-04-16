// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file ludlc_logger.h
 *
 * @brief A Zephyr compatible logging macros.
 *
 * This file defines Zephyr-specific logging macros for the LuDLC library.
 * It maps standard LuDLC logging levels (LUDLC_LOG_DEBUG, LUDLC_LOG_INFO, etc)
 * to Zephyr's native logging implementation.
 *
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * This file is licensed under either the Apache License, Version 2.0,
 * or the GNU General Public License, version 2 or (at your option)
 * any later version.
 */

#ifndef __LUDLC_LOGGER_H__
#define __LUDLC_LOGGER_H__

#include <zephyr/logging/log.h>

/**
 * @brief Sets the maximum log level to be compiled.
 *
 * To set this, compile with, for example: -DMAX_LOG_LEVEL=LOG_INFO
 * This will completely remove all LOG_DEBUG calls from the compiled binary.
 * If not defined, it defaults to LOG_LEVEL_DEBUG (i.e., log everything).
 */
#ifndef MAX_LOG_LEVEL
#define MAX_LOG_LEVEL LOG_DEBUG
#endif

#if MAX_LUDLC_LOG_LEVEL <= LOG_DEBUG
#define LUDLC_LOG_DEBUG(...) LOG_DBG(__VA_ARGS__)
#else
#define LUDLC_LOG_DEBUG(...) do { (void)0; } while(0)
#endif

#define LUDLC_LOG_TRACE	LUDLC_LOG_DEBUG

#if MAX_LUDLC_LOG_LEVEL <= LOG_INFO
#define LUDLC_LOG_INFO(...) LOG_INF(__VA_ARGS__)
#else
#define LUDLC_LOG_INFO(...) do { (void)0; } while(0)
#endif

#if MAX_LUDLC_LOG_LEVEL <= LOG_WARN
#define LUDLC_LOG_WARN(...) LOG_WRN(__VA_ARGS__)
#else
#define LUDLC_LOG_WARN(...) do { (void)0; } while(0)
#endif

#if MAX_LUDLC_LOG_LEVEL <= LOG_ERROR
#define LUDLC_LOG_ERROR(...) LOG_ERR(__VA_ARGS__)
#else
#define LUDLC_LOG_ERROR(...) do { (void)0; } while(0)
#endif

#ifndef LUDLC_LOG_REGISTER
LOG_MODULE_DECLARE(ludlc);
#endif

#endif /* __LUDLC_LOGGER_H__ */

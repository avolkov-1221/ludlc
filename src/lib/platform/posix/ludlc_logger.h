// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file logger.h
 *
 * @brief A POSIX compatible logging macros.
 *
 * This header detects the build environment and maps the logging
 * macros (LOG_DEBUG, LOG_INFO, etc.) to the correct native
 * logging implementation.
 *
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * This file is licensed under either the Apache License, Version 2.0,
 * or the GNU General Public License, version 2 or (at your option)
 * any later version.
 */

#ifndef __LUDLC_LOGGER_H__
#define __LUDLC_LOGGER_H__

/* =========================================================================
 *     DEFAULT POSIX USERSPACE LOGGER IMPLEMENTATION
 * =========================================================================
 * This is the original implementation.
 * It requires rxi's log.c/log.h to be compiled and linked.
 */
#include <log.h>

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
#define LUDLC_LOG_DEBUG(...) log_debug(__VA_ARGS__)
#else
#define LUDLC_LOG_DEBUG(...) do { (void)0; } while(0)
#endif

#if MAX_LUDLC_LOG_LEVEL <= LOG_INFO
#define LUDLC_LOG_INFO(...) log_info(__VA_ARGS__)
#else
#define LUDLC_LOG_INFO(...) do { (void)0; } while(0)
#endif

#if MAX_LUDLC_LOG_LEVEL <= LOG_WARN
#define LUDLC_LOG_WARN(...) log_warn(__VA_ARGS__)
#else
#define LUDLC_LOG_WARN(...) do { (void)0; } while(0)
#endif

#if MAX_LUDLC_LOG_LEVEL <= LOG_ERROR
#define LUDLC_LOG_ERROR(...) log_error(__VA_ARGS__)
#else
#define LUDLC_LOG_ERROR(...) do { (void)0; } while(0)
#endif

#endif /* __LUDLC_LOGGER_H__ */


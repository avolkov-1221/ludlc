// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file posix/ludlc_logger.h
 *
 * @brief POSIX logging macros.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * Maps the LuDLC log levels (LUDLC_LOG_DEBUG, LUDLC_LOG_INFO and so on) onto
 * rxi's log.c implementation.
 */

#ifndef __LUDLC_LOGGER_H__
#define __LUDLC_LOGGER_H__

/*
 * The default POSIX userspace logger: requires rxi's log.c/log.h to be
 * compiled and linked.
 */
#include <log.h>

/**
 * @name Levels the gating below can compare
 *
 * log.h declares LOG_TRACE and the rest as an enum, which the preprocessor
 * cannot evaluate: in an @c \#if every one of them reads as 0. These mirror
 * that enum, in the same order, so the tests below mean what they say.
 * @{
 */
#define LUDLC_LOG_LVL_FATAL	0
#define LUDLC_LOG_LVL_ERROR	1
#define LUDLC_LOG_LVL_WARN	2
#define LUDLC_LOG_LVL_INFO	3
#define LUDLC_LOG_LVL_DEBUG	4
#define LUDLC_LOG_LVL_TRACE	5
/** @} */

/**
 * @brief Sets the most verbose log level compiled in.
 *
 * Build with, for example, -DMAX_LOG_LEVEL=LUDLC_LOG_LVL_WARN to drop
 * anything more verbose than a warning from the binary. Defaults to
 * @ref LUDLC_LOG_LVL_INFO, which leaves out debug and trace.
 */
#ifndef MAX_LOG_LEVEL
#define MAX_LOG_LEVEL LUDLC_LOG_LVL_INFO
#endif

#if MAX_LOG_LEVEL >= LUDLC_LOG_LVL_TRACE
#define LUDLC_LOG_TRACE(...) log_trace(__VA_ARGS__)
#else
#define LUDLC_LOG_TRACE(...) do { (void)0; } while(0)
#endif

#if MAX_LOG_LEVEL >= LUDLC_LOG_LVL_DEBUG
#define LUDLC_LOG_DEBUG(...) log_debug(__VA_ARGS__)
#else
#define LUDLC_LOG_DEBUG(...) do { (void)0; } while(0)
#endif

#if MAX_LOG_LEVEL >= LUDLC_LOG_LVL_INFO
#define LUDLC_LOG_INFO(...) log_info(__VA_ARGS__)
#else
#define LUDLC_LOG_INFO(...) do { (void)0; } while(0)
#endif

#if MAX_LOG_LEVEL >= LUDLC_LOG_LVL_WARN
#define LUDLC_LOG_WARN(...) log_warn(__VA_ARGS__)
#else
#define LUDLC_LOG_WARN(...) do { (void)0; } while(0)
#endif

#if MAX_LOG_LEVEL >= LUDLC_LOG_LVL_ERROR
#define LUDLC_LOG_ERROR(...) log_error(__VA_ARGS__)
#else
#define LUDLC_LOG_ERROR(...) do { (void)0; } while(0)
#endif

#endif /* __LUDLC_LOGGER_H__ */

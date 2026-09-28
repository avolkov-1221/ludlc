// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file zephyr/ludlc_logger.h
 *
 * @brief Zephyr logging macros.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * Maps the LuDLC log levels (LUDLC_LOG_DEBUG, LUDLC_LOG_INFO and so on) onto
 * Zephyr's logging API.
 */

#ifndef __LUDLC_LOGGER_H__
#define __LUDLC_LOGGER_H__

#include <zephyr/logging/log.h>

/*
 * Zephyr drops the calls it does not want at compile time through its own
 * per-module log level, set in Kconfig, so these are plain aliases and gate
 * nothing themselves.
 */
#define LUDLC_LOG_TRACE	LOG_DBG
#define LUDLC_LOG_DEBUG	LOG_DBG
#define LUDLC_LOG_INFO	LOG_INF
#define LUDLC_LOG_WARN	LOG_WRN
#define LUDLC_LOG_ERROR	LOG_ERR

#ifndef LUDLC_LOG_REGISTER
LOG_MODULE_DECLARE(ludlc, LOG_LEVEL_DBG);
#endif

#endif /* __LUDLC_LOGGER_H__ */

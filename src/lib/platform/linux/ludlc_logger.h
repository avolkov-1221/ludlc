/* SPDX-License-Identifier: GPL-2.0-only */
/**
 * @file linux/ludlc_logger.h
 *
 * @brief The logging the core expects, as printk.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * Every message gets one "ludlc: " prefix and a newline. Trace is folded into
 * debug, printk having no separate level for it.
 */

#ifndef _LUDLC_LOGGER_H_
#define _LUDLC_LOGGER_H_

#include <linux/printk.h>

#define LUDLC_LOG_ERROR(fmt, ...) pr_err("ludlc: " fmt "\n", ##__VA_ARGS__)
#define LUDLC_LOG_WARN(fmt, ...)  pr_warn("ludlc: " fmt "\n", ##__VA_ARGS__)
#define LUDLC_LOG_INFO(fmt, ...)  pr_info("ludlc: " fmt "\n", ##__VA_ARGS__)
#define LUDLC_LOG_DEBUG(fmt, ...) pr_debug("ludlc: " fmt "\n", ##__VA_ARGS__)
#define LUDLC_LOG_TRACE(fmt, ...) pr_debug("ludlc: " fmt "\n", ##__VA_ARGS__)

#endif /* _LUDLC_LOGGER_H_ */

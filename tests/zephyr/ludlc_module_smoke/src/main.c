// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only)
/**
 * @file ludlc_module_smoke/src/main.c
 *
 * @brief Checks that an application can build against the LuDLC Zephyr module.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * A build-smoke target: it includes the public header and takes the size of
 * a connection pointer, so it passes once the module and its headers build
 * in an application. Nothing is tested at run time.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <ludlc.h>

int main(void)
{
	ARG_UNUSED(sizeof(struct ludlc_connection *));

	return 0;
}

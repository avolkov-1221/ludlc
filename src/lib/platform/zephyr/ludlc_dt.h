/* SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only) */
/**
 * @file ludlc_dt.h
 *
 * @brief A LuDLC link described in the devicetree.
 *
 * @copyright
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * @details
 * A node of compatible "ludlc,uart" or "ludlc,spi-peripheral" describes this
 * end of a link: the controller it is a device of, and what the link runs,
 * with the properties both ends must agree on from ludlc,link-common.yaml.
 * The macros here turn such a node into the arguments its transport is
 * created with, at build time:
 *
 * @code
 * static const ludlc_platform_args_t args =
 * 	LUDLC_UART_ARGS_DT(DT_NODELABEL(coproc_link));
 *
 * ret = ludlc_serial_connection_create(&args, &conn, &callbacks, NULL);
 * @endcode
 *
 * A property the node leaves out takes the default of its binding, and a
 * checksum left out is CRC-16/KERMIT, as with the default arguments. Each
 * transport checks every enabled node of its compatible when it is built, so
 * that a framing the build leaves out, a checksum wider than
 * CONFIG_LUDLC_CSUM_BITS, or a payload or a tag the build cannot run stops the
 * build rather than the link.
 */

#ifndef __LUDLC_DT_H__
#define __LUDLC_DT_H__

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#include <ludlc.h>

#if defined(CONFIG_LUDLC_SPI_CONN)
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#endif

/*
 * The framings, declared again rather than through ludlc_framing.h, which
 * brings the core's private headers and its log module into the application.
 */
#if CONFIG_LUDLC_FRAMING_HDLC
extern const struct ludlc_framing ludlc_framing_hdlc;
#endif
#if CONFIG_LUDLC_FRAMING_COBS
extern const struct ludlc_framing ludlc_framing_cobs;
#endif
#if CONFIG_LUDLC_FRAMING_RAW
extern const struct ludlc_framing ludlc_framing_raw;
#endif

/** @cond INTERNAL_HIDDEN */

/* The checksum each value of ludlc,csum selects, and its width in bits. */
#define LUDLC_DT_CSUM_none			NULL
#define LUDLC_DT_CSUM_crc16_ccitt_pr0		(&ludlc_crc16_ccitt_pr0)
#define LUDLC_DT_CSUM_crc16_x25_pr1		(&ludlc_crc16_x25_pr1)
#define LUDLC_DT_CSUM_crc16_pr0			(&ludlc_crc16_pr0)
#define LUDLC_DT_CSUM_crc32_ccitt_pr0		(&ludlc_crc32_ccitt_pr0)

#define LUDLC_DT_CSUM_BITS_none			0
#define LUDLC_DT_CSUM_BITS_crc16_ccitt_pr0	16
#define LUDLC_DT_CSUM_BITS_crc16_x25_pr1	16
#define LUDLC_DT_CSUM_BITS_crc16_pr0		16
#define LUDLC_DT_CSUM_BITS_crc32_ccitt_pr0	32

/** @endcond */

/** @brief The checksum @p node_id runs, CRC-16/KERMIT if it names none. */
#define LUDLC_DT_CSUM(node_id)						\
	COND_CODE_1(DT_NODE_HAS_PROP(node_id, ludlc_csum),		\
		    (UTIL_CAT(LUDLC_DT_CSUM_,				\
			      DT_STRING_TOKEN(node_id, ludlc_csum))),	\
		    (&ludlc_crc16_ccitt_pr0))

/** @brief The width of the checksum @p node_id runs, in bits. */
#define LUDLC_DT_CSUM_BITS(node_id)					\
	COND_CODE_1(DT_NODE_HAS_PROP(node_id, ludlc_csum),		\
		    (UTIL_CAT(LUDLC_DT_CSUM_BITS_,			\
			      DT_STRING_TOKEN(node_id, ludlc_csum))),	\
		    (16))

/** @brief The framing @p node_id runs, its binding's default if unnamed. */
#define LUDLC_DT_FRAMING(node_id)					\
	(&UTIL_CAT(ludlc_framing_, DT_STRING_TOKEN(node_id, ludlc_framing)))

/** @brief Whether this build carries the framing @p node_id runs. */
#define LUDLC_DT_FRAMING_BUILT(node_id)					\
	IS_ENABLED(UTIL_CAT(CONFIG_LUDLC_FRAMING_,			\
			    DT_STRING_UPPER_TOKEN(node_id, ludlc_framing)))

/** @cond INTERNAL_HIDDEN */

/*
 * What of @p node_id goes into its ludlc_conn_cfg, and whether the build can
 * run it: a setting the cfg has no field for is the build's own, which the
 * node has to repeat.
 */
#if CONFIG_LUDLC_MULTI_CONN
#define LUDLC_DT_CFG_PAYLOAD(node_id)					\
	.max_payload = DT_PROP_OR(node_id, ludlc_max_payload, 0),

#define LUDLC_DT_PAYLOAD_OK(node_id)					\
	IN_RANGE(DT_PROP_OR(node_id, ludlc_max_payload, 1), 1,		\
		 CONFIG_LUDLC_MAX_PAYLOAD_SIZE)
#else
#define LUDLC_DT_CFG_PAYLOAD(node_id)

#define LUDLC_DT_PAYLOAD_OK(node_id)					\
	(DT_PROP_OR(node_id, ludlc_max_payload,				\
		    CONFIG_LUDLC_MAX_PAYLOAD_SIZE) ==			\
	 CONFIG_LUDLC_MAX_PAYLOAD_SIZE)
#endif

#if LUDLC_CHAN_TAG_PER_LINK
#define LUDLC_DT_CFG_TAG(node_id)					\
	.chan_tag_bits = DT_PROP(node_id, ludlc_channel_tag_bits),

#define LUDLC_DT_TAG_OK(node_id)					\
	(DT_PROP(node_id, ludlc_channel_tag_bits) < CONFIG_LUDLC_CHANNEL_BITS)
#else
#define LUDLC_DT_CFG_TAG(node_id)

#define LUDLC_DT_TAG_OK(node_id)					\
	(DT_PROP(node_id, ludlc_channel_tag_bits) ==			\
	 CONFIG_LUDLC_CHAN_TAG_BITS)
#endif

/** @endcond */

/**
 * @brief The ludlc_conn_cfg @p node_id describes.
 *
 * The payload is kept only where the build has CONFIG_LUDLC_MULTI_CONN, and
 * the tag only where each link sets its own, see LUDLC_CHAN_TAG_PER_LINK.
 * Otherwise each is the build's own, and LUDLC_DT_CHECK() makes sure the node
 * agrees with it.
 */
#define LUDLC_DT_CONN_CFG(node_id)					\
	{								\
		.get_timestamp = NULL,					\
		LUDLC_DT_CFG_PAYLOAD(node_id)				\
		LUDLC_DT_CFG_TAG(node_id)				\
	}

/**
 * @brief Refuse at build time a node no link could be opened from.
 *
 * Each transport runs this over every enabled node of its compatible.
 */
#define LUDLC_DT_CHECK(node_id)						\
	BUILD_ASSERT(LUDLC_DT_FRAMING_BUILT(node_id),			\
		     DT_NODE_PATH(node_id)				\
		     ": ludlc,framing is left out of this build");	\
	BUILD_ASSERT(LUDLC_DT_CSUM_BITS(node_id) <=			\
		     CONFIG_LUDLC_CSUM_BITS,				\
		     DT_NODE_PATH(node_id)				\
		     ": ludlc,csum is wider than CONFIG_LUDLC_CSUM_BITS"); \
	BUILD_ASSERT(LUDLC_DT_PAYLOAD_OK(node_id),			\
		     DT_NODE_PATH(node_id)				\
		     ": ludlc,max-payload is not one this build carries"); \
	BUILD_ASSERT(LUDLC_DT_TAG_OK(node_id),				\
		     DT_NODE_PATH(node_id)				\
		     ": ludlc,channel-tag-bits is not one this build runs");

/**
 * @brief The arguments of the "ludlc,uart" node @p node_id.
 *
 * For ludlc_serial_connection_create(). The UART is the node's parent, and
 * keeps its own line settings.
 */
#define LUDLC_UART_ARGS_DT(node_id)					\
	{								\
		.dev = DEVICE_DT_GET(DT_BUS(node_id)),			\
		.csum = LUDLC_DT_CSUM(node_id),				\
		.framing = LUDLC_DT_FRAMING(node_id),			\
		.cfg = LUDLC_DT_CONN_CFG(node_id),			\
		.thread = {						\
			.rx_prio = DT_PROP(node_id,			\
					   ludlc_rx_thread_priority),	\
			.tx_prio = DT_PROP(node_id,			\
					   ludlc_tx_thread_priority),	\
		},							\
	}

#if defined(CONFIG_LUDLC_SPI_CONN)
/** @brief The SPI mode @p node_id describes, as spi_config operation bits. */
#define LUDLC_DT_SPI_MODE(node_id)					\
	((DT_PROP(node_id, spi_cpol) ? SPI_MODE_CPOL : 0) |		\
	 (DT_PROP(node_id, spi_cpha) ? SPI_MODE_CPHA : 0) |		\
	 (DT_PROP(node_id, spi_lsb_first) ? SPI_TRANSFER_LSB : 0))

/**
 * @brief The arguments of the "ludlc,spi-peripheral" node @p node_id.
 *
 * For ludlc_spi_slave_connection_create(). The SPI controller is the node's
 * parent, its chip select the node's reg, and the data-ready line the node's
 * data-ready-gpios, none if it has no such line. The link runs one thread,
 * at the node's ludlc,thread-priority.
 */
#define LUDLC_SPI_PERIPHERAL_ARGS_DT(node_id)				\
	{								\
		.dev = DEVICE_DT_GET(DT_BUS(node_id)),			\
		.csum = LUDLC_DT_CSUM(node_id),				\
		.framing = LUDLC_DT_FRAMING(node_id),			\
		.cfg = LUDLC_DT_CONN_CFG(node_id),			\
		.thread = {						\
			.rx_prio = DT_PROP(node_id, ludlc_thread_priority), \
			.tx_prio = DT_PROP(node_id, ludlc_thread_priority), \
		},							\
		.spi = {						\
			.slave = DT_REG_ADDR(node_id),			\
			.mode = LUDLC_DT_SPI_MODE(node_id),		\
			.data_ready = GPIO_DT_SPEC_GET_OR(node_id,	\
					data_ready_gpios, {0}),		\
		},							\
	}
#endif /* CONFIG_LUDLC_SPI_CONN */

#endif /* __LUDLC_DT_H__ */

// SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)
/**
 * @file ludlc.c
 *
 * @brief LuDLC demo executable main file.
 *
 * This application provides an interactive demo for the LuDLC serial
 * connection API, featuring a simple echo protocol. It demonstrates
 * connection setup, teardown, sending data, and handling asynchronous
 * receive and confirmation callbacks.
 *
 * Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
 *
 * This file is licensed under either the Apache License, Version 2.0,
 * or the GNU General Public License, version 2 or (at your option)
 * any later version.
 */

#include <stdio.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <error.h>
#include <getopt.h>
#include <sys/queue.h>
#include <pthread.h>
#include <stdatomic.h>

#include <ludlc.h>
#include <ludlc_serial.h>
#include <ludlc_logger.h>

/** @brief LuDLC channel used for the echo demo protocol. */
enum {
	ECHO_CHANNEL = CONFIG_LUDLC_CONTROL_CHANNEL + 1,
};

/** @brief Global flag, true if running in server mode (TODO). */
static bool g_server = false;
/** @brief Global condition variable for exit synchronization (currently unused). */
static pthread_cond_t g_exit_cv = PTHREAD_COND_INITIALIZER;
/** @brief Atomic flag set by the signal handler to request application exit. */
atomic_bool g_stop_flag = 0;

/** @brief Command-line options for getopt_long. */
static struct option long_options[] = {
	/* These options set a flag. */
	{ "help", no_argument, NULL, 'h' },
	{ "server", no_argument, NULL, 's' },
	{ 0, 0, 0, 0 }
};

/**
 * @struct wait_ack_litem
 * @brief A list item to track an allocated packet payload.
 *
 * When a packet is sent, its payload is allocated on the heap and
 * stored in this structure. This item is added to the `wait_ack_lhead`
 * queue. When the `on_confirm` callback is received, the corresponding
 * item is found, removed from the queue, and freed.
 */
struct wait_ack_litem {
	TAILQ_ENTRY(wait_ack_litem) list; /**!< TAILQ linkage. */
	size_t size;                      /**!< Size of the payload. */
	uint8_t payload[];                /**!< Flexible array for payload data. */
};

/** @brief TAILQ head for tracking packets awaiting acknowledgment. */
TAILQ_HEAD(wait_ack_lhead, wait_ack_litem);
/** @brief Global instance of the wait-for-ack queue head. */
static struct wait_ack_lhead wait_ack_lhead =
		TAILQ_HEAD_INITIALIZER(wait_ack_lhead);

/**
 * @brief Sends an echo packet and tracks it for acknowledgment.
 *
 * This function allocates a new packet buffer, copies the data,
 * decrements the first byte (which acts as a simple TTL), and enqueues
 * it for sending. The allocated buffer is tracked in the
 * `wait_ack_lhead` queue until it is confirmed.
 *
 * @param conn The LuDLC connection.
 * @param data Pointer to the payload data to send.
 * @param size Size of the payload data.
 * @return 0 on success, or -ENOMEM if allocation fails.
 */
static int send_echo_resp(struct ludlc_connection *conn, const uint8_t *data,
		ludlc_payload_size_t size)
{
	struct wait_ack_litem *item;
	int ret = 0;

	/* The first byte is a 'TTL' counter; stop if 0. */
	if (!data[0])
		return ret;

	item = calloc(1, sizeof(struct wait_ack_litem) + size);
	if (!item)
		return -ENOMEM;

	item->size = size;
	memcpy(item->payload, data, size);
	item->payload[0]--; /* Decrement the 'TTL' */

	TAILQ_INSERT_TAIL(&wait_ack_lhead, item, list);
	ret = ludlc_enqueue_data(conn, ECHO_CHANNEL, item->payload, size, 1);
	if (ret) {
		LUDLC_LOG_INFO("fail to send ECHO packet (%p)", conn);
		/* If enqueue fails, remove from list and
		 * free immediately */
		TAILQ_REMOVE(&wait_ack_lhead, item, list);
		free(item);
		return ret;
	}
	return 0;
}

/**
 * @brief Sends a predefined "Hello World!" echo request.
 *
 * The first byte '\2' acts as a simple TTL or hop counter for the echo.
 *
 * @param conn The LuDLC connection.
 * @return Result from send_echo_resp().
 */
static int send_echo(struct ludlc_connection *conn)
{
	static const char data[] = "\2Hello World!";
	return send_echo_resp(conn, data, sizeof(data));
}

/**
 * @brief Finds and frees a packet payload from the wait-ack queue.
 *
 * This is called by `on_packet_confirm` to clean up the memory
 * associated with a packet that has been successfully (or unsuccessfully)
 * transmitted.
 *
 * @param conn The LuDLC connection (unused).
 * @param data The payload pointer that was confirmed.
 * @param size The size of the payload.
 */
static void free_echo_item(struct ludlc_connection *conn, const void *data,
		ludlc_payload_size_t size)
{
	struct wait_ack_litem *item;
	TAILQ_FOREACH(item, &wait_ack_lhead, list) {
		if(item->payload == data && item->size == size) {
			TAILQ_REMOVE(&wait_ack_lhead, item, list);
			free(item);
			break;
		}
	}
}

/**
 * @brief Prints the command-line usage instructions.
 * @return 1 (indicating error).
 */
static int usage(void)
{
	fprintf(stderr, "Usage: ludlc [-h] [-s] /dev/ttyPort\n");
	return EXIT_FAILURE;
}

 /**
  * @brief LuDLC callback: Called when the connection is lost.
  * @param conn The connection that was disconnected.
  * @param user_arg User context (unused).
  */
static void on_disconnect(struct ludlc_connection *conn, void *user_arg)
{
	LUDLC_LOG_INFO("connection lost (%p)", conn);
}

/**
 * @brief LuDLC callback: Called when the connection is established.
 * @param conn The connection that was established.
 * @param user_arg User context (unused).
 */
static void on_connect(struct ludlc_connection *conn, void *user_arg)
{
	LUDLC_LOG_INFO("connection established (%p)", conn);
}

/**
 * @brief LuDLC callback: Called when a new packet is received.
 *
 * Dumps the packet to the console and, if it's on the ECHO_CHANNEL,
 * sends a response.
 *
 * @param conn The connection that received the packet.
 * @param chan The channel the packet was on.
 * @param data The packet payload.
 * @param size The payload size.
 * @param tstamp The reception timestamp.
 * @param user_arg User context (unused).
 */
static void handle_rx_packet(struct ludlc_connection *conn,
			ludlc_channel_t chan,
			const void *data,
			ludlc_payload_size_t size,
			ludlc_timestamp_t tstamp,
			void *user_arg)
{
	printf("--- packet arrive @%ull  ---\n",
			(unsigned long long)tstamp);
	printf(" channel: %u\n", chan);
	printf(" payload (len=%zu):\n  ", size);
	if (size == 0) {
		printf("(empty)");
	} else {
		const uint8_t *payload = data;
		for (ludlc_payload_size_t i = 0; i < size; i++) {
			printf("%02X ", payload[i]);
			if ((i + 1) % 16 == 0 && (i + 1) < size) {
				printf("\n  ");
			}
		}
	}
	printf("\n-------------------------\n\n");

	switch (chan) {
	case ECHO_CHANNEL:
		/* Send the packet back */
		send_echo_resp(conn, data, size);
		break;
	default:
		break;
	}
}

/**
 * @brief LuDLC callback: Called when a sent packet is confirmed (ACK'd or failed).
 *
 * Dumps the confirmation status and cleans up the corresponding
 * packet buffer from the wait-ack queue.
 *
 * @param conn The connection.
 * @param error 0 on success, or a negative error code on failure.
 * @param chan The channel the packet was sent on.
 * @param data The payload pointer of the confirmed packet.
 * @param data_size The size of the payload.
 * @param user_arg User context (unused).
 */
void on_packet_confirm(struct ludlc_connection *conn,
		int error,
		ludlc_channel_t chan,
		const void *data,
		ludlc_payload_size_t data_size,
		void *user_arg)
{
	printf("--- packet to channel %u confirmed  ---\n", chan);
	printf(" error code: %d\n", error);
	printf(" payload (len=%zu): %p\n  ", data_size, data);
	printf("-------------------------\n\n");

	switch (chan) {
	case ECHO_CHANNEL:
		/* Free the memory associated with this confirmed packet */
		free_echo_item(conn, data, data_size);
		break;
	default:
		break;
	}
}

static const uint16_t crc16_kermit_table[] = {
	0x0000, 0x1189, 0x2312, 0x329B, 0x4624, 0x57AD, 0x6536, 0x74BF,
	0x8C48, 0x9DC1, 0xAF5A, 0xBED3, 0xCA6C, 0xDBE5, 0xE97E, 0xF8F7,
	0x1081, 0x0108, 0x3393, 0x221A, 0x56A5, 0x472C, 0x75B7, 0x643E,
	0x9CC9, 0x8D40, 0xBFDB, 0xAE52, 0xDAED, 0xCB64, 0xF9FF, 0xE876,
	0x2102, 0x308B, 0x0210, 0x1399, 0x6726, 0x76AF, 0x4434, 0x55BD,
	0xAD4A, 0xBCC3, 0x8E58, 0x9FD1, 0xEB6E, 0xFAE7, 0xC87C, 0xD9F5,
	0x3183, 0x200A, 0x1291, 0x0318, 0x77A7, 0x662E, 0x54B5, 0x453C,
	0xBDCB, 0xAC42, 0x9ED9, 0x8F50, 0xFBEF, 0xEA66, 0xD8FD, 0xC974,
	0x4204, 0x538D, 0x6116, 0x709F, 0x0420, 0x15A9, 0x2732, 0x36BB,
	0xCE4C, 0xDFC5, 0xED5E, 0xFCD7, 0x8868, 0x99E1, 0xAB7A, 0xBAF3,
	0x5285, 0x430C, 0x7197, 0x601E, 0x14A1, 0x0528, 0x37B3, 0x263A,
	0xDECD, 0xCF44, 0xFDDF, 0xEC56, 0x98E9, 0x8960, 0xBBFB, 0xAA72,
	0x6306, 0x728F, 0x4014, 0x519D, 0x2522, 0x34AB, 0x0630, 0x17B9,
	0xEF4E, 0xFEC7, 0xCC5C, 0xDDD5, 0xA96A, 0xB8E3, 0x8A78, 0x9BF1,
	0x7387, 0x620E, 0x5095, 0x411C, 0x35A3, 0x242A, 0x16B1, 0x0738,
	0xFFCF, 0xEE46, 0xDCDD, 0xCD54, 0xB9EB, 0xA862, 0x9AF9, 0x8B70,
	0x8408, 0x9581, 0xA71A, 0xB693, 0xC22C, 0xD3A5, 0xE13E, 0xF0B7,
	0x0840, 0x19C9, 0x2B52, 0x3ADB, 0x4E64, 0x5FED, 0x6D76, 0x7CFF,
	0x9489, 0x8500, 0xB79B, 0xA612, 0xD2AD, 0xC324, 0xF1BF, 0xE036,
	0x18C1, 0x0948, 0x3BD3, 0x2A5A, 0x5EE5, 0x4F6C, 0x7DF7, 0x6C7E,
	0xA50A, 0xB483, 0x8618, 0x9791, 0xE32E, 0xF2A7, 0xC03C, 0xD1B5,
	0x2942, 0x38CB, 0x0A50, 0x1BD9, 0x6F66, 0x7EEF, 0x4C74, 0x5DFD,
	0xB58B, 0xA402, 0x9699, 0x8710, 0xF3AF, 0xE226, 0xD0BD, 0xC134,
	0x39C3, 0x284A, 0x1AD1, 0x0B58, 0x7FE7, 0x6E6E, 0x5CF5, 0x4D7C,
	0xC60C, 0xD785, 0xE51E, 0xF497, 0x8028, 0x91A1, 0xA33A, 0xB2B3,
	0x4A44, 0x5BCD, 0x6956, 0x78DF, 0x0C60, 0x1DE9, 0x2F72, 0x3EFB,
	0xD68D, 0xC704, 0xF59F, 0xE416, 0x90A9, 0x8120, 0xB3BB, 0xA232,
	0x5AC5, 0x4B4C, 0x79D7, 0x685E, 0x1CE1, 0x0D68, 0x3FF3, 0x2E7A,
	0xE70E, 0xF687, 0xC41C, 0xD595, 0xA12A, 0xB0A3, 0x8238, 0x93B1,
	0x6B46, 0x7ACF, 0x4854, 0x59DD, 0x2D62, 0x3CEB, 0x0E70, 0x1FF9,
	0xF78F, 0xE606, 0xD49D, 0xC514, 0xB1AB, 0xA022, 0x92B9, 0x8330,
	0x7BC7, 0x6A4E, 0x58D5, 0x495C, 0x3DE3, 0x2C6A, 0x1EF1, 0x0F78
};

/**
 * @brief Calculates the running CRC-16/KERMIT checksum for a single byte.
 *
 * This is a mandatory function for the `ludlc_proto_cb` if hardware
 * does not have integrated packet validation (like typical UARTs).
 *
 * @param crc The current CRC value.
 * @param data The new byte to incorporate into the CRC.
 * @return The updated CRC value.
 */
static ludlc_csum_t crc16_ccitt(ludlc_csum_t crc, const uint8_t data)
{
	return  (crc >> 8) ^ crc16_kermit_table[(crc ^ data) & 0xFF];
}

/**
 * @brief The `ludlc_proto_cb` implementation for the POSIX serial transport.
 */
static struct ludlc_proto_cb proto = {
	.csum_byte = crc16_ccitt,
	.get_timestamp = ludlc_default_get_timestamp,
};

/**
 * @brief Signal handler for SIGINT (Ctrl+C).
 *
 * Sets the global stop flag to request a graceful shutdown.
 * @param sig The signal number (unused).
 */
static void handle_sigint(int sig)
{
	(void)sig; // unused
	g_stop_flag = true;
}

/** @brief Global structure of LuDLC callbacks for this demo. */
static struct ludlc_conn_cb cb = {
	.on_disconnect = on_disconnect,
	.on_connect = on_connect,
	.on_recv = handle_rx_packet,
	.on_confirm = on_packet_confirm,
};

/**
 * @brief Displays the main menu to the user.
 */
static void displayMenu()
{
	printf("\n\n--- MAIN MENU ---\n");
	printf("1. Send Hello World packet to the ECHO channel \n");
	printf("0. Exit\n");
	printf("-----------------\n");
	printf("Enter your choice: ");
}

/**
 * @brief Handles the interactive user menu.
 *
 * Reads user input from stdin and triggers actions (like sending
 * an echo packet). This blocks the main thread, which is fine for a demo.
 *
 * @param conn The LuDLC connection to use for actions.
 * @return 0 to exit the menu, -1 to continue.
 */
static int menu_handler(struct ludlc_connection *conn)
{
	int choice = -1; /* Initialize choice to a value that doesn't exit */

	/* The main menu loop */
	do {
		displayMenu();

		int scanResult = scanf(" %d", &choice);

		if (scanResult != 1) {
			/* Check for EOF (e.g., Ctrl+D) or read error */
			if(errno != 0) {
				choice = 0;
			} else {
				printf(
				 "\nError: Invalid input. Please enter a number.\n");

				/* Clear the invalid input from the buffer */
				/* Read and discard characters until a newline is found */
				while (getchar() != '\n')
					;

				choice = -1; /* Reset choice to loop again */
				/* Skip the rest of this loop iteration */
				continue;
			}
		}

		/* Process the user's choice */
		switch (choice) {
		case 0:
			printf("\n>>> Exiting. Goodbye!\n");
			break;

		case 1:
			send_echo(conn);
			break;

		default:
			printf("\n>>> Invalid choice. "
				"Please select a number from 0 to 1.\n");
			break;
		}

		/* Pause for the user to see the result before looping */
		if (choice != 0) {
			printf("\nPress Enter to continue...");
			/* Clear the input buffer from the previous scanf */
			while (getchar() != '\n')
				;
			/* Wait for the user to press Enter */
			getchar();
		}

	} while (choice != 0);
}

/**
 * @brief Main entry point for the LuDLC demo application.
 *
 * Parses command-line arguments, sets up the SIGINT handler,
 * creates the serial connection, and runs the main menu loop
 * until shutdown is requested (by menu or Ctrl+C).
 */
int main (int argc, char **argv)
{
	int ret = EXIT_SUCCESS;
	int c;
	int option_index = 0;
	unsigned long baud;
	sigset_t set;
	struct sigaction sa;
	struct ludlc_connection *conn = NULL;
	int verb = LOG_INFO;

	ludlc_platform_args_t ser_args = {
		.baudrate = 115200UL,
	};

	/* Parse command-line options */
	while ((c = getopt_long(argc, argv, "b:shvq", long_options,
				&option_index)) != -1) {
		switch (c) {
		case 'v':
			if (++verb > LOG_TRACE) {
				verb = LOG_TRACE;
			}
			break;
		case 'q':
			if (verb) {
				verb--;
			}
			break;

		case 's':
/* TODO: */
			g_server = true;
		break;
		default:
/* TODO: */
//			return usage();
			break;
		}
	}

	log_set_level(verb);

	/* Use the remaining non-option argument as the port name */
	if (optind < argc)
		ser_args.port = argv[optind];

	if (!ser_args.port)
		return usage();

	/* Install signal handler for Ctrl+C */
	sa.sa_handler = handle_sigint;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	if (sigaction(SIGINT, &sa, NULL) == 0) {
		LUDLC_LOG_INFO("Create connection...");

		ret = ludlc_serial_connection_create(&ser_args, &conn,
							&proto, &cb);
		if ( !ret ) {
			/* Run main loop until stop flag is set (by menu or Ctrl+C) */
			while (!g_stop_flag) {
				if (!menu_handler(conn))
					break;
			}

			LUDLC_LOG_INFO("Exiting...");
			ludlc_serial_connection_destroy(conn);

			/* Clean up any packets still waiting for ack */
			while (!TAILQ_EMPTY(&wait_ack_lhead)) {
				struct wait_ack_litem *item =
						TAILQ_FIRST(&wait_ack_lhead);
				TAILQ_REMOVE(&wait_ack_lhead, item, list);
				free(item);
			}

		} else {
			LUDLC_LOG_ERROR("Creation of LuDLC serial connection "
					"failed: %s. Exit", strerror(-ret));
			ret = EXIT_FAILURE;
		}
	} else {
		perror("sigaction");
		ret =  EXIT_FAILURE;
	}

	return ret;
}

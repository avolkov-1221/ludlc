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
		ret = ludlc_serial_connection_create(&ser_args, &conn, &cb);
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

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
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <errno.h>
#include <error.h>
#include <getopt.h>
#include <sys/queue.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <termios.h>
#endif

#include <ludlc.h>
#include <ludlc_serial.h>
#include <ludlc_logger.h>

/** @brief LuDLC channel used for the echo demo protocol. */
enum {
	ECHO_CHANNEL = CONFIG_LUDLC_CONTROL_CHANNEL + 1,
};

enum demo_flags {
	DEMO_F_SERVER = (1U << 0),
	DEMO_F_UI_INITIALIZED = (1U << 1),
	DEMO_F_UI_INPUT_ACTIVE = (1U << 2),
	DEMO_F_UI_TS_INITIALIZED = (1U << 3),
};

/** @brief Bitfield for rarely-modified demo state/config toggles. */
static uint32_t g_demo_flags;

static inline bool demo_flag_test(enum demo_flags flag)
{
	return (g_demo_flags & (uint32_t)flag) != 0U;
}

static inline void demo_flag_set(enum demo_flags flag)
{
	g_demo_flags |= (uint32_t)flag;
}

static inline void demo_flag_clear(enum demo_flags flag)
{
	g_demo_flags &= ~((uint32_t)flag);
}

/** @brief Atomic flag set by the signal handler to request application exit. */
atomic_bool g_stop_flag = 0;
#ifndef _WIN32
/** @brief Atomic flag set when terminal geometry changes. */
atomic_bool g_resize_flag = 0;
#endif

enum long_option_id {
	OPT_LOG_HISTORY = 1000,
};

/** @brief Command-line options for getopt_long. */
static struct option long_options[] = {
	/* These options set a flag. */
	{ "help", no_argument, NULL, 'h' },
	{ "baudrate", required_argument, NULL, 'b' },
	{ "server", no_argument, NULL, 's' },
	{ "log-history", required_argument, NULL, OPT_LOG_HISTORY },
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

#define UI_LOG_LINE_LEN  256U
#define UI_LOG_HISTORY_DEFAULT 2000U
#define UI_LOG_HISTORY_MAX_LIMIT 100000U

enum menu_action_result {
	MENU_ACTION_CONTINUE = 0,
	MENU_ACTION_EXIT = 1,
};

struct menu_item {
	int id;
	const char *label;
	enum menu_action_result (*handler)(struct ludlc_connection *conn);
};

static char *g_ui_log_history;
static size_t g_ui_log_capacity = UI_LOG_HISTORY_DEFAULT;
static size_t g_ui_log_head;
static size_t g_ui_log_count;
static char g_ui_input_buf[64];
static size_t g_ui_input_len;

static pthread_mutex_t g_ui_lock = PTHREAD_MUTEX_INITIALIZER;

#ifdef _WIN32
static LARGE_INTEGER g_ui_ts_start_counter;
static LARGE_INTEGER g_ui_ts_frequency;
#else
static struct timespec g_ui_ts_start;
#endif

static void ui_redraw_locked(void);

static enum menu_action_result menu_action_reconnect(struct ludlc_connection *conn);
static enum menu_action_result menu_action_disconnect(struct ludlc_connection *conn);
static enum menu_action_result menu_action_send_echo(struct ludlc_connection *conn);
static enum menu_action_result menu_action_exit(struct ludlc_connection *conn);

static const struct menu_item g_menu_items[] = {
	{ 3, "Send disconnect", menu_action_disconnect },
	{ 2, "Try reconnect", menu_action_reconnect },
	{ 1, "Send Hello World packet to the ECHO channel", menu_action_send_echo },
	{ 0, "Exit", menu_action_exit },
};

static const size_t g_menu_items_count =
	sizeof(g_menu_items) / sizeof(g_menu_items[0]);

static size_t ui_footer_lines(void)
{
	/* menu header + items + separator + prompt */
	return g_menu_items_count + 3U;
}

static void ui_timestamp_init(void)
{
	if (demo_flag_test(DEMO_F_UI_TS_INITIALIZED)) {
		return;
	}

#ifdef _WIN32
	(void)QueryPerformanceFrequency(&g_ui_ts_frequency);
	(void)QueryPerformanceCounter(&g_ui_ts_start_counter);
#else
	(void)clock_gettime(CLOCK_MONOTONIC, &g_ui_ts_start);
#endif
	demo_flag_set(DEMO_F_UI_TS_INITIALIZED);
}

static uint64_t ui_timestamp_us(void)
{
#ifdef _WIN32
	LARGE_INTEGER now;
	uint64_t ticks;
	uint64_t freq;

	if (!demo_flag_test(DEMO_F_UI_TS_INITIALIZED)) {
		ui_timestamp_init();
	}

	(void)QueryPerformanceCounter(&now);
	ticks = (uint64_t)(now.QuadPart - g_ui_ts_start_counter.QuadPart);
	freq = (uint64_t)g_ui_ts_frequency.QuadPart;
	if (freq == 0U) {
		return 0U;
	}

	return (ticks * 1000000ULL) / freq;
#else
	struct timespec now;
	uint64_t sec;
	uint64_t nsec;

	if (!demo_flag_test(DEMO_F_UI_TS_INITIALIZED)) {
		ui_timestamp_init();
	}

	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	sec = (uint64_t)(now.tv_sec - g_ui_ts_start.tv_sec);
	if (now.tv_nsec >= g_ui_ts_start.tv_nsec) {
		nsec = (uint64_t)(now.tv_nsec - g_ui_ts_start.tv_nsec);
	} else {
		sec -= 1U;
		nsec = 1000000000ULL + (uint64_t)now.tv_nsec -
			(uint64_t)g_ui_ts_start.tv_nsec;
	}

	return sec * 1000000ULL + nsec / 1000ULL;
#endif
}

static void ui_make_timestamp(char *buffer, size_t size)
{
	if (!buffer || size == 0) {
		return;
	}

	snprintf(buffer, size, "%012lluus",
		(unsigned long long)ui_timestamp_us());
}

static int ui_configure_log_history(size_t capacity)
{
	char *buffer;

	if (capacity == 0 || capacity > UI_LOG_HISTORY_MAX_LIMIT) {
		return -EINVAL;
	}

	buffer = calloc(capacity, UI_LOG_LINE_LEN);
	if (!buffer) {
		return -ENOMEM;
	}

	free(g_ui_log_history);

	g_ui_log_history = buffer;
	g_ui_log_capacity = capacity;
	g_ui_log_head = 0;
	g_ui_log_count = 0;

	return 0;
}

static void ui_free_log_history(void)
{
	free(g_ui_log_history);

	g_ui_log_history = NULL;
	g_ui_log_head = 0;
	g_ui_log_count = 0;
}

static void ui_store_log_locked(const char *line)
{
	size_t idx;

	if (!line)
		line = "";

	if (!g_ui_log_history || g_ui_log_capacity == 0) {
		return;
	}

	if (g_ui_log_count < g_ui_log_capacity) {
		idx = (g_ui_log_head + g_ui_log_count) % g_ui_log_capacity;
		g_ui_log_count++;
	} else {
		idx = g_ui_log_head;
		g_ui_log_head = (g_ui_log_head + 1U) % g_ui_log_capacity;
	}

	snprintf(g_ui_log_history + (idx * UI_LOG_LINE_LEN), UI_LOG_LINE_LEN,
		"%s", line);
}

static void ui_reset_input_state_locked(void)
{
	demo_flag_clear(DEMO_F_UI_INPUT_ACTIVE);
	g_ui_input_len = 0;
	g_ui_input_buf[0] = '\0';
	ui_redraw_locked();
}

#ifdef _WIN32
static bool ui_take_resize_event(void)
{
	return false;
}

static int ui_install_resize_handler(void)
{
	return 0;
}
#else
static void handle_sigwinch(int sig)
{
	(void)sig;
	g_resize_flag = true;
}

static bool ui_take_resize_event(void)
{
	if (g_resize_flag) {
		g_resize_flag = false;
		return true;
	}

	return false;
}

static int ui_install_resize_handler(void)
{
	struct sigaction sa_winch;

	sa_winch.sa_handler = handle_sigwinch;
	sigemptyset(&sa_winch.sa_mask);
	sa_winch.sa_flags = 0;

	return sigaction(SIGWINCH, &sa_winch, NULL);
}
#endif

static void ui_print_menu_locked(void)
{
	size_t i;

	printf("=== MAIN MENU ===\n");
	for (i = 0; i < g_menu_items_count; i++) {
		printf("[%d] %s\n", g_menu_items[i].id, g_menu_items[i].label);
	}
	printf("=================\n");
}

static void ui_format_menu_choices(char *buffer, size_t size)
{
	size_t i;
	size_t used = 0;

	if (!buffer || !size)
		return;

	buffer[0] = '\0';
	for (i = 0; i < g_menu_items_count; i++) {
		int written = snprintf(buffer + used, size - used, "%s%d",
			(i == 0) ? "" : ",", g_menu_items[i].id);
		if (written < 0 || (size_t)written >= (size - used)) {
			break;
		}
		used += (size_t)written;
	}
}

static void ui_print_prompt_locked(void)
{
	char choices[64];

	ui_format_menu_choices(choices, sizeof(choices));
	printf("Select option [%s]: ", choices);
	if (demo_flag_test(DEMO_F_UI_INPUT_ACTIVE) && g_ui_input_len > 0) {
		printf("%s", g_ui_input_buf);
	}
}

static void ui_print_footer_locked(void)
{
	ui_print_menu_locked();
	ui_print_prompt_locked();
}

static void ui_update_layout_locked(void)
{
	if (!demo_flag_test(DEMO_F_UI_INITIALIZED)) {
		printf("=== LOG OUTPUT ===\n");
		demo_flag_set(DEMO_F_UI_INITIALIZED);
	}
}

static void ui_redraw_locked(void)
{
	bool had_layout = demo_flag_test(DEMO_F_UI_INITIALIZED);

	ui_update_layout_locked();
	if (had_layout) {
		/* Replace previous footer in-place (menu + prompt). */
		printf("\r\033[%zuA\033[J", ui_footer_lines() - 1U);
	}
	ui_print_footer_locked();
	fflush(stdout);
}

static void ui_handle_resize_event(void)
{
	if (ui_take_resize_event()) {
		pthread_mutex_lock(&g_ui_lock);
		ui_redraw_locked();
		pthread_mutex_unlock(&g_ui_lock);
	}
}

static void ui_append_log_locked(const char *line)
{
	bool had_layout = demo_flag_test(DEMO_F_UI_INITIALIZED);
	char rendered[UI_LOG_LINE_LEN];
	char tbuf[16];

	ui_make_timestamp(tbuf, sizeof(tbuf));
	snprintf(rendered, sizeof(rendered), "%s %s", tbuf, line ? line : "");

	ui_store_log_locked(rendered);
	ui_update_layout_locked();

	/* Replace previous footer with one new log line + fresh footer. */
	if (had_layout) {
		printf("\r\033[%zuA\033[J", ui_footer_lines() - 1U);
	}

	printf("%s\n", rendered);
	ui_print_footer_locked();
	fflush(stdout);
}

static void ui_shutdown(void)
{
	pthread_mutex_lock(&g_ui_lock);

	if (demo_flag_test(DEMO_F_UI_INITIALIZED)) {
		printf("\n");
		fflush(stdout);
	}

	pthread_mutex_unlock(&g_ui_lock);
}

static void ui_logf(const char *fmt, ...)
{
	char line[UI_LOG_LINE_LEN];
	va_list args;

	va_start(args, fmt);
	vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);

	pthread_mutex_lock(&g_ui_lock);
	ui_append_log_locked(line);
	pthread_mutex_unlock(&g_ui_lock);
}

static void ui_log_packet_hex(const void *data, ludlc_payload_size_t size)
{
	const uint8_t *payload = data;
	char hex_part[3 * 8 + 1];
	char ascii_part[8 + 1];
	ludlc_payload_size_t i;

	if (!size || !payload) {
		ui_logf("payload(len=0): (empty)");
		return;
	}

	ui_logf("payload(len=%zu):", size);

	for (i = 0; i < size; i += 8U) {
		size_t j;
		size_t pos = 0;
		size_t chunk_len = (size - i) < 8U ? (size - i) : 8U;

		for (j = 0; j < 8U; j++) {
			if (j < chunk_len) {
				uint8_t c = *payload++;

				pos += (size_t)snprintf(hex_part + pos,
					sizeof(hex_part) - pos, "%02X ", c);
				ascii_part[j] = (c >= 32U && c <= 126U) ?
					(char)c : '.';
			} else {
				pos += (size_t)snprintf(hex_part + pos,
					sizeof(hex_part) - pos, "   ");
				ascii_part[j] = ' ';
			}
		}
		ascii_part[8] = '\0';
		ui_logf("  %04zu: %-24s |%s|", (size_t)i, hex_part, ascii_part);
	}
}

static void ui_log_backend_callback(log_Event *ev)
{
	char msg[UI_LOG_LINE_LEN];
	char line[UI_LOG_LINE_LEN];

	if (!ev) {
		return;
	}

	msg[0] = '\0';
	(void)vsnprintf(msg, sizeof(msg), ev->fmt, ev->ap);

	(void)snprintf(line, sizeof(line), "%-5s: %s",
		       log_level_string(ev->level), msg);

	pthread_mutex_lock(&g_ui_lock);
	ui_append_log_locked(line);
	pthread_mutex_unlock(&g_ui_lock);
}

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
	if (!data[0]) {
		return ret;
	}

	item = calloc(1, sizeof(struct wait_ack_litem) + size);
	if (!item) {
		return -ENOMEM;
	}

	item->size = size;
	memcpy(item->payload, data, size);
	item->payload[0]--; /* Decrement the 'TTL' */

	TAILQ_INSERT_TAIL(&wait_ack_lhead, item, list);
	ret = ludlc_enqueue_data(conn, ECHO_CHANNEL, item->payload, size, 1);
	if (ret) {
		ui_logf("failed to queue ECHO packet (%p)", conn);
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

static enum menu_action_result menu_action_reconnect(
					struct ludlc_connection *conn)
{
	int ret = ludlc_connect(conn);

	if (ret) {
		ui_logf("failed to connect: %s", strerror(-ret));
	} else {
		ui_logf("connection procedure has started");
	}

	return MENU_ACTION_CONTINUE;
}

static enum menu_action_result menu_action_disconnect(
					struct ludlc_connection *conn)
{
	int ret = ludlc_send_disconnect(conn);

	if (ret) {
		ui_logf("failed to send DISCONECT packet: %s", strerror(-ret));
	} else {
		ui_logf("DISCONECT packet queued");
	}

	return MENU_ACTION_CONTINUE;
}

static enum menu_action_result menu_action_send_echo(struct ludlc_connection *conn)
{
	int ret = send_echo(conn);

	if (ret) {
		ui_logf("failed to send ECHO packet: %s", strerror(-ret));
	} else {
		ui_logf("ECHO packet queued");
	}

	return MENU_ACTION_CONTINUE;
}

static enum menu_action_result menu_action_exit(struct ludlc_connection *conn)
{
	(void)conn;
	ui_logf("Exiting. Goodbye!");
	return MENU_ACTION_EXIT;
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
 * @param prog Program name for usage prefix.
 * @param out Output stream (`stdout` for help, `stderr` for errors).
 * @return EXIT_SUCCESS when printing help, EXIT_FAILURE on error usage.
 */
static int usage(const char *prog, FILE *out)
{
	if (!prog || !*prog) {
		prog = "ludlc_demo";
	}
	if (!out) {
		out = stderr;
	}

	fprintf(out,
		"Usage: %s [options] <serial-port>\n"
		"\n"
		"Options:\n"
		"  -h, --help              Show this help and exit\n"
		"  -b, --baudrate <rate>   Serial baudrate (default: 115200)\n"
		"  -s, --server            Enable server mode (reserved/TODO)\n"
		"      --log-history <N>   Keep N UI log lines (1..%u)\n"
		"  -v                      Increase log verbosity (repeatable)\n"
		"  -q                      Decrease log verbosity (repeatable)\n",
		prog, UI_LOG_HISTORY_MAX_LIMIT);

	return (out == stdout) ? EXIT_SUCCESS : EXIT_FAILURE;
}

 /**
  * @brief LuDLC callback: Called when the connection is lost.
  * @param conn The connection that was disconnected.
  * @param user_arg User context (unused).
  */
static void on_disconnect(struct ludlc_connection *conn, void *user_arg)
{
	(void)user_arg;
	ui_logf("connection lost (%p)", conn);
}

/**
 * @brief LuDLC callback: Called when the connection is established.
 * @param conn The connection that was established.
 * @param user_arg User context (unused).
 */
static void on_connect(struct ludlc_connection *conn, void *user_arg)
{
	(void)user_arg;
	ui_logf("connection established (%p)", conn);
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
	(void)user_arg;
	ui_logf("packet received @%llu on channel %u",
		(unsigned long long)tstamp, chan);
	ui_log_packet_hex(data, size);

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
	(void)user_arg;
	ui_logf("packet confirm: chan=%u, error=%d, payload=%p, len=%zu",
		chan, error, data, data_size);

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
	(void)sig;
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
 * @brief Reads one numeric menu choice.
 *
 * @param out_choice Destination for parsed integer choice.
 * @return 0 on success, -1 on EOF/read error, -2 on invalid numeric input.
 */
static int ui_read_number(int *out_choice)
{
	char line[64];
	char *end = NULL;
	long value;

	if (!out_choice)
		return -1;

#ifdef _WIN32
	if (!fgets(line, sizeof(line), stdin)) {
		if (errno == EINTR) {
			return 1;
		}
		return -1;
	}
#else
	struct termios oldt;
	struct termios raw;

	if (tcgetattr(STDIN_FILENO, &oldt) != 0) {
		return -1;
	}

	raw = oldt;
	raw.c_lflag &= ~(ICANON | ECHO);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
		return -1;
	}

	pthread_mutex_lock(&g_ui_lock);
	demo_flag_set(DEMO_F_UI_INPUT_ACTIVE);
	g_ui_input_len = 0;
	g_ui_input_buf[0] = '\0';
	ui_redraw_locked();
	pthread_mutex_unlock(&g_ui_lock);

	while (true) {
		char ch = 0;
		ssize_t rd = read(STDIN_FILENO, &ch, 1);

		if (rd != 1) {
			if (rd < 0 && errno == EINTR) {
				pthread_mutex_lock(&g_ui_lock);
				ui_reset_input_state_locked();
				pthread_mutex_unlock(&g_ui_lock);
				(void)tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
				return 1;
			}
			pthread_mutex_lock(&g_ui_lock);
			ui_reset_input_state_locked();
			pthread_mutex_unlock(&g_ui_lock);
			(void)tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
			return -1;
		}

		if (ch == '\n' || ch == '\r') {
			break;
		}

		pthread_mutex_lock(&g_ui_lock);
		if ((ch == 0x7f || ch == 0x08) && g_ui_input_len > 0) {
			g_ui_input_len--;
			g_ui_input_buf[g_ui_input_len] = '\0';
			ui_redraw_locked();
		} else if (ch >= 32 && ch <= 126 &&
				g_ui_input_len + 1U < sizeof(g_ui_input_buf)) {
			g_ui_input_buf[g_ui_input_len++] = ch;
			g_ui_input_buf[g_ui_input_len] = '\0';
			ui_redraw_locked();
		}
		pthread_mutex_unlock(&g_ui_lock);
	}

	pthread_mutex_lock(&g_ui_lock);
	snprintf(line, sizeof(line), "%s", g_ui_input_buf);
	ui_reset_input_state_locked();
	pthread_mutex_unlock(&g_ui_lock);

	(void)tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
#endif

	errno = 0;
	value = strtol(line, &end, 10);
	if (end == line) {
		return -2;
	}

	while (*end == ' ' || *end == '\t') {
		end++;
	}
	if (*end != '\n' && *end != '\0') {
		return -2;
	}

	if (errno == ERANGE || value < INT_MIN || value > INT_MAX) {
		return -2;
	}

	*out_choice = (int)value;
	return 0;
}

/**
 * @brief Handles the interactive user menu.
 *
 * Menu is always visible. Choice is parsed as a number.
 *
 * @param conn The LuDLC connection to use for actions.
 * @return 0 when user requests exit.
 */
static int menu_handler(struct ludlc_connection *conn)
{
	pthread_mutex_lock(&g_ui_lock);
	ui_redraw_locked();
	pthread_mutex_unlock(&g_ui_lock);

	while (!g_stop_flag) {
		size_t i;
		const struct menu_item *selected = NULL;
		int choice = 0;
		int read_ret;
		char choices[64];

		ui_handle_resize_event();

		read_ret = ui_read_number(&choice);
		if (read_ret == 1) {
			ui_handle_resize_event();
			continue;
		}
		if (read_ret == -1) {
			if (g_stop_flag) {
				return 0;
			}
			ui_logf("input error while reading menu choice");
			continue;
		}
		if (read_ret == -2) {
			ui_format_menu_choices(choices, sizeof(choices));
			ui_logf("invalid choice (use %s)", choices);
			continue;
		}

		for (i = 0; i < g_menu_items_count; i++) {
			if (g_menu_items[i].id == choice) {
				selected = &g_menu_items[i];
				break;
			}
		}

		if (!selected) {
			ui_format_menu_choices(choices, sizeof(choices));
			ui_logf("invalid choice %d (use %s)", choice, choices);
			continue;
		}

		if (selected->handler(conn) == MENU_ACTION_EXIT) {
			return 0;
		}
	}

	return 0;
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
	size_t requested_log_history = UI_LOG_HISTORY_DEFAULT;
	struct sigaction sa;
	struct ludlc_connection *conn = NULL;
	int verb = LOG_INFO;

	ludlc_platform_args_t ser_args = {
		.baudrate = 115200UL,
	};

	ui_timestamp_init();

	/* Parse command-line options */
	while ((c = getopt_long(argc, argv, "b:shvq", long_options,
				&option_index)) != -1) {
		switch (c) {
		case 'h':
			return usage(argv[0], stdout);
		case 'b': {
			char *end = NULL;
			unsigned long value;

			errno = 0;
			value = strtoul(optarg, &end, 10);
			if (errno == ERANGE || end == optarg || *end != '\0' ||
			    value == 0 || value > UINT_MAX) {
				fprintf(stderr,
					"Invalid baudrate '%s' (expected 1..%u)\n",
					optarg, UINT_MAX);
				return usage(argv[0], stderr);
			}

			ser_args.baudrate = value;
			break;
		}
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
			demo_flag_set(DEMO_F_SERVER);
		break;
		case OPT_LOG_HISTORY: {
			char *end = NULL;
			unsigned long value;

			errno = 0;
			value = strtoul(optarg, &end, 10);
			if (errno == ERANGE || end == optarg || *end != '\0' ||
			    value == 0 || value > UI_LOG_HISTORY_MAX_LIMIT) {
				fprintf(stderr,
					"Invalid --log-history value '%s' "
					"(expected 1..%u)\n",
					optarg, UI_LOG_HISTORY_MAX_LIMIT);
				return usage(argv[0], stderr);
			}
			requested_log_history = (size_t)value;
			break;
		}
		default:
			return usage(argv[0], stderr);
		}
	}

	ret = ui_configure_log_history(requested_log_history);
	if (ret) {
		fprintf(stderr, "Failed to allocate %zu log lines: %s\n",
			requested_log_history, strerror(-ret));
		return EXIT_FAILURE;
	}

	/* Keep the screen stable by suppressing direct stderr logger output. */
	log_set_quiet(true);
	if (log_add_callback(ui_log_backend_callback, NULL, verb) != 0) {
		ui_logf("warning: failed to register logger callback; "
			"LUDLC_LOG output will be suppressed");
	}

	/* Use the remaining non-option argument as the port name */
	if (optind < argc)
		ser_args.port = argv[optind];

	if (!ser_args.port) {
		ui_free_log_history();
		return usage(argv[0], stderr);
	}

	/* Install signal handler for Ctrl+C */
	sa.sa_handler = handle_sigint;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	if (sigaction(SIGINT, &sa, NULL) == 0) {
		if (ui_install_resize_handler() != 0) {
			ui_logf("warning: failed to install SIGWINCH handler");
		}
		ui_logf("creating connection...");

		ret = ludlc_serial_connection_create(&ser_args, &conn,
							&proto, &cb);
		if ( !ret ) {
			/* Run interactive menu until user exits or Ctrl+C. */
			(void)menu_handler(conn);

			ui_logf("exiting...");
			ludlc_serial_connection_destroy(conn);

			/* Clean up any packets still waiting for ack */
			while (!TAILQ_EMPTY(&wait_ack_lhead)) {
				struct wait_ack_litem *item =
						TAILQ_FIRST(&wait_ack_lhead);
				TAILQ_REMOVE(&wait_ack_lhead, item, list);
				free(item);
			}

		} else {
			ui_logf("failed to create LuDLC serial connection: %s",
				strerror(-ret));
			ret = EXIT_FAILURE;
		}
	} else {
		perror("sigaction");
		ret =  EXIT_FAILURE;
	}

	ui_shutdown();
	ui_free_log_history();
	return ret;
}

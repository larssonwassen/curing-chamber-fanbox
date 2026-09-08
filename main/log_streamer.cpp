#include "log_streamer.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "mqtt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "consts.h"
#include "util.h"

extern "C" {
	#include <stdarg.h>
	#include <string.h>
	#include <stdio.h>
	#include <stdlib.h>
	#include <limits.h>
}

static RingbufHandle_t rb = nullptr;

static const char* TAG = "syslog";

constexpr uint32_t MAX_MSG_LEN = 192;
constexpr uint32_t MAX_LEVEL_LEN = 2;
constexpr uint32_t MAX_MODULE_LEN = 32;
constexpr uint32_t MAX_ESCAPED_MSG_LEN = MAX_MSG_LEN * 1.25;
constexpr uint32_t MAX_ESCAPED_MODULE_LEN = MAX_MODULE_LEN * 1.25;
struct LogItem {
	char msg[MAX_MSG_LEN];
	char level[MAX_LEVEL_LEN];
	char module[MAX_MODULE_LEN];
	uint32_t uptime_ms;
};

constexpr uint32_t BATCH_MAX_AGE_MS = 5000;
constexpr uint32_t BATCH_MAX_ITEMS = 32;
constexpr uint32_t BATCH_MAX_BYTES = BATCH_MAX_ITEMS * (
	MAX_ESCAPED_MSG_LEN +
	MAX_ESCAPED_MODULE_LEN +
	sizeof(LogItem::level) +
	10 + // uptime_ms rendered as decimal, not sizeof(uint32_t)
	1 +  // ',' separating entries
	52   // number of characters in the JSON log message template
);

static esp_log_level_t log_level_from_string(const char* level) {
	switch (level[0]) {
		case 'E': return ESP_LOG_ERROR;
		case 'W': return ESP_LOG_WARN;
		case 'I': return ESP_LOG_INFO;
		case 'D': return ESP_LOG_DEBUG;
		case 'V': return ESP_LOG_VERBOSE;
		default: return ESP_LOG_NONE;
	}
}

static int (*orig_vprintf)(const char*, va_list) = nullptr;
static int uart_vprintf(const char *fmt, va_list ap) {
	if (orig_vprintf) {
		return orig_vprintf(fmt, ap);
	}
	return vprintf(fmt, ap);
}
static int uart_printf(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	int n = uart_vprintf(fmt, ap);
	va_end(ap);
	return n;
}

int log_no_stream(const char* level, const char* tag, const char* fmt, ...) {
	char buff[MAX_MSG_LEN];

	// snprintf and vsnprintf return the length they *would* have written, not
	// the length they wrote. Adding that to an offset unchecked walks `buff + n`
	// off the end of the stack frame and underflows `sizeof(buff) - n` into a
	// huge size_t, handing the next vsnprintf an unbounded buffer.
	int n = snprintf(buff, sizeof(buff), "%s (%" PRIu32 ") %s: ", level, (uint32_t)esp_log_timestamp(), tag);
	if (n < 0) {
		return 0;
	}
	if ((size_t)n > sizeof(buff) - 1) {
		n = (int)sizeof(buff) - 1;
	}

	va_list ap;
	va_start(ap, fmt);
	int m = vsnprintf(buff + n, sizeof(buff) - (size_t)n, fmt, ap);
	va_end(ap);
	if (m > 0) {
		n += m;
		if ((size_t)n > sizeof(buff) - 1) {
			n = (int)sizeof(buff) - 1;
		}
	}

	// Reserve room for the newline, truncating the message if it filled buff.
	if ((size_t)n > sizeof(buff) - 2) {
		n = (int)sizeof(buff) - 2;
	}
	buff[n++] = '\n';
	buff[n] = '\0';

	// "%s", not buff. The assembled line contains caller-supplied text, so
	// passing it as the format string turns any '%' in a log message into a
	// format directive reading arguments that were never pushed.
	return uart_printf("%s", buff);
}

static int streamer_vprintf(const char* fmt, va_list ap) {
	va_list ap_copy;
	va_copy(ap_copy, ap);
	uart_vprintf(fmt, ap_copy);
	va_end(ap_copy);

	char buff[MAX_MSG_LEN];
	int n = vsnprintf(buff, sizeof(buff), fmt, ap);
	n = remove_ansi_escape_codes(buff);
	if (n == 0 || (n == 1 && buff[0] == '\0')) {
		return 0;
	}
	if (xRingbufferSend(rb, (void*)buff, n, pdMS_TO_TICKS(10)) != pdPASS) {
		UART_LOGE(TAG, "Failed to send log message to ringbuffer");
	}
	return n;
}

static bool next_line_token(const char** source_start, const char* source_end, int8_t open_char, int8_t close_char, const char** data_start, const char** data_end) {
	*data_start = nullptr;
	*data_end = nullptr;

	const char* p = *source_start;
	if (p >= source_end) {
		return false;
	}

	// Bounds test first, then dereference. The input is a slice of a larger
	// buffer and carries no null terminator, so `*p` evaluated at source_end
	// reads a byte this function does not own -- and if that byte happens to
	// match, the scan runs on past the end.
	if (open_char != -1) {
		while (p < source_end && *p != open_char) {
			++p;
		}
		if (p >= source_end) {
			UART_LOGV(TAG, "Could not find opening char '%c' in '%.*s'", (char)open_char, (int)(source_end - *source_start), *source_start);
			return false;
		}
	}

	*data_start = (open_char != -1) ? p + 1 : p;

	if (close_char != -1) {
		while (p < source_end && *p != close_char) {
			++p;
		}
		if (p >= source_end) {
			UART_LOGV(TAG, "Could not find closing char '%c' in '%.*s'", (char)close_char, (int)(source_end - *source_start), *source_start);
			*data_start = nullptr;
			return false;
		}
		*data_end = p;
	} else {
		*data_end = source_end;
	}

	// An open_char match on the final byte puts data_start at source_end, and
	// data_end + 1 lands one past it. Clamp both so the next call sees a valid
	// empty range rather than a pointer outside the buffer.
	if (*data_start > source_end) {
		*data_start = source_end;
	}
	*source_start = MIN(*data_end + 1, source_end);
	return true;
}
static bool next_line_token_int(const char** source_start, const char* source_end, int8_t open_char, int8_t close_char, uint32_t* out) {
	const char *start;
	const char *end;
	if (!next_line_token(source_start, source_end, open_char, close_char, &start, &end)) {
		return false;
	}
	return parse_int_fast(start, end, out);
}
static bool next_line_token_str(const char** source_start, const char* source_end, int8_t open_char, int8_t close_char, char* out, size_t out_cap) {
	const char *start;
	const char *end;
	if (!next_line_token(source_start, source_end, open_char, close_char, &start, &end)) {
		return false;
	}
	if (out_cap == 0) {
		return false;
	}
	if (start < end && *start == ' ') {
		++start;
	}
	// memcpy, not strncpy: the source is a slice with no null terminator, and
	// the exact length is already known. MIN over two size_t values, not a
	// ptrdiff_t against a size_t, which promotes and compares as unsigned.
	size_t avail = (start < end) ? (size_t)(end - start) : 0;
	size_t len = MIN(avail, out_cap - 1); // -1 to leave space for null terminator
	memcpy(out, start, len);
	out[len] = '\0';
	return true;
}

static bool check_log_level(char level) {
	switch (level) {
		case 'E': return true;
		case 'W': return true;
		case 'I': return true;
		case 'D': return true;
		case 'V': return true;
		default: return false;
	}
}

static void parse_log_line(const char* line, size_t line_len, LogItem* out) {
	// Examples: 
	// [0;32mI (56) boot: chip revision: v3.0[0m
	// I (56) boot: chip revision: v3.0
	// E (51231) wifi:connected data:2

	do {
		if (line_len >= 8 && check_log_level(line[0]) && line[2] == '(') {
			const char *ptr = line;
			const char *ptr_end = line + line_len;

			out->level[0] = line[0];
			out->level[1] = '\0';

			if (!next_line_token_int(&ptr, ptr_end, '(', ')', &out->uptime_ms)) {
				UART_LOGV(TAG, "Failed to parse uptime: '%.*s'", (int)(ptr_end - ptr), ptr);
				break;
			}

			if (!next_line_token_str(&ptr, ptr_end, ' ', ':', out->module, sizeof(out->module))) {
				UART_LOGV(TAG, "Failed to parse module: '%.*s'", (int)(ptr_end - ptr), ptr);
				break;
			}

			if (!next_line_token_str(&ptr, ptr_end, -1, -1, out->msg, sizeof(out->msg))) {
				UART_LOGV(TAG, "Failed to parse msg: '%.*s'", (int)(ptr_end - ptr), ptr);
				break;
			}

			// All good!
			return;
		}
	} while (false);

	// %.*s consumes an int; a size_t passed here is read as a negative
	// precision on the value range where it matters, which prints unbounded.
	UART_LOGW(TAG, "Failed to parse log line: '%.*s'", (int)MIN(line_len, (size_t)INT_MAX), line);

	// fallback
	strcpy(out->level, "I");
	strcpy(out->module, "-");
	size_t msg_len = MIN(line_len, MAX_MSG_LEN - 1);
	strncpy(out->msg, line, msg_len);
	// Make sure the string is null-terminated
	out->msg[msg_len] = '\0';
	out->uptime_ms = esp_log_timestamp();
}

// Publisher task
static char json_buffer[BATCH_MAX_BYTES];
static void log_streamer_task(void* arg) {
	LogItem batch[BATCH_MAX_ITEMS];
	int batch_read_idx = 0;
	int batch_write_idx = 0;
	char escaped_msg[MAX_ESCAPED_MSG_LEN];
	char escaped_module[MAX_ESCAPED_MODULE_LEN];

	// Buffer to hold incomplete lines across multiple ring buffer receives
	static char incomplete_line_buffer[MAX_MSG_LEN * 2];
	static size_t incomplete_line_len = 0;

	for (;;) {
		size_t sz = 0;
		char* logDataPtr = (char*)xRingbufferReceive(rb, &sz, pdMS_TO_TICKS(1000)); // returns pointer inside rb
		if (logDataPtr) {
			esp_log_level_t log_level = (esp_log_level_t)shared_attributes->streamerLogLevel.get();

			// Temporary buffer to combine incomplete line with new data
			char combined_buffer[MAX_MSG_LEN * 2 + MAX_MSG_LEN];
			size_t combined_len = 0;

			// Prepend any incomplete line from previous iteration
			if (incomplete_line_len > 0) {
				memcpy(combined_buffer, incomplete_line_buffer, incomplete_line_len);
				combined_len = incomplete_line_len;
			}

			// Append new data (up to remaining space)
			size_t copy_sz = MIN(sz, sizeof(combined_buffer) - combined_len);
			memcpy(combined_buffer + combined_len, logDataPtr, copy_sz);
			combined_len += copy_sz;

			// Process complete lines (those ending with '\n')
			const char* p = combined_buffer;
			const char* end = combined_buffer + combined_len;
			while (p < end) {
				const char* line_start = p;

				// Find the end of this line
				while (p < end && *p != '\n') {
					++p;
				}

				// Check if we found a complete line (ends with '\n')
				if (p < end && *p == '\n') {
					// Complete line found
					const char* line_end = p;
					size_t line_len = line_end - line_start;

					if (line_len > 0) {
						LogItem li;
						parse_log_line(line_start, line_len, &li);
						if (log_level_from_string(li.level) <= log_level) {
							memcpy(&batch[batch_write_idx], &li, sizeof(LogItem));
							batch_write_idx = (batch_write_idx + 1) % BATCH_MAX_ITEMS;
							if (batch_read_idx == batch_write_idx) {
								batch_read_idx = (batch_read_idx + 1) % BATCH_MAX_ITEMS;
							}
						}
					}
					++p; // Skip the '\n'
				} else {
					// Incomplete line - save it for next iteration
					size_t remaining_len = end - line_start;
					if (remaining_len > 0 && remaining_len < sizeof(incomplete_line_buffer)) {
						memcpy(incomplete_line_buffer, line_start, remaining_len);
						incomplete_line_len = remaining_len;
					} else if (remaining_len >= sizeof(incomplete_line_buffer)) {
						// Line is too long, discard it
						UART_LOGW(TAG, "Discarding incomplete line that's too long (%zu bytes)", remaining_len);
						incomplete_line_len = 0;
					}
					break; // Stop processing
				}
			}

			// If we processed all data, reset incomplete buffer
			if (p >= end) {
				incomplete_line_len = 0;
			}

			vRingbufferReturnItem(rb, logDataPtr);
		}

		if (!(xEventGroupGetBits(network_state_event_group) & MQTT_CONNECTED_BIT)) {
			continue;
		}

		int count = (batch_write_idx - batch_read_idx + BATCH_MAX_ITEMS) % BATCH_MAX_ITEMS;
		if (count > 0) {
			// Reserve the trailing "]\0" up front so the loop never has to
			// account for it.
			const size_t json_cap = sizeof(json_buffer) - 2;
			json_buffer[0] = '[';
			size_t pos = 1;
			int published = 0;

			while (batch_read_idx != batch_write_idx) {
				const LogItem* l = &batch[batch_read_idx];

				json_escape(l->msg, escaped_msg, sizeof(escaped_msg));
				json_escape(l->module, escaped_module, sizeof(escaped_module));

				// snprintf reports the length it *would* have written. Adding
				// that to pos unchecked lets pos exceed sizeof(json_buffer),
				// after which every `sizeof(json_buffer) - pos` underflows to a
				// huge size_t and the next entry is written past the end. So:
				// measure, then commit only if it actually fit.
				int n = snprintf(
					json_buffer + pos,
					json_cap - pos,
					"%s{\"log\":{\"msg\":\"%s\",\"level\":\"%s\",\"module\":\"%s\",\"uptime\":%lu}}",
					published > 0 ? "," : "",
					escaped_msg,
					l->level,
					escaped_module,
					(unsigned long)l->uptime_ms
				);
				if (n < 0 || (size_t)n >= json_cap - pos) {
					// Did not fit (or failed to encode). snprintf truncated at
					// `pos`, so everything before it is still valid JSON; the ']'
					// below overwrites the partial entry. Leave the item in the
					// batch for the next publish -- unless nothing at all fit,
					// in which case it never will and would block the queue.
					if (published == 0) {
						UART_LOGW(TAG, "Dropping log entry that does not fit the publish buffer");
						batch_read_idx = (batch_read_idx + 1) % BATCH_MAX_ITEMS;
						continue;
					}
					break;
				}
				pos += (size_t)n;
				published++;
				batch_read_idx = (batch_read_idx + 1) % BATCH_MAX_ITEMS;
			}

			if (published == 0) {
				continue;
			}

			json_buffer[pos++] = ']';
			json_buffer[pos] = '\0';

			int msgId = esp_mqtt_client_publish(mqtt_client, "v1/devices/me/telemetry", json_buffer, 0, 0, 0);
			if (msgId < 0) {
				UART_LOGE(TAG, "Failed to publish message, error=%d", msgId);
				continue;
			} else {
				UART_LOGI(TAG, "Published %d logs", published);
			}
		}
	}
}

void log_streamer_setup(void) {
	rb = xRingbufferCreate(10 * 1024, RINGBUF_TYPE_BYTEBUF);
	configASSERT(rb);
	orig_vprintf = esp_log_set_vprintf(streamer_vprintf);

	BaseType_t result = xTaskCreate(log_streamer_task, "LogStreamer", 16384, nullptr, tskIDLE_PRIORITY + 10, nullptr);
	if (result != pdPASS) {
		ESP_LOGE(TAG, "Failed to create LogStreamer");
	} else {
		ESP_LOGI(TAG, "LogStreamer created successfully.");
	}
}

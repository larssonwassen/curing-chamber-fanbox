// Minimal stand-in so JsonBuilder.h can be compiled on the host. It pulls in
// esp_log.h only for esp_log_level_t, which it accepts as a value type.
#pragma once

typedef enum {
	ESP_LOG_NONE,
	ESP_LOG_ERROR,
	ESP_LOG_WARN,
	ESP_LOG_INFO,
	ESP_LOG_DEBUG,
	ESP_LOG_VERBOSE,
} esp_log_level_t;

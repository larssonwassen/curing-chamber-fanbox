#include "ram_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#ifndef TAG
#define TAG "RAM"
#endif

static inline unsigned to_kb(size_t b) {
	return (unsigned)((b + 1023) / 1024);
}

void ram_log_snapshot(const char* tag) {
	const size_t tot_int = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
	const size_t free_int = heap_caps_get_free_size (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
	const size_t large_int = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

	const size_t tot_ps = heap_caps_get_total_size(MALLOC_CAP_SPIRAM   | MALLOC_CAP_8BIT);
	const size_t free_ps = heap_caps_get_free_size (MALLOC_CAP_SPIRAM   | MALLOC_CAP_8BIT);
	const size_t large_ps = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM   | MALLOC_CAP_8BIT);

	const char* t = tag ? tag : "";
	ESP_LOGI(TAG, "[%s] INTERNAL 8-bit: total=%u KB, free=%u KB, largest=%u KB", t, to_kb(tot_int), to_kb(free_int), to_kb(large_int));
	ESP_LOGI(TAG, "[%s] PSRAM    8-bit: total=%u KB, free=%u KB, largest=%u KB", t, to_kb(tot_ps), to_kb(free_ps), to_kb(large_ps));
	ESP_LOGI(TAG, "[%s] COMBINED      : total=%u KB, free=%u KB", t, to_kb(tot_int + tot_ps), to_kb(free_int + free_ps));
}

long unsigned int get_ram_free(void) {
	const size_t free_int = heap_caps_get_free_size (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
	const size_t free_ps = heap_caps_get_free_size (MALLOC_CAP_SPIRAM   | MALLOC_CAP_8BIT);
	return free_int + free_ps;
}

long unsigned int get_ram_total(void) {
	const size_t tot_int = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
	const size_t tot_ps = heap_caps_get_total_size(MALLOC_CAP_SPIRAM   | MALLOC_CAP_8BIT);
	return tot_int + tot_ps;
}

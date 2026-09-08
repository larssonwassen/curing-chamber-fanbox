#include "OtaUpdater.h"

#include "consts.h"
#include "mqtt.h"
#include "JsonParser/JsonParser.h"
#include "JsonBuilder/JsonBuilder.h"

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/md.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   // strcasecmp

static const char* TAG = "ota";

static const char* CHUNK_RESPONSE_PREFIX = "v2/fw/response/";
static const char* CHUNK_RESPONSE_FILTER = "v2/fw/response/+/chunk/+";

// A download that goes this long without a chunk arriving is re-requested. The
// broker can drop a request, and without this the device would sit in
// DOWNLOADING until it was power-cycled.
static const int64_t CHUNK_TIMEOUT_US = 30LL * 1000 * 1000;
static const int MAX_CHUNK_RETRIES = 5;

// How long to wait after a failed update before asking for the announcement
// again. ThingsBoard does not re-send it unaided.
static const int64_t FAILED_RETRY_US = 300LL * 1000 * 1000;

// How long a freshly-installed image gets to reach the broker before it gives
// up and reboots itself.
//
// The bootloader's rollback only triggers on a reset. An image that boots,
// stays up, and simply cannot reach the broker is therefore never rolled back
// -- it sits unconfirmed forever, which for a device sealed inside a chamber
// means someone has to open it and hold the reset line. This timer is what
// makes that case self-correcting.
//
// Ten minutes is deliberately generous: it has to cover a slow DHCP lease, a
// broker that is briefly down, or a WiFi network that comes back after the
// device does. Rebooting a good image costs a few seconds; failing to reboot a
// bad one costs a trip to the chamber.
static const TickType_t PENDING_VERIFY_TIMEOUT = pdMS_TO_TICKS(10 * 60 * 1000);

enum class State {
	Idle,
	Downloading,
	Failed,
};

namespace {

struct Download {
	State state = State::Idle;

	int request_id = 0;
	size_t chunk_index = 0;
	size_t received = 0;
	size_t expected = 0;
	int retries = 0;
	bool accepting_chunk = false;
	int64_t last_progress_us = 0;
	int64_t failed_at_us = 0;

	esp_ota_handle_t handle = 0;
	const esp_partition_t* partition = nullptr;
	// mbedtls_md rather than mbedtls_sha256: mbedTLS 4 moved sha256.h under
	// mbedtls/private/, and md.h covers every digest ThingsBoard offers anyway.
	mbedtls_md_context_t md;
	bool md_started = false;
	unsigned char digest_size = 0;

	char title[64] = {};
	char version[32] = {};
	char checksum[MBEDTLS_MD_MAX_SIZE * 2 + 1] = {}; // hex, longest is SHA-512
	char algorithm[16] = {};
};

Download g;
SemaphoreHandle_t g_lock = nullptr;

struct Lock {
	Lock()  { if (g_lock) xSemaphoreTake(g_lock, portMAX_DELAY); }
	~Lock() { if (g_lock) xSemaphoreGive(g_lock); }
};

/// Publish an fw_state update, plus the running version on terminal states.
void report(const char* fw_state, const char* error = nullptr) {
	if (!(xEventGroupGetBits(network_state_event_group) & MQTT_CONNECTED_BIT)) {
		return;
	}
	const esp_app_desc_t* app = esp_app_get_description();

	char payload[320];
	JsonBuilder jb(payload, sizeof(payload));
	jb.beginObject();
	jb.add("current_fw_title", app->project_name);
	jb.add("current_fw_version", app->version);
	jb.add("fw_state", fw_state);
	if (error != nullptr) {
		jb.add("fw_error", error);
	}
	jb.endObject();
	if (!jb.finalize()) {
		ESP_LOGE(TAG, "Failed to build fw_state payload");
		return;
	}

	ESP_LOGI(TAG, "fw_state=%s%s%s", fw_state, error ? " error=" : "", error ? error : "");
	// enqueue, not publish: most of these calls happen inside the MQTT event
	// handler, and esp_mqtt_client_publish at QoS 1 waits there for an ack that
	// the same task has to dispatch. enqueue hands the message to the outbox and
	// returns.
	esp_mqtt_client_enqueue(mqtt_client, "v1/devices/me/telemetry", jb.c_str(), jb.size(), 1, 0, true);
}

/// Release the OTA handle and move to Failed. Caller holds the lock.
void abortDownload(const char* why) {
	if (g.handle != 0) {
		esp_ota_abort(g.handle);
		g.handle = 0;
	}
	if (g.md_started) {
		mbedtls_md_free(&g.md);
		g.md_started = false;
	}
	g.state = State::Failed;
	g.accepting_chunk = false;
	g.failed_at_us = esp_timer_get_time();
	ESP_LOGE(TAG, "OTA aborted: %s", why);
	report("FAILED", why);
}

/// Ask the broker for the current chunk. Caller holds the lock.
void requestChunk(void) {
	char topic[64];
	snprintf(topic, sizeof(topic), "v2/fw/request/%d/chunk/%u",
			 g.request_id, (unsigned)g.chunk_index);

	// The payload is the requested chunk size as a decimal string.
	char payload[16];
	int n = snprintf(payload, sizeof(payload), "%u", (unsigned)OtaUpdater::CHUNK_SIZE);

	g.last_progress_us = esp_timer_get_time();
	// enqueue for the same reason as report(): this runs in the MQTT event
	// handler after each chunk lands. A request the broker never answers is
	// covered by the timeout in supervisorTask.
	int msg_id = esp_mqtt_client_enqueue(mqtt_client, topic, payload, n, 1, 0, true);
	if (msg_id < 0) {
		abortDownload("failed to publish chunk request");
		return;
	}
	ESP_LOGD(TAG, "Requested chunk %u (%u/%u bytes so far)",
			 (unsigned)g.chunk_index, (unsigned)g.received, (unsigned)g.expected);
}

/// Map a ThingsBoard fw_checksum_algorithm name to an mbedTLS digest.
mbedtls_md_type_t digestForAlgorithm(const char* algorithm) {
	if (strcasecmp(algorithm, "SHA256") == 0) return MBEDTLS_MD_SHA256;
	if (strcasecmp(algorithm, "SHA384") == 0) return MBEDTLS_MD_SHA384;
	if (strcasecmp(algorithm, "SHA512") == 0) return MBEDTLS_MD_SHA512;
	// MD5, CRC32 and the murmur variants ThingsBoard also offers are not
	// accepted: an image is only installed if its integrity was checked with a
	// digest worth trusting.
	return MBEDTLS_MD_NONE;
}

/// Compare the accumulated digest against the advertised checksum.
bool checksumMatches(void) {
	uint8_t digest[MBEDTLS_MD_MAX_SIZE];
	unsigned char size = g.digest_size;

	if (size == 0 || size > sizeof(digest) || mbedtls_md_finish(&g.md, digest) != 0) {
		return false;
	}
	mbedtls_md_free(&g.md);
	g.md_started = false;

	char hex[MBEDTLS_MD_MAX_SIZE * 2 + 1];
	for (unsigned char i = 0; i < size; i++) {
		snprintf(hex + i * 2, 3, "%02x", digest[i]);
	}
	hex[size * 2] = '\0';

	if (strcasecmp(hex, g.checksum) != 0) {
		ESP_LOGE(TAG, "Checksum mismatch: computed %s, expected %s", hex, g.checksum);
		return false;
	}
	return true;
}

/// Finish the image and reboot into it. Caller holds the lock.
void finishDownload(void) {
	if (!checksumMatches()) {
		abortDownload("checksum mismatch");
		return;
	}
	report("VERIFIED");

	esp_err_t err = esp_ota_end(g.handle);
	g.handle = 0;
	if (err != ESP_OK) {
		abortDownload(esp_err_to_name(err));
		return;
	}

	err = esp_ota_set_boot_partition(g.partition);
	if (err != ESP_OK) {
		abortDownload(esp_err_to_name(err));
		return;
	}

	report("UPDATING");
	ESP_LOGW(TAG, "Rebooting into %s to apply %s", g.partition->label, g.version);
	// Give the publish a moment to leave before the reboot cuts the socket.
	vTaskDelay(pdMS_TO_TICKS(1000));
	esp_restart();
}

/// Copy a string node into a fixed buffer. Returns false if absent or empty.
bool readString(JsonParser& jp, int root, const char* key, char* dest, size_t cap) {
	int idx = jp.find(root, key);
	if (idx < 0) {
		return false;
	}
	const JsonParser::Node& n = jp.node(idx);
	if (n.type != JsonParser::T_STRING || n.as.s == nullptr || n.as.s[0] == '\0') {
		return false;
	}
	strlcpy(dest, n.as.s, cap);
	return true;
}

/// Chunk index from "v2/fw/response/<req>/chunk/<n>", or -1.
int chunkIndexFromTopic(const char* topic) {
	const char* last = strrchr(topic, '/');
	if (last == nullptr || last[1] == '\0') {
		return -1;
	}
	char* end = nullptr;
	long v = strtol(last + 1, &end, 10);
	if (end == last + 1 || *end != '\0' || v < 0) {
		return -1;
	}
	return (int)v;
}

void supervisorTask(void* arg) {
	// Read this before waiting on anything: whether the running image still has
	// to prove itself decides how long that wait is allowed to be.
	const esp_partition_t* running = esp_ota_get_running_partition();
	esp_ota_img_states_t ota_state;
	bool pending_verify =
		esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
		ota_state == ESP_OTA_IMG_PENDING_VERIFY;

	if (pending_verify) {
		if (!waitForBit(MQTT_CONNECTED_BIT, PENDING_VERIFY_TIMEOUT)) {
			// Still unconfirmed and out of time. Rebooting is what hands the
			// decision to the bootloader, which boots the previous slot because
			// this image never called esp_ota_mark_app_valid_cancel_rollback().
			ESP_LOGE(TAG, "No broker connection %d minutes after an update; "
						  "rebooting %s to roll back",
					 (int)(PENDING_VERIFY_TIMEOUT / configTICK_RATE_HZ / 60), running->label);
			// No report() here -- there is no broker to report to, which is the
			// entire reason this branch was taken.
			vTaskDelay(pdMS_TO_TICKS(100)); // Let the log line reach the UART.
			esp_restart();
		}
	} else {
		waitForBit(MQTT_CONNECTED_BIT);
	}

	// Confirm the running image. Until this call the bootloader will roll back
	// to the previous slot on the next reset, which is exactly what should
	// happen to an image that cannot get this far -- it means WiFi and the
	// broker are both reachable on the new firmware.
	if (pending_verify) {
		esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
		if (err == ESP_OK) {
			ESP_LOGI(TAG, "Confirmed the new image on %s; rollback cancelled", running->label);
			report("UPDATED");
		} else {
			ESP_LOGE(TAG, "esp_ota_mark_app_valid_cancel_rollback failed: %s", esp_err_to_name(err));
		}
	} else {
		const esp_app_desc_t* app = esp_app_get_description();
		ESP_LOGI(TAG, "Running %s %s from %s", app->project_name, app->version, running->label);
		report("IDLE");
	}

	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(5000));

		Lock lock;
		if (g.state == State::Failed) {
			// ThingsBoard does not re-announce on its own, so a failed attempt
			// would otherwise wait for a reboot. Go back to Idle and ask for the
			// shared attributes again.
			if (esp_timer_get_time() - g.failed_at_us >= FAILED_RETRY_US) {
				ESP_LOGI(TAG, "Retrying the firmware announcement after a failure");
				g.state = State::Idle;
				request_attributes();
			}
			continue;
		}
		if (g.state != State::Downloading) {
			continue;
		}
		if (esp_timer_get_time() - g.last_progress_us < CHUNK_TIMEOUT_US) {
			continue;
		}
		if (++g.retries > MAX_CHUNK_RETRIES) {
			abortDownload("chunk request timed out");
			continue;
		}
		ESP_LOGW(TAG, "Chunk %u timed out, retry %d/%d",
				 (unsigned)g.chunk_index, g.retries, MAX_CHUNK_RETRIES);
		requestChunk();
	}
}

} // namespace

bool OtaUpdater::ownsTopic(const char* topic) {
	return strncmp(topic, CHUNK_RESPONSE_PREFIX, strlen(CHUNK_RESPONSE_PREFIX)) == 0;
}

void OtaUpdater::begin(void) {
	g_lock = xSemaphoreCreateMutex();
	configASSERT(g_lock);

	BaseType_t result = xTaskCreate(supervisorTask, "OtaSupervisor", 4096, nullptr,
									tskIDLE_PRIORITY + 5, nullptr);
	if (result != pdPASS) {
		ESP_LOGE(TAG, "Failed to create the OTA supervisor task");
	}
}

void OtaUpdater::subscribe(void) {
	int msg_id = esp_mqtt_client_subscribe(mqtt_client, CHUNK_RESPONSE_FILTER, 1);
	if (msg_id < 0) {
		ESP_LOGE(TAG, "Failed to subscribe to %s", CHUNK_RESPONSE_FILTER);
	} else {
		ESP_LOGI(TAG, "Subscribed to %s", CHUNK_RESPONSE_FILTER);
	}
}

void OtaUpdater::onAttributes(JsonParser& jp, int root) {
	char title[sizeof(g.title)];
	char version[sizeof(g.version)];
	char checksum[sizeof(g.checksum)];
	char algorithm[sizeof(g.algorithm)] = "SHA256";

	if (!readString(jp, root, "fw_title", title, sizeof(title)) ||
		!readString(jp, root, "fw_version", version, sizeof(version))) {
		return; // Not a firmware announcement.
	}

	int size_idx = jp.find(root, "fw_size");
	if (size_idx < 0 || jp.node(size_idx).type != JsonParser::T_NUMBER ||
		!jp.node(size_idx).number_is_int || jp.node(size_idx).i64 <= 0) {
		ESP_LOGE(TAG, "Firmware announced without a usable fw_size");
		return;
	}
	size_t size = (size_t)jp.node(size_idx).i64;

	if (!readString(jp, root, "fw_checksum", checksum, sizeof(checksum))) {
		ESP_LOGE(TAG, "Firmware announced without fw_checksum; refusing");
		return;
	}
	readString(jp, root, "fw_checksum_algorithm", algorithm, sizeof(algorithm));

	// Refuse before allocating anything rather than install an image whose
	// integrity was never checked.
	mbedtls_md_type_t digest_type = digestForAlgorithm(algorithm);
	if (digest_type == MBEDTLS_MD_NONE) {
		ESP_LOGE(TAG, "Unsupported fw_checksum_algorithm '%s'; refusing", algorithm);
		report("FAILED", "unsupported checksum algorithm");
		return;
	}

	const esp_app_desc_t* app = esp_app_get_description();
	if (strcmp(title, app->project_name) != 0) {
		ESP_LOGW(TAG, "Ignoring firmware '%s'; this device runs '%s'", title, app->project_name);
		return;
	}
	if (strcmp(version, app->version) == 0) {
		ESP_LOGD(TAG, "Already running %s %s", title, version);
		return;
	}

	Lock lock;
	if (g.state == State::Downloading) {
		if (strcmp(version, g.version) == 0) {
			return; // Already fetching this one.
		}
		abortDownload("superseded by a newer announcement");
	}

	const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
	if (target == nullptr) {
		ESP_LOGE(TAG, "No OTA partition available");
		report("FAILED", "no ota partition");
		return;
	}
	if (size > target->size) {
		ESP_LOGE(TAG, "Firmware is %u bytes; %s holds %u",
				 (unsigned)size, target->label, (unsigned)target->size);
		report("FAILED", "image larger than the partition");
		return;
	}

	esp_err_t err = esp_ota_begin(target, size, &g.handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
		report("FAILED", esp_err_to_name(err));
		return;
	}

	g.partition = target;
	g.expected = size;
	g.received = 0;
	g.chunk_index = 0;
	g.retries = 0;
	g.accepting_chunk = false;
	g.request_id++;
	strlcpy(g.title, title, sizeof(g.title));
	strlcpy(g.version, version, sizeof(g.version));
	strlcpy(g.checksum, checksum, sizeof(g.checksum));
	strlcpy(g.algorithm, algorithm, sizeof(g.algorithm));

	mbedtls_md_init(&g.md);
	const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(digest_type);
	if (mbedtls_md_setup(&g.md, md_info, 0) != 0 || mbedtls_md_starts(&g.md) != 0) {
		mbedtls_md_free(&g.md);
		esp_ota_abort(g.handle);
		g.handle = 0;
		ESP_LOGE(TAG, "Failed to initialise %s", algorithm);
		report("FAILED", "digest init failed");
		return;
	}
	g.md_started = true;
	g.digest_size = mbedtls_md_get_size(md_info);
	g.state = State::Downloading;

	ESP_LOGW(TAG, "Updating %s %s -> %s (%u bytes into %s)",
			 title, app->version, version, (unsigned)size, target->label);
	report("DOWNLOADING");
	requestChunk();
}

void OtaUpdater::onChunkData(const char* topic, const uint8_t* data, size_t len,
							 size_t offset, size_t total) {
	Lock lock;
	if (g.state != State::Downloading) {
		return;
	}

	// Only write the chunk that was asked for. A late reply to a retried request
	// would otherwise be written a second time. The decision is made once, on
	// the first fragment, and the rest of that message follows it -- fragments
	// of one message arrive contiguously.
	if (offset == 0) {
		int idx = chunkIndexFromTopic(topic);
		g.accepting_chunk = (idx >= 0 && (size_t)idx == g.chunk_index);
		if (!g.accepting_chunk) {
			ESP_LOGW(TAG, "Ignoring chunk %d; expecting %u", idx, (unsigned)g.chunk_index);
			return;
		}
		if (total == 0) {
			abortDownload("broker sent an empty chunk");
			return;
		}
	}

	if (!g.accepting_chunk || len == 0) {
		return;
	}
	if (g.received + len > g.expected) {
		abortDownload("broker sent more data than fw_size");
		return;
	}

	// Write straight through. Fragments arrive in order and each one is at most
	// the MQTT receive buffer, so nothing here holds a whole chunk.
	esp_err_t err = esp_ota_write(g.handle, data, len);
	if (err != ESP_OK) {
		abortDownload(esp_err_to_name(err));
		return;
	}
	if (mbedtls_md_update(&g.md, data, len) != 0) {
		abortDownload("digest update failed");
		return;
	}

	g.received += len;
	g.last_progress_us = esp_timer_get_time();

	// Wait for the rest of this chunk.
	if (offset + len < total) {
		return;
	}

	g.accepting_chunk = false;
	g.retries = 0;
	if (g.received >= g.expected) {
		ESP_LOGI(TAG, "Downloaded %u bytes", (unsigned)g.received);
		report("DOWNLOADED");
		finishDownload();
		return;
	}

	g.chunk_index++;
	requestChunk();
}

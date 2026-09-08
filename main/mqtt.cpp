#include "mqtt.h"
#include "esp_log.h"
#include <type_traits>   // std::remove_reference
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "JsonParser/JsonParser.h"
#include "JsonBuilder/JsonBuilder.h"
#include "consts.h"
#include "config/DeviceConfig.h"
#include "ota/OtaUpdater.h"
#include "esp_crt_bundle.h"
#include <string.h>

static const char *TAG = "mqtt";

esp_mqtt_client_handle_t mqtt_client = nullptr;

// Helper function to handle number attributes
template<typename T>
static bool handle_number_attribute(
	JsonParser& jp,
	int root,
	const char* key,
	T& attr
) {
	int idx = jp.find(root, key);
	if (idx < 0) {
		ESP_LOGV(TAG, "Failed to find %s", key);
		return false;
	}

	const JsonParser::Node& n = jp.node(idx);
	if (n.type == JsonParser::T_NUMBER) {
		attr.set(n.as.num);
		ESP_LOGI(TAG, "Updated %s to %f", key, (double)n.as.num);
		return true;
	}
	ESP_LOGE(TAG, "Failed to update %s", key);
	return false;
}

// Helper function to handle integer number attributes
template<typename T>
static bool handle_int_attribute(
	JsonParser& jp,
	int root,
	const char* key,
	T& attr
) {
	int idx = jp.find(root, key);
	if (idx < 0) {
		ESP_LOGV(TAG, "Failed to find %s", key);
		return false;
	}

	const JsonParser::Node& n = jp.node(idx);
	if (n.type == JsonParser::T_NUMBER && n.number_is_int) {
		auto v = static_cast<typename std::remove_reference<decltype(attr.get())>::type>(n.i64);
		attr.set(v);
		ESP_LOGI(TAG, "Updated %s to %d", key, (int)v);
		return true;
	}
	ESP_LOGE(TAG, "Failed to update %s", key);
	return false;
}

// Helper function to handle boolean attributes
template<typename T>
static bool handle_bool_attribute(
	JsonParser& jp,
	int root,
	const char* key,
	T& attr
) {
	int idx = jp.find(root, key);
	if (idx < 0) {
		ESP_LOGV(TAG, "Failed to find %s", key);
		return false;
	}

	const JsonParser::Node& n = jp.node(idx);
	if (n.type == JsonParser::T_BOOL) {
		bool v = n.as.b != 0;
		attr.set(v);
		ESP_LOGI(TAG, "Updated %s to %d", key, (int)v);
		return true;
	}
	ESP_LOGE(TAG, "Failed to update %s", key);
	return false;
}

int request_attributes_request_id = 0;
void request_attributes(void) {
	char topic[64];
	snprintf(topic, sizeof(topic), "v1/devices/me/attributes/request/%d", ++request_attributes_request_id);

	char payload[256];
	JsonBuilder jb(payload, sizeof(payload));
	jb.beginObject();
	jb.add("sharedKeys", "uart_log_level,streamer_log_level,fan_enabled,ctrl_loop_enabled,humidity_setpoint,humidity_overshoot_limit,humidity_undershoot_limit,fw_title,fw_version,fw_size,fw_checksum,fw_checksum_algorithm");
	jb.endObject();
	if (!jb.finalize()) {
		ESP_LOGE(TAG, "Failed to build attributes request message");
		return;
	}

	int rMsgId = esp_mqtt_client_publish(mqtt_client, topic, jb.c_str(), jb.size(), 0, 0);
	if (rMsgId < 0) {
		ESP_LOGE(TAG, "Failed to publish attributes request message, msgId=%d", rMsgId);
	} else {
		ESP_LOGI(TAG, "Triggered attributes request, msg_id=%d, topic=%s, request_id=%d", rMsgId, topic, request_attributes_request_id);
	}
}

void subscribe(const char* topic) {
	int pMsgId = esp_mqtt_client_subscribe(mqtt_client, topic, 1);
	if (pMsgId < 0) {
		ESP_LOGE(TAG, "Failed to subscribe to %s, msg_id=%d", topic, pMsgId);
	} else {
		ESP_LOGI(TAG, "Subscribed to %s", topic);
	}
}

// Reassembly state for MQTT_EVENT_DATA. Only ever touched from the MQTT client
// task, which delivers events serially, so no locking is needed.
static char rx_topic[128];
static char rx_payload[4096];
static size_t rx_len = 0;
static bool rx_overflow = false;
static bool rx_is_ota = false;

static bool topic_starts_with(const char* topic, const char* prefix) {
	return strncmp(topic, prefix, strlen(prefix)) == 0;
}

/**
 * @brief Dispatch one fully reassembled MQTT message.
 *
 * @param topic Null-terminated topic the message arrived on.
 * @param data  Mutable payload buffer; JsonParser unescapes strings in place.
 * @param len   Payload length. Not null-terminated.
 */
static void handle_mqtt_message(const char* topic, char* data, size_t len) {
	// Dispatch on the topic before touching the payload. Every subscription
	// today carries JSON, but the OTA chunk topics are raw binary, and feeding
	// those to the JSON parser produces nothing but parse errors.
	if (!topic_starts_with(topic, "v1/devices/me/attributes")) {
		ESP_LOGD(TAG, "Ignoring message on unhandled topic '%s'", topic);
		return;
	}

	ESP_LOGD(TAG, "Attributes message on '%s': %.*s", topic, (int)MIN(len, (size_t)256), data);

	static JsonParser::Node nodes[32];
	JsonParser jp(nodes, 32);

	int root = jp.parse(data, len);
	if (root < 0) {
		ESP_LOGE(TAG, "Parse error: %s", jp.last_error());
		return;
	}
	int dataRoot = root;
	int shared = jp.find(root, "shared");
	if (shared >= 0) {
		dataRoot = shared;
	}
	// Handle integer attributes
	handle_int_attribute(jp, dataRoot, "uart_log_level", shared_attributes->uartLogLevel);
	handle_int_attribute(jp, dataRoot, "streamer_log_level", shared_attributes->streamerLogLevel);

	// Handle boolean attributes
	handle_bool_attribute(jp, dataRoot, "fan_enabled", shared_attributes->fanEnabled);
	handle_bool_attribute(jp, dataRoot, "ctrl_loop_enabled", shared_attributes->ctrlLoopEnabled);

	// Handle floating point attributes
	handle_number_attribute(jp, dataRoot, "humidity_setpoint", shared_attributes->humiditySetpoint);
	handle_number_attribute(jp, dataRoot, "humidity_overshoot_limit", shared_attributes->humidityOvershootLimit);
	handle_number_attribute(jp, dataRoot, "humidity_undershoot_limit", shared_attributes->humidityUndershootLimit);

	esp_log_level_set("*", shared_attributes->uartLogLevel.get());

	// The same document carries the fw_* keys when ThingsBoard announces a
	// firmware update.
	OtaUpdater::onAttributes(jp, dataRoot);
}

static void mqtt_event_handler(void* event_handler_arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
	esp_mqtt_event_t* event = (esp_mqtt_event_t*)event_data;
	switch (event_id) {
		case MQTT_EVENT_BEFORE_CONNECT:
			ESP_LOGD(TAG, "BEFORE_CONNECT");
			break;
		case MQTT_EVENT_CONNECTED: {
			ESP_LOGI(TAG, "CONNECTED");
			xEventGroupSetBits(network_state_event_group, MQTT_CONNECTED_BIT);
			break;
		}
		case MQTT_EVENT_DISCONNECTED:
			ESP_LOGW(TAG, "DISCONNECTED");
			xEventGroupClearBits(network_state_event_group, MQTT_CONNECTED_BIT);
			break;
		case MQTT_EVENT_SUBSCRIBED:
			ESP_LOGD(TAG, "SUBSCRIBED, msg_id=%d", event->msg_id);
			break;
		case MQTT_EVENT_UNSUBSCRIBED:
			ESP_LOGD(TAG, "UNSUBSCRIBED, msg_id=%d", event->msg_id);
			break;
		case MQTT_EVENT_PUBLISHED:
			ESP_LOGD(TAG, "PUBLISHED, msg_id=%d", event->msg_id);
			break;
		case MQTT_EVENT_DATA: {
			// A payload larger than the client's receive buffer is delivered as
			// a sequence of MQTT_EVENT_DATA events carrying successive slices.
			// Only the first carries the topic; the rest are continuations
			// identified by current_data_offset. Parsing each slice on its own
			// fails, so reassemble before dispatching.
			if (event->current_data_offset == 0) {
				rx_len = 0;
				rx_overflow = false;

				size_t topic_len = MIN((size_t)event->topic_len, sizeof(rx_topic) - 1);
				if (event->topic != nullptr && topic_len > 0) {
					memcpy(rx_topic, event->topic, topic_len);
				}
				rx_topic[topic_len] = '\0';
				if ((size_t)event->topic_len >= sizeof(rx_topic)) {
					ESP_LOGW(TAG, "Topic truncated to %u of %d bytes",
							 (unsigned)(sizeof(rx_topic) - 1), event->topic_len);
				}
				rx_is_ota = OtaUpdater::ownsTopic(rx_topic);
			}

			if (rx_is_ota) {
				// Firmware chunks are raw binary and can be far larger than the
				// reassembly buffer. Hand each fragment straight to the updater,
				// which writes it into the inactive OTA slot as it arrives.
				OtaUpdater::onChunkData(rx_topic, (const uint8_t*)event->data,
										(size_t)event->data_len,
										(size_t)event->current_data_offset,
										(size_t)event->total_data_len);
				break;
			}

			if (event->data_len > 0) {
				// Copy rather than parse in place: JsonParser unescapes strings
				// by writing into the buffer, and event->data points into the
				// MQTT client's own receive buffer, which is not ours to modify.
				size_t n = MIN((size_t)event->data_len, sizeof(rx_payload) - rx_len);
				memcpy(rx_payload + rx_len, event->data, n);
				rx_len += n;
				if (n < (size_t)event->data_len) {
					rx_overflow = true;
				}
			}

			if (event->current_data_offset + event->data_len < event->total_data_len) {
				break; // More fragments to come.
			}

			if (rx_overflow) {
				ESP_LOGE(TAG, "Payload of %d bytes exceeds the %u byte reassembly buffer; dropped",
						 event->total_data_len, (unsigned)sizeof(rx_payload));
				rx_len = 0;
				break;
			}

			handle_mqtt_message(rx_topic, rx_payload, rx_len);
			rx_len = 0;
			break;
		}
		case MQTT_EVENT_ERROR: {
			const char* errorType = "";
			char details_buff[128];
			details_buff[0] = '\0';
			esp_mqtt_error_codes_t* eh = event->error_handle;
			switch (eh->error_type) {
				case MQTT_ERROR_TYPE_NONE:
					errorType = "NONE";
					break;
				case MQTT_ERROR_TYPE_TCP_TRANSPORT: {
					errorType = "TCP_TRANSPORT";
					snprintf(details_buff, sizeof(details_buff), "-> esp_tls_last_esp_err=%d esp_tls_stack_err=%d esp_tls_cert_verify_flags=%d", eh->esp_tls_last_esp_err, eh->esp_tls_stack_err, eh->esp_tls_cert_verify_flags);
					break;
				}
				case MQTT_ERROR_TYPE_CONNECTION_REFUSED: {
					errorType = "CONNECTION_REFUSED";
					const char* retCode = "";
					switch (eh->connect_return_code) {
						case MQTT_CONNECTION_ACCEPTED: retCode = "CONNECTION_ACCEPTED"; break;
						case MQTT_CONNECTION_REFUSE_PROTOCOL: retCode = "CONNECTION_REFUSE_PROTOCOL"; break;
						case MQTT_CONNECTION_REFUSE_ID_REJECTED: retCode = "CONNECTION_REFUSE_ID_REJECTED"; break;
						case MQTT_CONNECTION_REFUSE_SERVER_UNAVAILABLE: retCode = "CONNECTION_REFUSE_SERVER_UNAVAILABLE"; break;
						case MQTT_CONNECTION_REFUSE_BAD_USERNAME: retCode = "CONNECTION_REFUSE_BAD_USERNAME"; break;
						case MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED: retCode = "CONNECTION_REFUSE_NOT_AUTHORIZED"; break;
					}
					snprintf(details_buff, sizeof(details_buff), "-> retCode=%s", retCode);
					break;
				}
				case MQTT_ERROR_TYPE_SUBSCRIBE_FAILED:
					errorType = "SUBSCRIBE_FAILED";
					break;
			}
			ESP_LOGW(TAG, "ERROR -> %s %s", errorType, details_buff);
			break;
		}

		default:
			ESP_LOGE(TAG, "MQTT_EVENT_UNKNOWN, event_id=%d", event->event_id);
			break;
	}
}

static void mqtt_setup_task(void* arg) {
	esp_mqtt_client_config_t mqtt_cfg = {};
	mqtt_cfg.broker.address.uri = DeviceConfig::mqttUri();
	mqtt_cfg.credentials.username = DeviceConfig::mqttToken();
	if (DeviceConfig::mqttUsesTls()) {
		// Verify against the bundled root CA set. Whether the broker is reached
		// over TLS is decided by the provisioned URI's scheme, not by a build
		// flag, so moving a device to mqtts:// is a re-provision rather than a
		// reflash.
		mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
	}

	mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
	configASSERT(mqtt_client);

	esp_mqtt_client_register_event(mqtt_client, MQTT_EVENT_ANY, mqtt_event_handler, NULL);

	waitForBit(WIFI_CONNECTED_BIT);
	esp_err_t ret = esp_mqtt_client_start(mqtt_client);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to start MQTT client, error=%d", ret);
		return;
	}
	ESP_LOGI(TAG, "MQTT client started successfully");

	for (;;) {
		waitForBit(MQTT_CONNECTED_BIT);

		// Re-subscribe on every connect. The broker discards subscriptions when
		// the session ends and esp-mqtt does not replay them, so after a WiFi
		// blip the device used to stay connected but deaf -- no shared
		// attributes, and now no firmware announcements either.
		subscribe("v1/devices/me/attributes");
		subscribe("v1/devices/me/attributes/response/+");
		OtaUpdater::subscribe();
		request_attributes();

		// Event groups can only wait for bits to be set, so poll for the drop.
		// This task has nothing else to do in the meantime.
		while (xEventGroupGetBits(network_state_event_group) & MQTT_CONNECTED_BIT) {
			vTaskDelay(pdMS_TO_TICKS(1000));
		}
		ESP_LOGI(TAG, "Disconnected; will re-subscribe on the next connect");
	}
}

void mqtt_setup(void) {
	BaseType_t result = xTaskCreate(
		mqtt_setup_task,
		"MQTTSetupTask",
		8192,
		NULL,
		tskIDLE_PRIORITY + 10,
		NULL
	);
	if (result != pdPASS) {
		ESP_LOGE(TAG, "Failed to create MQTT setup task");
	} else {
		ESP_LOGI(TAG, "MQTT setup task created successfully");
	}
}

#include "mqtt.h"
#include "esp_log.h"
#include <type_traits>   // std::remove_reference
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "JsonParser/JsonParser.h"
#include "JsonBuilder/JsonBuilder.h"
#include "consts.h"

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
	jb.add("sharedKeys", "uart_log_level,streamer_log_level,fan_enabled,ctrl_loop_enabled,humidity_setpoint,humidity_overshoot_limit,humidity_undershoot_limit");
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
			ESP_LOGI(TAG, "DATA (topic=%.*s, data=%.*s)", event->topic_len, event->topic, event->data_len, event->data);
			static JsonParser::Node nodes[32];
			JsonParser jp(nodes, 32);

			int root = jp.parse(event->data, event->data_len);
			if (root < 0) {
				ESP_LOGE(TAG, "Parse error: %s", jp.last_error());
				break;
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
					sprintf(details_buff, "-> esp_tls_last_esp_err=%d esp_tls_stack_err=%d esp_tls_cert_verify_flags=%d", eh->esp_tls_last_esp_err, eh->esp_tls_stack_err, eh->esp_tls_cert_verify_flags);
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
					sprintf(details_buff, "-> retCode=%s", retCode);
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
	const esp_mqtt_client_config_t mqtt_cfg = {
		.broker = {
			.address = {
				.uri = MQTT_URL,
			},
		},
		.credentials = {
			.username = MQTT_ACCESS_TOKEN,
		},
	};

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

	waitForBit(MQTT_CONNECTED_BIT);
	subscribe("v1/devices/me/attributes");
	subscribe("v1/devices/me/attributes/response/+");
 	request_attributes();

	vTaskDelete(NULL);
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

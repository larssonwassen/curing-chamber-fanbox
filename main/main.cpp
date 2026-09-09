#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_random.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_crc.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "pins.h"
#include "I2C/I2CBus.h"
#include "I2C/I2CDevice.h"
#include "EMC2101/EMC2101.h"
#include "JsonBuilder/JsonBuilder.h"
#include "ram_info.h"
#include "consts.h"
#include "log_streamer.h"
#include "mqtt.h"
#include "config/DeviceConfig.h"
#include "ota/OtaUpdater.h"
#include "climate/ClimateSensor.h"
#include "pot/Potentiometer.h"
#include "plate/PlateProbe.h"
#include "control/Ventilation.h"
#include "net/TimeSync.h"


static const char *TAG = "curing-chamber-fanbox";

// WiFi reconnection backoff.
//
// This used to stop after five attempts and set WIFI_FAIL_BIT, which nothing
// ever read -- so five consecutive disconnects took the device off the network
// permanently: no telemetry, no shared attributes, and no way to push firmware
// to it. A router reboot was enough to do it. There is no attempt count now,
// only a growing delay, because there is no number of failures after which
// giving up is the right answer for a device sealed in a chamber.
static const uint32_t WIFI_RETRY_MIN_MS = 1000;
static const uint32_t WIFI_RETRY_MAX_MS = 60000;
static uint32_t s_wifi_retry_delay_ms = WIFI_RETRY_MIN_MS;
static esp_timer_handle_t s_wifi_retry_timer = nullptr;

static void schedule_wifi_retry(void) {
	if (s_wifi_retry_timer == nullptr) {
		return;
	}
	// Stop first: a disconnect can arrive while a retry is already armed.
	esp_timer_stop(s_wifi_retry_timer);
	esp_err_t err = esp_timer_start_once(s_wifi_retry_timer,
										 (uint64_t)s_wifi_retry_delay_ms * 1000);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to arm the WiFi retry timer: %s", esp_err_to_name(err));
		return;
	}
	ESP_LOGI(TAG, "WiFi reconnect in %u ms", (unsigned)s_wifi_retry_delay_ms);
	s_wifi_retry_delay_ms = MIN(s_wifi_retry_delay_ms * 2, WIFI_RETRY_MAX_MS);
}

static void wifi_retry_cb(void* arg) {
	esp_err_t err = esp_wifi_connect();
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "esp_wifi_connect() failed: %s", esp_err_to_name(err));
		schedule_wifi_retry();
	}
}

TaskHandle_t fan_task_handle = nullptr;
TaskHandle_t telemetry_task_handle = nullptr;

static void wifi_event_handler(void* event_handler_arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
		ESP_LOGI(TAG, "Wifi started");
		ESP_ERROR_CHECK(esp_wifi_connect());
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
		ESP_LOGI(TAG, "Disconnected");
		xEventGroupClearBits(network_state_event_group, WIFI_CONNECTED_BIT);
		schedule_wifi_retry();
	} else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		s_wifi_retry_delay_ms = WIFI_RETRY_MIN_MS;
		xEventGroupSetBits(network_state_event_group, WIFI_CONNECTED_BIT);
	}
}

static void wifi_setup(void)
{
	ESP_ERROR_CHECK(esp_netif_init());

	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_create_default_wifi_sta();

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));

	ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

	const esp_timer_create_args_t retry_timer_args = {
		.callback = wifi_retry_cb,
		.arg = nullptr,
		.dispatch_method = ESP_TIMER_TASK,
		.name = "wifi_retry",
		.skip_unhandled_events = true,
	};
	ESP_ERROR_CHECK(esp_timer_create(&retry_timer_args, &s_wifi_retry_timer));

	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	wifi_config_t wifi_config = { };
	// strlcpy, not strcpy: the credentials come from NVS, and sta.ssid /
	// sta.password are fixed 32- and 64-byte fields.
	strlcpy((char*)wifi_config.sta.ssid, DeviceConfig::wifiSsid(), sizeof(wifi_config.sta.ssid));
	strlcpy((char*)wifi_config.sta.password, DeviceConfig::wifiPsk(), sizeof(wifi_config.sta.password));
	// A minimum, not a requirement: WPA3 sorts above WPA2 in this enum, so a
	// WPA3-SAE network still associates as WPA3. Pinning it to WPA3_PSK refused
	// WPA2-only networks outright, which is a hard failure for anyone running
	// this firmware on a different network than the one it was written on.
	wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
	wifi_config.sta.pmf_cfg.capable = true;
	wifi_config.sta.pmf_cfg.required = false;

	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

	// SSID only. The PSK used to be logged here alongside it, which put the
	// network's credential into the UART log and, via log_streamer, into the
	// telemetry stream.
	ESP_LOGI(TAG, "Starting WIFI. SSID: [%s]", DeviceConfig::wifiSsid());
	ESP_ERROR_CHECK(esp_wifi_start());
}

// Owned exclusively by fan_task. Nothing else touches it: the EMC2101 driver
// holds no lock, and every read is a multi-register I2C transaction that a
// concurrent write would interleave with. Other tasks read telemetry->fanRPM,
// which fan_task publishes every 100 ms.
static EMC2101 fan;
void fan_task(void* arg) {
	const int max_retries = 3;
	int retries = 0;
	while (!fan.begin()) {
		vTaskDelay(pdMS_TO_TICKS(1000));
		if (retries++ == max_retries) {
			ESP_LOGE(TAG, "Exceeded maximum retries for fan initialization");
			while (true) {
				vTaskDelay(pdMS_TO_TICKS(10000));
			}
		}
	}

	if (!fan.setDataRate(EMC2101_RATE_32_HZ)) {
		ESP_LOGE(TAG, "Failed to set data rate");
	}

	switch (fan.getDataRate()) {
		case EMC2101_RATE_1_16_HZ: ESP_LOGI(TAG, "FAN DATA RATA IS: 1/16_HZ"); break;
		case EMC2101_RATE_1_8_HZ: ESP_LOGI(TAG, "FAN DATA RATA IS: 1/8_HZ"); break;
		case EMC2101_RATE_1_4_HZ: ESP_LOGI(TAG, "FAN DATA RATA IS: 1/4_HZ"); break;
		case EMC2101_RATE_1_2_HZ: ESP_LOGI(TAG, "FAN DATA RATA IS: 1/2_HZ"); break;
		case EMC2101_RATE_1_HZ: ESP_LOGI(TAG, "FAN DATA RATA IS: 1 HZ"); break;
		case EMC2101_RATE_2_HZ: ESP_LOGI(TAG, "FAN DATA RATA IS: 2 HZ"); break;
		case EMC2101_RATE_4_HZ: ESP_LOGI(TAG, "FAN DATA RATA IS: 4 HZ"); break;
		case EMC2101_RATE_8_HZ: ESP_LOGI(TAG, "FAN DATA RATA IS: 8 HZ"); break;
		case EMC2101_RATE_16_HZ: ESP_LOGI(TAG, "FAN DATA RATA IS: 16 HZ"); break;
		case EMC2101_RATE_32_HZ: ESP_LOGI(TAG, "FAN DATA RATA IS: 32 HZ"); break;
		default: ESP_LOGI(TAG, "FAN DATA RATA IS: unknown"); break;
	}

	fan.setDutyCycle(0);

	// -1 so the first pass always writes, whatever the knob reads.
	int16_t shadowPercent = -1;
	uint16_t shadowRPM = 0;
	while (true) {
		uint16_t rpm = fan.getFanRPM();
		if (abs(shadowRPM - rpm) >= 10) {
			shadowRPM = rpm;
			ESP_LOGD(TAG, "Fan RPM: %d", rpm);
		}
		telemetry->fanRPM.set(rpm);
		// Convert the 0-255 knob reading to a percentage, then apply the
		// ventilation floor. Without the floor, a knob left at zero turns every
		// scheduled burst into a relay click and no air at all -- and the
		// ventilation controller would go on believing the chamber had been
		// aired.
		uint8_t currentDuty = telemetry->fanDuty.get();
		uint8_t prcnt = (uint8_t)ceil((currentDuty / 255.0) * 100);
		if ((VentState)attributes->ventState.get() == VentState::Running) {
			uint8_t floorPct = (uint8_t)MIN(100, MAX(0, shared_attributes->ventMinDutyPercent.get()));
			prcnt = MAX(prcnt, floorPct);
		}
		if (shadowPercent != prcnt) {
			shadowPercent = prcnt;
			if (prcnt == 0) {
				fan.setFanMinRPM(100);
				fan.setDutyCycle(0);
			} else if (!fan.setDutyCycle(prcnt)) {
				ESP_LOGE(TAG, "Failed to set fan duty cycle");
			} else {
				ESP_LOGD(TAG, "Fan duty %d%% (knob: %d/255)", prcnt, currentDuty);
			}
		}
		vTaskDelay(pdMS_TO_TICKS(100));
	}
}

static bool publish_json(const char* topic, JsonBuilder* jb, int qos = 0, int retain = 0) {
	if (!jb->finalize()) {
		ESP_LOGE(TAG, "Failed to finalize JSON message. (size=%u)", (unsigned)jb->size());
		// Reset here too. The builder is long-lived and shared across every
		// publish from this task; leaving it in the failed state meant one bad
		// message poisoned all later ones, and the task retried forever against
		// a builder that could never succeed again.
		jb->reset();
		return false;
	}
	size_t size = jb->size();
	int msgId = esp_mqtt_client_publish(mqtt_client, topic, jb->c_str(), size, qos, retain);
	jb->reset();
	if (msgId < 0) {
		ESP_LOGE(TAG, "Failed to publish message, msgId=%d", msgId);
		return false;
	}
	ESP_LOGD(TAG, "Published message, msgId=%d, topic=%s, size=%u", msgId, topic, size);
	return true;
}
static bool publish_telemetry(JsonBuilder* jb) {
	jb->beginObject();
	jb->add("fan_duty_cycle", telemetry->fanDuty.get());
	jb->add("fan_rpm", telemetry->fanRPM.get());
	jb->add("temperature", (double)(telemetry->temperature.get()), 1);
	// One decimal, like temperature. At zero this rounded the primary
	// controlled variable to whole percent -- in exactly the graph you would
	// use to see whether the control loop is behaving.
	jb->add("humidity", (double)(telemetry->humidity.get()), 1);
	// Null rather than a fabricated zero when no probe is fitted or it has
	// stopped answering: a chart with a gap in it is honest, a chart pinned to
	// 0 C looks like a frozen plate.
	if (PlateProbe::isFresh()) {
		jb->add("plate_temperature", (double)(telemetry->plateTemperature.get()), 1);
	} else if (PlateProbe::isConfigured()) {
		jb->addNull("plate_temperature");
	}
	jb->endObject();
	return publish_json("v1/devices/me/telemetry", jb);
}
static void telemetry_task(void* arg) {
	char buff[1024];
	JsonBuilder jb(buff, sizeof(buff));
	while (true) {
		waitForBit(MQTT_CONNECTED_BIT);
		while (true) {
			if (!publish_telemetry(&jb)) {
				break;
			}
			vTaskDelay(pdMS_TO_TICKS(5000));
		}

		vTaskDelay(pdMS_TO_TICKS(60000));
	}
}
static bool publish_attributes(JsonBuilder* jb) {
	jb->beginObject();
	jb->add("fan_running", attributes->fanRunning.get());
	jb->add("fan_enabled", attributes->fanEnabled.get());
	jb->add("vent_state", Ventilation::stateName((VentState)attributes->ventState.get()));
	jb->add("vent_next_seconds", (int32_t)attributes->ventNextSeconds.get());
	jb->add("plate_probe", PlateProbe::description());
	jb->add("fan_stalled", attributes->fanStalled.get());
	jb->add("ram_free", get_ram_free());
	jb->add("ram_total", get_ram_total());
	jb->endObject();
	return publish_json("v1/devices/me/attributes", jb);
}
static void publish_attributes_task(void* arg) {
	char buff[1024];
	JsonBuilder jb(buff, sizeof(buff));
	while (true) {
		waitForBit(MQTT_CONNECTED_BIT);
		publish_attributes(&jb);
		while (true) {
			if (attributes->waitForChange(pdMS_TO_TICKS(10000))) {
				// Clear before publishing, not after: a value changing while
				// the publish is in flight used to have its flag wiped by this
				// call, so the new value waited for the next unrelated change.
				attributes->clearChanged();
				if (!publish_attributes(&jb)) {
					break;
				}

				// To reduce spamming
				vTaskDelay(pdMS_TO_TICKS(5000));
			}
		}
		vTaskDelay(pdMS_TO_TICKS(60000));
	}
}

/**
 * @brief Stop with a repeating explanation when NVS holds no credentials.
 *
 * Deliberately not a reboot loop: rebooting would spam the log with partial
 * boots and, once OTA rollback is enabled, an unprovisioned image would never
 * reach esp_ota_mark_app_valid_cancel_rollback() -- which is the right outcome,
 * but only if the device sits still long enough for the message to be read.
 */
static void halt_unprovisioned(esp_err_t err) {
	while (true) {
		ESP_LOGE(TAG, "Device is not provisioned (%s).", esp_err_to_name(err));
		ESP_LOGE(TAG, "Write wifi_ssid, wifi_psk, tb_uri and tb_token to the NVS");
		ESP_LOGE(TAG, "namespace '%s'. See provisioning/README.md.", DeviceConfig::NVS_NAMESPACE);
		vTaskDelay(pdMS_TO_TICKS(10000));
	}
}

extern "C" void app_main(void) {
	// Initialize NVS
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);
	init_consts();

	// Credentials live in NVS, written once per device at flash time. There is
	// no compile-time fallback on purpose: an unprovisioned device stops here
	// rather than silently trying to join some default network.
	esp_err_t cfg_err = DeviceConfig::load();
	if (cfg_err != ESP_OK) {
		halt_unprovisioned(cfg_err);
	}

	ram_log_snapshot("boot");
	esp_log_level_set("*", shared_attributes->uartLogLevel.get());
	

	// Set pin D2 high
	gpio_config_t io_conf = {};
	io_conf.intr_type = GPIO_INTR_DISABLE;
	io_conf.mode = GPIO_MODE_OUTPUT;
	io_conf.pin_bit_mask = (1ULL << D2);
	io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
	io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
	ESP_ERROR_CHECK(gpio_config(&io_conf));

	bool fanEnabled = shared_attributes->fanEnabled.get();
	ESP_ERROR_CHECK(gpio_set_level(D2, fanEnabled ? 1 : 0));
	attributes->fanEnabled.set(fanEnabled);
	// The I2C bus is not up yet and fan_task has not run, so the EMC2101 cannot
	// be queried here -- the old fan.getFanRPM() call only ever returned 0 after
	// logging a warning. Report not-running until fan_task publishes a reading.
	attributes->fanRunning.set(false);

	
	log_streamer_setup();
	// Started before the client so its supervisor is already waiting on
	// MQTT_CONNECTED_BIT, and its lock exists before any attributes can arrive.
	OtaUpdater::begin();
	wifi_setup();
	TimeSync::begin();
	mqtt_setup();

	// Initialize I2C bus
	if (!I2CBus0.begin()) {
		// Returning from app_main here left WiFi and MQTT running with not a
		// single task created: the device looked alive, reported nothing, and
		// controlled nothing. Without the bus there is no fan and no sensor, so
		// say so as loudly as an unprovisioned device does.
		while (true) {
			ESP_LOGE(TAG, "Failed to initialize the I2C bus. The fan controller and");
			ESP_LOGE(TAG, "climate sensor are both unreachable; not starting any task.");
			vTaskDelay(pdMS_TO_TICKS(10000));
		}
	}
	ESP_LOGI(TAG, "I2C bus initialized successfully");

	BaseType_t result = pdFALSE;
	// Create the Fan task
	result = xTaskCreate(
		fan_task,                // Task function
		"FanTask",               // Task name (for debugging)
		8192,                    // Stack size (in words)
		nullptr,                 // Task input parameter
		tskIDLE_PRIORITY + 1,    // Priority (above idle)
		&fan_task_handle         // Task handle
	);

	if (result != pdPASS) {
		ESP_LOGE(TAG, "Failed to create FanTask");
	} else {
		ESP_LOGI(TAG, "FanTask created successfully.");
	}

	Potentiometer::begin();
	ClimateSensor::begin();
	PlateProbe::begin();

	result = xTaskCreate(
		telemetry_task,              // Task function
		"TelemetryTask",             // Task name (for debugging)
		8192,                        // Stack size (in words)
		nullptr,                     // Task input parameter
		tskIDLE_PRIORITY + 2,        // Priority (above idle)
		&telemetry_task_handle       // Task handle
	);
	if (result != pdPASS) {
		ESP_LOGE(TAG, "Failed to create TelemetryTask");
	} else {
		ESP_LOGI(TAG, "TelemetryTask created successfully.");
	}

	result = xTaskCreate(
		publish_attributes_task,      // Task function
		"PublishAttributesTask",      // Task name (for debugging)
		8192,                         // Stack size (in words)
		nullptr,                      // Task input parameter
		tskIDLE_PRIORITY + 2,         // Priority (above idle)
		nullptr                       // Task handle
	);
	if (result != pdPASS) {
		ESP_LOGE(TAG, "Failed to create PublishAttributesTask");
	} else {
		ESP_LOGI(TAG, "PublishAttributesTask created successfully.");
	}

	Ventilation::begin();

	ram_log_snapshot("setup complete");
}

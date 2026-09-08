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
#include "esp_adc/adc_continuous.h"
#include "soc/soc_caps.h"
#include "esp_random.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_crc.h"
#include "nvs_flash.h"
#include "pins.h"
#include "I2C/I2CBus.h"
#include "I2C/I2CDevice.h"
#include "EMC2101/EMC2101.h"
#include "SHT31/SHT31.h"
#include "JsonBuilder/JsonBuilder.h"
#include "ram_info.h"
#include "consts.h"
#include "log_streamer.h"
#include "mqtt.h"
#include "config/DeviceConfig.h"
#include "ota/OtaUpdater.h"


static const char *TAG = "curing-chamber-fanbox";

static const int WIFI_MAXIMUM_RETRY = 5;
static int s_retry_num = 0;

TaskHandle_t fan_task_handle = nullptr;
TaskHandle_t adc_task_handle = nullptr;
TaskHandle_t climate_sensor_task_handle = nullptr;
TaskHandle_t telemetry_task_handle = nullptr;
TaskHandle_t control_loop_task_handle = nullptr;

static void wifi_event_handler(void* event_handler_arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
		ESP_LOGI(TAG, "Wifi started");
		ESP_ERROR_CHECK(esp_wifi_connect());
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
		ESP_LOGI(TAG, "Disconnected");
		xEventGroupClearBits(network_state_event_group, WIFI_CONNECTED_BIT);
		if (s_retry_num < WIFI_MAXIMUM_RETRY) {
			esp_wifi_connect();
			s_retry_num++;
			ESP_LOGI(TAG, "Connect retry");
		} else {
			xEventGroupSetBits(network_state_event_group, WIFI_FAIL_BIT);
		}
	} else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		s_retry_num = 0;
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

	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	wifi_config_t wifi_config = { };
	// strlcpy, not strcpy: the credentials come from NVS, and sta.ssid /
	// sta.password are fixed 32- and 64-byte fields.
	strlcpy((char*)wifi_config.sta.ssid, DeviceConfig::wifiSsid(), sizeof(wifi_config.sta.ssid));
	strlcpy((char*)wifi_config.sta.password, DeviceConfig::wifiPsk(), sizeof(wifi_config.sta.password));
	wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA3_PSK;
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

	uint8_t shadowDuty = telemetry->fanDuty.get();
	uint16_t shadowRPM = 0;
	while (true) {
		uint16_t rpm = fan.getFanRPM();
		if (abs(shadowRPM - rpm) >= 10) {
			shadowRPM = rpm;
			ESP_LOGD(TAG, "Fan RPM: %d", rpm);
		}
		telemetry->fanRPM.set(rpm);
		uint8_t currentDuty = telemetry->fanDuty.get();
		if (shadowDuty != currentDuty) {
			shadowDuty = currentDuty;
			// Convert 0-255 range to 0-100% range
			uint8_t prcnt = ceil((currentDuty / 255.0) * 100);
			if (prcnt <= 0) {
				fan.setFanMinRPM(100);
				fan.setDutyCycle(0);
			} else if (!fan.setDutyCycle(prcnt)) {
				ESP_LOGE(TAG, "Failed to set fan duty cycle");
			} else {
				ESP_LOGD(TAG, "Potentiometer changed to %d%% (ADC: %d)", prcnt, currentDuty);
			}
		}
		vTaskDelay(pdMS_TO_TICKS(100));
	}
}

static SHT31 climateSensor; // SHT31 temperature and humidity sensor
void climate_sensor_task(void* arg) {
	const int max_retries = 3;
	int retries = 0;
	// Try default address first
	while (!climateSensor.begin(SHT31_I2CADDR_DEFAULT)) {
		ESP_LOGE(TAG, "Failed to initialize SHT31 at address 0x%02X, retrying...", SHT31_I2CADDR_DEFAULT);
		vTaskDelay(pdMS_TO_TICKS(1000));
		if (retries++ >= max_retries) {
			ESP_LOGE(TAG, "Exceeded maximum retries for SHT31 initialization");
			while (true) {
				vTaskDelay(pdMS_TO_TICKS(10000));
			}
		}
	}
	
	ESP_LOGI(TAG, "Climate Sensor initialized successfully");
	
	// Set high repeatability for best accuracy
	if (!climateSensor.setRepeatability(SHT31_REPEATABILITY_HIGH)) {
		ESP_LOGE(TAG, "Failed to set climate sensor repeatability");
	}
	
	float temperature = 0.0f, humidity = 0.0f, lastTemperature = 0.0f, lastHumidity = 0.0f;
	while (true) {
		if (climateSensor.readTempHumidity(&temperature, &humidity)) {
			if (abs(lastTemperature - temperature) >= 0.05f || abs(lastHumidity - humidity) >= 0.5f) {
				ESP_LOGD(TAG, "%.2f°C %.2f%%", temperature, humidity);
				lastTemperature = temperature;
				lastHumidity = humidity;
			}
			telemetry->temperature.set(temperature);
			telemetry->humidity.set(humidity);
		} else {
			ESP_LOGE(TAG, "Failed to read climate sensor data");
		}
		
		vTaskDelay(pdMS_TO_TICKS(1000));
	}
}

static double calculate_attenuated_voltage(double Vin, double attenuation_dB) {
	return Vin * pow(10.0, attenuation_dB / 20.0);
}

static bool IRAM_ATTR s_conv_done_cb(adc_continuous_handle_t handle, const adc_continuous_evt_data_t *edata, void *user_data)
{
	BaseType_t mustYield = pdFALSE;
	vTaskNotifyGiveFromISR(adc_task_handle, &mustYield);
	return (mustYield == pdTRUE);
}

#define ADC_FRAME_SIZE SOC_ADC_DIGI_DATA_BYTES_PER_CONV * 16
#define ADC_MAX_INPUT_VOLTAGE 3.253f // measured voltage at the pot.
#define ADC_MAX_REF_VOLTAGE calculate_attenuated_voltage(1.1f, 12.0f)
static void adc_task(void* arg) {

	adc_continuous_handle_cfg_t adc_config = {
		.max_store_buf_size = 1024,
		.conv_frame_size = ADC_FRAME_SIZE,
		.flags = {
			.flush_pool = true,
		},
	};
	adc_continuous_handle_t handle = NULL;
	ESP_ERROR_CHECK(adc_continuous_new_handle(&adc_config, &handle));

	// Zero-init: these config structs gain fields between IDF releases, and any
	// field left unset would otherwise carry stack garbage into the driver.
	// `format` is deprecated as of IDF 6 -- the driver selects the only output
	// format the target supports (TYPE2 on the ESP32-S3), so it is no longer set.
	adc_digi_pattern_config_t adc_pattern[1] = {};
	adc_pattern[0].atten = ADC_ATTEN_DB_12;
	adc_pattern[0].channel = ADC_CHANNEL_0;
	adc_pattern[0].unit = ADC_UNIT_1;
	adc_pattern[0].bit_width = 12;

	adc_continuous_config_t dig_cfg = {};
	dig_cfg.pattern_num = 1;
	dig_cfg.sample_freq_hz = 20 * 1000;
	dig_cfg.conv_mode = ADC_CONV_SINGLE_UNIT_1;
	dig_cfg.adc_pattern = adc_pattern;
	ESP_ERROR_CHECK(adc_continuous_config(handle, &dig_cfg));

	adc_continuous_evt_cbs_t cbs = {
		.on_conv_done = s_conv_done_cb,
		.on_pool_ovf = NULL,
	};

	ESP_ERROR_CHECK(adc_continuous_register_event_callbacks(handle, &cbs, NULL));
	ESP_ERROR_CHECK(adc_continuous_start(handle));
	ESP_LOGI(TAG, "ADC continuous started");

	uint32_t resN = 0;
	static uint8_t result[ADC_FRAME_SIZE]; // Use static buffer to avoid stack overflow
	memset(result, 0xCC, ADC_FRAME_SIZE);
	while (true) {
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		while (true) {

			esp_err_t ret = adc_continuous_read(handle, result, ADC_FRAME_SIZE, &resN, 100);

			if (ret == ESP_OK) {
				double totval = 0;
				uint32_t frameCount = 0;
				for (int i = 0; i < resN; i += SOC_ADC_DIGI_RESULT_BYTES) {
					adc_digi_output_data_t *p = (adc_digi_output_data_t*)&result[i];
					uint32_t chan_num = p->type2.channel;
					uint32_t data = p->type2.data;
					/* Check the channel number validation, the data is invalid if the channel num exceed the maximum channel */
					if (chan_num != ADC_CHANNEL_0) {
						ESP_LOGW(TAG, "Invalid data [%lu_%lu]", chan_num, data);
					} else {
						totval += data;
						frameCount++;
					}
				}
				if (frameCount == 0) {
					ESP_LOGE(TAG, "No valid data frames received");
					break;
				}
				uint32_t maxVal = 0xFFF;
				uint32_t inputVal = (totval / frameCount);
				uint32_t d = 255 * ((double)inputVal / maxVal);
				uint8_t dutyCycle = (uint8_t)MIN(0xFF, d);
				uint8_t currentDuty = telemetry->fanDuty.get();
				int16_t diff = abs((int16_t)dutyCycle - (int16_t)currentDuty);
				if (abs((int16_t)dutyCycle - (int16_t)currentDuty) > 0) {
					ESP_LOGD(TAG, "New duty: %u (val: %lu, max: %lu, max_vin: %.02f, max_ref: %.02f, frameCount: %lu, totVal: %.02f, diff: %d)", dutyCycle, inputVal, maxVal, ADC_MAX_INPUT_VOLTAGE, ADC_MAX_REF_VOLTAGE, frameCount, totval, diff);
				}
				telemetry->fanDuty.set(dutyCycle);

				vTaskDelay(25);
			} else if (ret == ESP_ERR_TIMEOUT) {
				ESP_LOGE(TAG, "ADC continuous mode driver read timeout");
				break;
			} else if (ret == ESP_ERR_INVALID_STATE) {
				ESP_LOGE(TAG, "ADC continuous mode driver state is invalid");
				break;
			}
		}
	}
}

static bool publish_json(const char* topic, JsonBuilder* jb, int qos = 0, int retain = 0) {
	if (!jb->finalize()) {
		ESP_LOGE(TAG, "Failed to finalize JSON message. (size=%u)", (unsigned)jb->size());
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
	jb->add("humidity", (double)(telemetry->humidity.get()), 0);
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
				if (!publish_attributes(&jb)) {
					break;
				}
				attributes->clearChanged();

				// To reduce spamming
				vTaskDelay(pdMS_TO_TICKS(5000));
			}
		}
		vTaskDelay(pdMS_TO_TICKS(60000));
	}
}

static void control_loop_task(void* arg) {
	while (true) {
		bool fanEnabled = attributes->fanEnabled.get();
		bool newFanEnabled = fanEnabled;
		if (shared_attributes->ctrlLoopEnabled.get()) {
			float humidity = telemetry->humidity.get();
			float humiditySetpoint = shared_attributes->humiditySetpoint.get();
			float humidityOvershootLimit = shared_attributes->humidityOvershootLimit.get();
			float humidityUndershootLimit = shared_attributes->humidityUndershootLimit.get();
			float upperLimit = humiditySetpoint + humidityOvershootLimit;
			float lowerLimit = humiditySetpoint - humidityUndershootLimit;
			if (
				( fanEnabled && humidity >= upperLimit) ||
				(!fanEnabled && humidity <= lowerLimit)) {
				newFanEnabled = !fanEnabled;
				ESP_LOGI(TAG, "Humidity outside thresholds [%.01f%%, %.01f%%] with value %.02f%%, %s fan.", lowerLimit, upperLimit, humidity, newFanEnabled ? "enabling" : "disabling");
			}
		} else {
			newFanEnabled = shared_attributes->fanEnabled.get();
			if (fanEnabled != newFanEnabled) {
				ESP_LOGI(TAG, "Fan changed to %s", newFanEnabled ? "enabled" : "disabled");
			}
		}

		gpio_set_level(D2, newFanEnabled ? 1 : 0);
		attributes->fanEnabled.set(newFanEnabled);
		// fan_task owns the EMC2101; take its published reading rather than
		// issuing a concurrent I2C transaction from this task.
		attributes->fanRunning.set(telemetry->fanRPM.get() > 0);
		vTaskDelay(pdMS_TO_TICKS(5000));
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
	mqtt_setup();

	// Initialize I2C bus
	if (!I2CBus0.begin()) {
		ESP_LOGE(TAG, "Failed to initialize I2C bus");
		return;
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

	// ADC task
	result = xTaskCreate(
		adc_task,                // Task function
		"ADCTask",               // Task name (for debugging)
		8192,                    // Stack size (in words)
		nullptr,                 // Task input parameter
		tskIDLE_PRIORITY + 1,    // Priority (above idle)
		&adc_task_handle
	);
	if (result != pdPASS) {
		ESP_LOGE(TAG, "Failed to create ADCTask");
	} else {
		ESP_LOGI(TAG, "ADCTask created successfully.");
	}

	// Climate sensor task
	result = xTaskCreate(
		climate_sensor_task,         // Task function
		"ClimateSensorTask",         // Task name (for debugging)
		8192,                        // Stack size (in words)
		nullptr,                     // Task input parameter
		tskIDLE_PRIORITY + 1,        // Priority (above idle)
		&climate_sensor_task_handle  // Task handle
	);
	if (result != pdPASS) {
		ESP_LOGE(TAG, "Failed to create ClimateSensorTask");
	} else {
		ESP_LOGI(TAG, "ClimateSensorTask created successfully.");
	}

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

	result = xTaskCreate(
		control_loop_task,           // Task function
		"ControlLoopTask",           // Task name (for debugging)
		8192,                        // Stack size (in words)
		nullptr,                     // Task input parameter
		tskIDLE_PRIORITY + 3,        // Priority (above idle)
		&control_loop_task_handle       // Task handle
	);
	if (result != pdPASS) {
		ESP_LOGE(TAG, "Failed to create ControlLoopTask");
	} else {
		ESP_LOGI(TAG, "ControlLoopTask created successfully.");
	}

	ram_log_snapshot("setup complete");
}

#include "ClimateSensor.h"

#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "SHT31/SHT31.h"
#include "consts.h"

static const char* TAG = "climate";

namespace {

SHT31 sensor;

// Written only by the sampling task, read by the ventilation controller. A
// 32-bit aligned load or store is atomic on this target.
volatile float    s_temperature = 0.0f;
volatile float    s_humidity = 0.0f;
volatile uint32_t s_lastOkMs = 0;
volatile bool     s_everOk = false;

// Thirty consecutive failed reads at one per second. Long enough to ride out a
// transient bus error, short enough that ventilation is not asked for on the
// strength of a number from several minutes ago.
const uint32_t CLIMATE_STALE_MS = 30000;

void climate_sensor_task(void* arg) {
	const int max_retries = 3;
	int retries = 0;
	while (!sensor.begin(SHT31_I2CADDR_DEFAULT)) {
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

	if (!sensor.setRepeatability(SHT31_REPEATABILITY_HIGH)) {
		ESP_LOGE(TAG, "Failed to set climate sensor repeatability");
	}

	float temperature = 0.0f, humidity = 0.0f, lastTemperature = 0.0f, lastHumidity = 0.0f;
	while (true) {
		if (sensor.readTempHumidity(&temperature, &humidity)) {
			if (fabsf(lastTemperature - temperature) >= 0.05f || fabsf(lastHumidity - humidity) >= 0.5f) {
				ESP_LOGD(TAG, "%.2f°C %.2f%%", temperature, humidity);
				lastTemperature = temperature;
				lastHumidity = humidity;
			}
			s_temperature = temperature;
			s_humidity = humidity;
			telemetry->temperature.set(temperature);
			telemetry->humidity.set(humidity);
			s_lastOkMs = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
			s_everOk = true;
		} else {
			ESP_LOGE(TAG, "Failed to read climate sensor data");
		}

		vTaskDelay(pdMS_TO_TICKS(1000));
	}
}

} // namespace

void ClimateSensor::begin(void) {
	BaseType_t result = xTaskCreate(climate_sensor_task, "ClimateSensorTask", 8192,
									nullptr, tskIDLE_PRIORITY + 1, nullptr);
	if (result != pdPASS) {
		ESP_LOGE(TAG, "Failed to create ClimateSensorTask");
	} else {
		ESP_LOGI(TAG, "ClimateSensorTask created successfully.");
	}
}

bool ClimateSensor::isFresh(void) {
	if (!s_everOk) {
		return false;
	}
	uint32_t now = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
	return (uint32_t)(now - s_lastOkMs) <= CLIMATE_STALE_MS;
}

float ClimateSensor::temperature(void) {
	return s_temperature;
}

float ClimateSensor::humidity(void) {
	return s_humidity;
}

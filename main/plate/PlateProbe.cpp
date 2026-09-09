#include "PlateProbe.h"

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "consts.h"

#if CONFIG_PLATE_PROBE_DS18B20
#include "onewire_bus.h"
#include "ds18b20.h"
#endif

static const char* TAG = "plate";

// Kconfig gives the pin as an integer; the description string wants it spelled.
#define PP_STR2(x) #x
#define PP_STR(x) PP_STR2(x)

namespace {

// Written only by the sampling task, read by the control loop. A 32-bit aligned
// load or store is atomic on this target, so these need no lock -- the same
// argument the climate staleness tracking in main.cpp makes.
volatile float    s_temperature = 0.0f;
volatile uint32_t s_lastOkMs = 0;
volatile bool     s_everOk = false;

// Six missed samples at one per five seconds. Long enough to ride out a CRC
// error or a transient bus glitch, short enough that a detached probe stops
// gating ventilation on a number from last week.
const uint32_t PLATE_STALE_MS = 30000;

const uint32_t SAMPLE_PERIOD_MS = 5000;

} // namespace

bool PlateProbe::isConfigured(void) {
#if CONFIG_PLATE_PROBE_DS18B20
	return true;
#else
	return false;
#endif
}

const char* PlateProbe::description(void) {
#if CONFIG_PLATE_PROBE_DS18B20
	return "DS18B20 on GPIO " PP_STR(CONFIG_PLATE_PROBE_GPIO);
#else
	return "none";
#endif
}

bool PlateProbe::isFresh(void) {
	if (!isConfigured() || !s_everOk) {
		return false;
	}
	uint32_t now = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
	return (uint32_t)(now - s_lastOkMs) <= PLATE_STALE_MS;
}

float PlateProbe::temperature(void) {
	return s_temperature;
}

#if CONFIG_PLATE_PROBE_DS18B20

static void plate_probe_task(void* arg) {
	onewire_bus_handle_t bus = nullptr;
	onewire_bus_config_t bus_config = {
		.bus_gpio_num = CONFIG_PLATE_PROBE_GPIO,
	};
	// One DS18B20 answers in nine bytes; the default sizing is generous enough
	// for the scratchpad read plus the ROM command that precedes it.
	onewire_bus_rmt_config_t rmt_config = {
		.max_rx_bytes = 10,
	};

	// Retry rather than give up: a probe that is unplugged at boot and fitted
	// later should start working without a reboot, and a chamber sealed for a
	// month is exactly where that matters.
	while (onewire_new_bus_rmt(&bus_config, &rmt_config, &bus) != ESP_OK) {
		ESP_LOGE(TAG, "Failed to bring up the 1-Wire bus on GPIO %d; retrying",
				 CONFIG_PLATE_PROBE_GPIO);
		vTaskDelay(pdMS_TO_TICKS(10000));
	}

	ds18b20_device_handle_t probe = nullptr;
	ds18b20_config_t probe_config = {};
	bool warned = false;
	while (true) {
		if (probe == nullptr) {
			// from_bus, not the enumerating variant: there is exactly one
			// device on this wire, and skipping enumeration means a probe
			// swapped for another part number keeps working without a rebuild.
			esp_err_t err = ds18b20_new_device_from_bus(bus, &probe_config, &probe);
			if (err != ESP_OK) {
				if (!warned) {
					ESP_LOGW(TAG, "No DS18B20 on GPIO %d (%s). Check the 4.7k pull-up to 3V3.",
							 CONFIG_PLATE_PROBE_GPIO, esp_err_to_name(err));
					warned = true;
				}
				probe = nullptr;
				vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
				continue;
			}
			warned = false;
			ESP_LOGI(TAG, "Plate probe found on GPIO %d", CONFIG_PLATE_PROBE_GPIO);
			// 12-bit: 0.0625 C steps for a 750 ms conversion. The plate moves
			// over minutes, so resolution costs nothing worth having.
			if (ds18b20_set_resolution(probe, DS18B20_RESOLUTION_12B) != ESP_OK) {
				ESP_LOGW(TAG, "Failed to set probe resolution; using its default");
			}
		}

		float t = 0.0f;
		esp_err_t err = ds18b20_trigger_temperature_conversion(probe);
		if (err == ESP_OK) {
			err = ds18b20_get_temperature(probe, &t);
		}
		if (err == ESP_OK) {
			s_temperature = t;
			s_lastOkMs = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
			s_everOk = true;
			telemetry->plateTemperature.set((double)t);
		} else {
			ESP_LOGW(TAG, "Plate read failed: %s", esp_err_to_name(err));
			// A device that has gone away stays a stale handle forever, so drop
			// it and let the next pass re-detect. Staleness handles the control
			// side; this handles the driver side.
			if (err != ESP_ERR_INVALID_CRC) {
				ds18b20_del_device(probe);
				probe = nullptr;
			}
		}

		vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
	}
}

void PlateProbe::begin(void) {
	BaseType_t result = xTaskCreate(plate_probe_task, "PlateProbeTask", 4096,
									nullptr, tskIDLE_PRIORITY + 1, nullptr);
	if (result != pdPASS) {
		ESP_LOGE(TAG, "Failed to create PlateProbeTask");
	} else {
		ESP_LOGI(TAG, "Plate probe: %s", description());
	}
}

#else // no probe configured

void PlateProbe::begin(void) {
	ESP_LOGI(TAG, "No plate probe configured; ventilation will not be gated on "
				  "plate temperature");
}

#endif

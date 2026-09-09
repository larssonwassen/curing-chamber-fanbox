#include "Potentiometer.h"

#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "consts.h"

static const char* TAG = "pot";

namespace {

const adc_unit_t     UNIT = ADC_UNIT_1;
const adc_channel_t  CHANNEL = ADC_CHANNEL_0; // GPIO 1 / A0
const adc_bitwidth_t WIDTH = ADC_BITWIDTH_12;

// 12 dB gets the full 0-3.3 V swing of the pot into range. The reading is used
// as a ratio of full scale, never as a voltage, so the ADC's absolute accuracy
// and its missing calibration curve do not matter here -- which is why the
// previous voltage constants could sit unused for as long as they did.
const adc_atten_t ATTEN = ADC_ATTEN_DB_12;

const int SAMPLES_PER_READ = 16;
const uint32_t PERIOD_MS = 250;

// One duty step is 16 ADC counts, and the last bit or two of a 12-bit reading
// is noise. Without a deadband a knob parked on a boundary dithers between two
// values forever, and every dither is an I2C write to the fan controller.
const int DEADBAND = 2;

void potentiometer_task(void* arg) {
	adc_oneshot_unit_handle_t adc = nullptr;
	adc_oneshot_unit_init_cfg_t unit_cfg = {};
	unit_cfg.unit_id = UNIT;
	unit_cfg.ulp_mode = ADC_ULP_MODE_DISABLE;
	ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &adc));

	adc_oneshot_chan_cfg_t chan_cfg = {};
	chan_cfg.atten = ATTEN;
	chan_cfg.bitwidth = WIDTH;
	ESP_ERROR_CHECK(adc_oneshot_config_channel(adc, CHANNEL, &chan_cfg));

	ESP_LOGI(TAG, "Potentiometer on ADC1 channel %d", (int)CHANNEL);

	const int maxRaw = (1 << 12) - 1;
	int lastDuty = (int)telemetry->fanDuty.get();
	bool everRead = false;

	while (true) {
		int total = 0;
		int taken = 0;
		for (int i = 0; i < SAMPLES_PER_READ; i++) {
			int raw = 0;
			esp_err_t err = adc_oneshot_read(adc, CHANNEL, &raw);
			if (err == ESP_OK) {
				total += raw;
				taken++;
			} else if (i == 0) {
				ESP_LOGW(TAG, "ADC read failed: %s", esp_err_to_name(err));
			}
		}

		if (taken > 0) {
			const int raw = total / taken;
			int duty = (raw * 255) / maxRaw;
			if (duty > 255) {
				duty = 255;
			}
			if (!everRead || abs(duty - lastDuty) >= DEADBAND || duty == 0 || duty == 255) {
				// The endpoints are exempt from the deadband: "fully off" and
				// "fully on" are the two positions a person actually aims for,
				// and being one step short of either is the wrong answer.
				if (duty != lastDuty) {
					ESP_LOGD(TAG, "Potentiometer %d/255 (raw %d)", duty, raw);
					telemetry->fanDuty.set((uint8_t)duty);
					lastDuty = duty;
				}
				everRead = true;
			}
		}

		vTaskDelay(pdMS_TO_TICKS(PERIOD_MS));
	}
}

} // namespace

void Potentiometer::begin(void) {
	BaseType_t result = xTaskCreate(potentiometer_task, "PotentiometerTask", 4096,
									nullptr, tskIDLE_PRIORITY + 1, nullptr);
	if (result != pdPASS) {
		ESP_LOGE(TAG, "Failed to create PotentiometerTask");
	} else {
		ESP_LOGI(TAG, "PotentiometerTask created successfully.");
	}
}

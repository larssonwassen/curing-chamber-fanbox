#include "Humidifier.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"

#include "pins.h"
#include "consts.h"
#include "climate/ClimateSensor.h"
#include "plate/PlateProbe.h"

static const char* TAG = "humid";

namespace {

/// Gate of the 2N7000 that inverts into the high-side FET. Active high: the
/// FET's own gate is held at +12 by its pull-up, so a low or floating GPIO --
/// reset, boot, brownout -- leaves the fan off without this firmware's help.
const gpio_num_t HUMIDIFIER_GPIO = D4;

/// Tachometer from the fan, pulled up to 3V3 on the controller board.
const gpio_num_t HUMIDIFIER_TACH_GPIO = D3;

const uint32_t TICK_MS = 1000;

/// Pulses a PC fan's tach transistor produces per revolution. Near-universal,
/// but worth confirming against a known-good reading the first time a
/// particular fan goes in.
const int TACH_PULSES_PER_REV = 2;

/// How long the fan may be commanded on while reporting zero RPM before that is
/// called a stall. A burst shorter than this reports no RPM through no fault of
/// the fan, so the policy's MIN_ON_MS has to be shorter than this is long.
const uint32_t STALL_GRACE_MS = 3000;

/// Counter ceiling. At 3000 RPM the fan produces 100 pulses a second and the
/// count is cleared every tick, so this is three orders of magnitude of
/// headroom; it exists only because the driver requires a limit.
const int PCNT_HIGH_LIMIT = 10000;

/// Shortest pulse the counter will believe. Tach edges are tens of
/// milliseconds apart, so anything near a microsecond is the compressor
/// switching, not the fan turning.
const uint32_t TACH_GLITCH_NS = 1000;

HumidifierPolicy s_policy;
pcnt_unit_handle_t s_tach_unit = nullptr;

HumidifierSettings read_settings(void) {
	HumidifierSettings cfg;
	cfg.targetRh = (float)shared_attributes->humidifierTargetRh.get();
	cfg.raiseBandRh = (float)shared_attributes->humidifierRaiseBandRh.get();
	cfg.burstSeconds = (float)shared_attributes->humidifierBurstSeconds.get();
	cfg.settleMinutes = (float)shared_attributes->humidifierSettleMinutes.get();
	cfg.maxDutyPercent = (float)shared_attributes->humidifierMaxDutyPercent.get();
	// Shared with ventilation on purpose: it is the same physical question
	// about the same piece of metal.
	cfg.plateGateC = (float)shared_attributes->plateGateTempC.get();
	return cfg;
}

/// Bring up the pulse counter on the tach line.
///
/// Returns false on failure, which is not fatal: the humidifier still runs, it
/// just cannot tell you whether the fan is actually turning.
bool tach_begin(void) {
	pcnt_unit_config_t unit_config = {};
	unit_config.low_limit = -1;
	unit_config.high_limit = PCNT_HIGH_LIMIT;

	esp_err_t err = pcnt_new_unit(&unit_config, &s_tach_unit);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "pcnt_new_unit() failed: %s", esp_err_to_name(err));
		s_tach_unit = nullptr;
		return false;
	}

	pcnt_glitch_filter_config_t filter_config = {};
	filter_config.max_glitch_ns = TACH_GLITCH_NS;
	err = pcnt_unit_set_glitch_filter(s_tach_unit, &filter_config);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "pcnt_unit_set_glitch_filter() failed: %s", esp_err_to_name(err));
		return false;
	}

	pcnt_chan_config_t chan_config = {};
	chan_config.edge_gpio_num = HUMIDIFIER_TACH_GPIO;
	chan_config.level_gpio_num = -1;
	pcnt_channel_handle_t chan = nullptr;
	err = pcnt_new_channel(s_tach_unit, &chan_config, &chan);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "pcnt_new_channel() failed: %s", esp_err_to_name(err));
		return false;
	}

	// Count one edge per pulse and ignore the level input entirely -- this is a
	// frequency measurement, not a quadrature decode.
	ESP_ERROR_CHECK(pcnt_channel_set_edge_action(chan,
		PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD));
	ESP_ERROR_CHECK(pcnt_channel_set_level_action(chan,
		PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_KEEP));

	ESP_ERROR_CHECK(pcnt_unit_enable(s_tach_unit));
	ESP_ERROR_CHECK(pcnt_unit_clear_count(s_tach_unit));
	ESP_ERROR_CHECK(pcnt_unit_start(s_tach_unit));

	ESP_LOGI(TAG, "Humidifier tachometer counting on GPIO %d", (int)HUMIDIFIER_TACH_GPIO);
	return true;
}

/// Read and clear the pulse counter, converting to RPM over `windowMs`.
uint16_t tach_read_rpm(uint32_t windowMs) {
	if (s_tach_unit == nullptr || windowMs == 0) {
		return 0;
	}
	int count = 0;
	esp_err_t err = pcnt_unit_get_count(s_tach_unit, &count);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "pcnt_unit_get_count() failed: %s", esp_err_to_name(err));
		return 0;
	}
	pcnt_unit_clear_count(s_tach_unit);
	if (count <= 0) {
		return 0;
	}
	const double revs = (double)count / (double)TACH_PULSES_PER_REV;
	const double rpm = revs * 60000.0 / (double)windowMs;
	return (uint16_t)(rpm > 65535.0 ? 65535.0 : rpm);
}

void humidifier_task(void* arg) {
	bool lastHumidifyNow = shared_attributes->humidifyNow.get();
	const char* lastReason = nullptr;
	HumidifierState lastState = HumidifierState::Idle;
	uint32_t fanOnSinceMs = 0;
	bool fanWasOn = false;
	bool stallLogged = false;
	uint32_t lastTachMs = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());

	while (true) {
		const uint32_t nowMs = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());

		// Measure first, so the reading describes the interval that just ended
		// rather than the one about to start.
		const uint32_t tachWindowMs = (uint32_t)(nowMs - lastTachMs);
		lastTachMs = nowMs;
		const uint16_t rpm = tach_read_rpm(tachWindowMs);
		telemetry->humidifierRPM.set(rpm);

		const bool ctrlLoopEnabled = shared_attributes->ctrlLoopEnabled.get();
		bool fanOn;

		if (!ctrlLoopEnabled) {
			// Manual mode, matching the exchange fan: the shared attribute is
			// the whole controller. This is the lockout to use when working
			// inside the box -- humidifier_enabled on its own does nothing
			// while the loop is running.
			fanOn = shared_attributes->humidifierEnabled.get();
			if (lastState != HumidifierState::Idle || lastReason == nullptr) {
				ESP_LOGI(TAG, "Automatic humidification disabled; fan follows humidifier_enabled");
				lastReason = "manual";
				lastState = HumidifierState::Idle;
			}
			attributes->humidifierState.set((int32_t)HumidifierState::Idle);
		} else if (!shared_attributes->humidifierEnabled.get()) {
			// Master switch. Distinct from manual mode: the loop is running,
			// the humidifier is simply not part of it. This is the default, so
			// a chamber without the box built yet behaves exactly as it did
			// before this firmware shipped.
			fanOn = false;
			if (lastState != HumidifierState::Idle || lastReason == nullptr) {
				ESP_LOGI(TAG, "Humidifier disabled");
				lastReason = "disabled";
				lastState = HumidifierState::Idle;
			}
			attributes->humidifierState.set((int32_t)HumidifierState::Idle);
		} else {
			HumidifierInputs in;
			in.nowMs = nowMs;
			in.humidityValid = ClimateSensor::isFresh();
			in.humidity = ClimateSensor::humidity();
			// The filter the climate sensor already maintains over the same
			// signal, rather than a second one. "Ready" rather than merely
			// "valid": a freshly seeded average is still the single sample it
			// was seeded from, and seeding at the trough of a compressor cycle
			// looks exactly like a dry chamber.
			in.averageEnabled = shared_attributes->humidityAverageMinutes.get() > 0.0;
			in.humidityAvgValid = ClimateSensor::humidityAverageReady();
			in.humidityAvg = ClimateSensor::humidityAverage();
			in.plateConfigured = PlateProbe::isConfigured();
			in.plateValid = PlateProbe::isFresh();
			in.plateC = PlateProbe::temperature();

			// Edge, not level: ThingsBoard keeps a shared attribute set until
			// somebody clears it, so treating it as a level would humidify
			// forever.
			const bool humidifyNow = shared_attributes->humidifyNow.get();
			in.manualBurstRequest = humidifyNow && !lastHumidifyNow;
			lastHumidifyNow = humidifyNow;

			const HumidifierSettings cfg = read_settings();
			const HumidifierDecision d = s_policy.update(in, cfg);
			fanOn = d.fanOn;

			if (d.reason != lastReason || d.state != lastState) {
				ESP_LOGI(TAG, "%s (%s) humidity=%.1f%% avg=%.1f%% duty=%.1f%%",
						 d.reason,
						 Humidifier::stateName(d.state),
						 in.humidityValid ? (double)in.humidity : 0.0,
						 in.humidityAvgValid ? (double)in.humidityAvg : 0.0,
						 (double)s_policy.dutyPercent());
				lastReason = d.reason;
				lastState = d.state;
			}

			attributes->humidifierState.set((int32_t)d.state);
			telemetry->humidifierDutyPercent.set((double)s_policy.dutyPercent());
		}

		gpio_set_level(HUMIDIFIER_GPIO, fanOn ? 1 : 0);

		// A burst that moves no air still looks like humidification to every
		// other part of this firmware, so the one place that can notice has to
		// say so: the fan is commanded on and the tachometer reads zero. The
		// fan lives above a water tray in a saturated airstream, which is
		// exactly where a sleeve bearing goes to die, so this is the alarm that
		// earns its keep.
		if (fanOn && !fanWasOn) {
			fanOnSinceMs = nowMs;
			stallLogged = false;
			attributes->humidifierStalled.set(false);
		}
		fanWasOn = fanOn;
		if (fanOn && (uint32_t)(nowMs - fanOnSinceMs) >= STALL_GRACE_MS && rpm == 0) {
			if (!stallLogged) {
				ESP_LOGW(TAG, "Humidifier commanded on but reporting 0 RPM. Check the "
							  "fan, the 12 V feed and the tach wire.");
				stallLogged = true;
			}
			attributes->humidifierStalled.set(true);
		}
		attributes->humidifierRunning.set(rpm > 0);

		vTaskDelay(pdMS_TO_TICKS(TICK_MS));
	}
}

} // namespace

const char* Humidifier::stateName(HumidifierState s) {
	switch (s) {
	case HumidifierState::Idle:     return "idle";
	case HumidifierState::Pending:  return "pending";
	case HumidifierState::Running:  return "running";
	case HumidifierState::Settling: return "settling";
	}
	return "unknown";
}

void Humidifier::begin(void) {
	if (!tach_begin()) {
		ESP_LOGW(TAG, "Humidifier tachometer unavailable; RPM will read 0 and "
					  "stall detection is off.");
	}

	BaseType_t result = xTaskCreate(humidifier_task, "HumidifierTask", 4096,
									nullptr, tskIDLE_PRIORITY + 3, nullptr);
	if (result != pdPASS) {
		ESP_LOGE(TAG, "Failed to create HumidifierTask");
	} else {
		ESP_LOGI(TAG, "HumidifierTask created successfully.");
	}
}

#include "Ventilation.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "pins.h"
#include "consts.h"
#include "climate/ClimateSensor.h"
#include "plate/PlateProbe.h"
#include "net/TimeSync.h"

static const char* TAG = "vent";

namespace {

// The relay that switches the fan. Speed is set separately, by the
// potentiometer through the EMC2101; this line only decides on or off.
const gpio_num_t FAN_GPIO = D2;

const uint32_t TICK_MS = 1000;

// How long the fan may be commanded on while reporting zero RPM before that is
// called a stall. Long enough for spin-up on a cold bearing.
const uint32_t STALL_GRACE_MS = 5000;

VentilationPolicy s_policy;

VentilationSettings read_settings(void) {
	VentilationSettings cfg;
	cfg.intervalHours = (float)shared_attributes->ventIntervalHours.get();
	cfg.firstHourLocal = (int)shared_attributes->ventFirstHourLocal.get();
	cfg.burstSeconds = (float)shared_attributes->ventBurstSeconds.get();
	cfg.settleMinutes = (float)shared_attributes->ventSettleMinutes.get();
	cfg.plateGateC = (float)shared_attributes->plateGateTempC.get();
	cfg.maxDeferMinutes = (float)shared_attributes->ventMaxDeferMinutes.get();
	cfg.minSecondsPerDay = (float)shared_attributes->ventMinSecondsPerDay.get();
	cfg.humiditySetpoint = (float)shared_attributes->humiditySetpoint.get();
	cfg.humidityUndershoot = (float)shared_attributes->humidityUndershootLimit.get();
	cfg.humidityAverageMinutes = (float)shared_attributes->humidityAverageMinutes.get();
	return cfg;
}

void ventilation_task(void* arg) {
	bool lastVentNow = shared_attributes->ventNow.get();
	const char* lastReason = nullptr;
	VentState lastState = VentState::Idle;
	uint32_t fanOnSinceMs = 0;
	bool fanWasOn = false;
	bool stallLogged = false;

	while (true) {
		const bool ctrlLoopEnabled = shared_attributes->ctrlLoopEnabled.get();
		bool fanOn;

		if (!ctrlLoopEnabled) {
			// Manual mode: the shared attribute is the whole controller. The
			// policy is left untouched rather than reset -- it derives its
			// timing from timestamps, so switching automatic back on resumes
			// the schedule where it stands instead of restarting it.
			fanOn = shared_attributes->fanEnabled.get();
			if (lastState != VentState::Idle || lastReason == nullptr) {
				ESP_LOGI(TAG, "Automatic ventilation disabled; fan follows fan_enabled");
				lastReason = "manual";
				lastState = VentState::Idle;
			}
			attributes->ventState.set((int32_t)VentState::Idle);
			attributes->ventNextSeconds.set(-1);
		} else {
			VentilationInputs in;
			in.nowMs = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
			in.haveClock = TimeSync::isSynced();
			in.localEpoch = TimeSync::localEpoch();
			in.humidityValid = ClimateSensor::isFresh();
			in.humidity = ClimateSensor::humidity();
			in.plateConfigured = PlateProbe::isConfigured();
			in.plateValid = PlateProbe::isFresh();
			in.plateC = PlateProbe::temperature();

			// Edge, not level: ThingsBoard keeps a shared attribute set until
			// somebody clears it, so treating it as a level would ventilate
			// forever. Rising edge only.
			const bool ventNow = shared_attributes->ventNow.get();
			in.manualBurstRequest = ventNow && !lastVentNow;
			lastVentNow = ventNow;

			const VentilationSettings cfg = read_settings();
			const VentilationDecision d = s_policy.update(in, cfg);
			fanOn = d.fanOn;

			if (d.reason != lastReason || d.state != lastState) {
				ESP_LOGI(TAG, "%s (%s) humidity=%.1f%% plate=%s",
						 d.reason,
						 Ventilation::stateName(d.state),
						 in.humidityValid ? (double)in.humidity : 0.0,
						 in.plateConfigured
							 ? (in.plateValid ? "reading" : "stale")
							 : "absent");
				lastReason = d.reason;
				lastState = d.state;
			}

			// Negative stands for "no reading yet", and is published as null.
			telemetry->humidityAverage.set(
				s_policy.humidityAverageValid() ? (double)s_policy.humidityAverage() : -1.0);

			attributes->ventState.set((int32_t)d.state);
			const uint32_t nextMs = s_policy.msUntilScheduled(in, cfg);
			attributes->ventNextSeconds.set(nextMs == 0 ? -1 : (int32_t)(nextMs / 1000));
		}

		gpio_set_level(FAN_GPIO, fanOn ? 1 : 0);
		attributes->fanEnabled.set(fanOn);

		// A burst that moves no air still counts as ventilation everywhere else
		// in this firmware, so the one place that can notice has to say so: the
		// fan is commanded on and the tachometer reads zero.
		const uint32_t nowMs = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
		if (fanOn && !fanWasOn) {
			fanOnSinceMs = nowMs;
			stallLogged = false;
			attributes->fanStalled.set(false);
		}
		fanWasOn = fanOn;
		if (fanOn && (uint32_t)(nowMs - fanOnSinceMs) >= STALL_GRACE_MS &&
			telemetry->fanRPM.get() == 0) {
			if (!stallLogged) {
				ESP_LOGW(TAG, "Fan commanded on but reporting 0 RPM. Check the "
							  "speed knob (a duty of zero looks exactly like this) "
							  "and the fan supply.");
				stallLogged = true;
			}
			attributes->fanStalled.set(true);
		}
		// fan_task owns the EMC2101; take its published reading rather than
		// issuing a concurrent I2C transaction from this task.
		attributes->fanRunning.set(telemetry->fanRPM.get() > 0);

		vTaskDelay(pdMS_TO_TICKS(TICK_MS));
	}
}

} // namespace

const char* Ventilation::stateName(VentState s) {
	switch (s) {
	case VentState::Idle:     return "idle";
	case VentState::Pending:  return "pending";
	case VentState::Running:  return "running";
	case VentState::Settling: return "settling";
	}
	return "unknown";
}

void Ventilation::begin(void) {
	BaseType_t result = xTaskCreate(ventilation_task, "VentilationTask", 8192,
									nullptr, tskIDLE_PRIORITY + 3, nullptr);
	if (result != pdPASS) {
		ESP_LOGE(TAG, "Failed to create VentilationTask");
	} else {
		ESP_LOGI(TAG, "VentilationTask created successfully.");
	}
}

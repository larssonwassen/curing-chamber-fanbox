// Host tests for the humidifier state machine, built with ASan/UBSan.
//
// Unlike the exchange fan, this actuator has real authority over chamber
// humidity, which makes its failure modes expensive in both directions: too
// little and the meat case-hardens, too much and the chamber sits near
// saturation with mould on the surface and frost on the plate. These tests
// cover the bounds that keep it between those -- bursts end, the raise and hold
// thresholds are genuinely different, a cold plate vetoes and is never
// overridden, and the duty cap is enforced over a rolling hour.

#include "humidifier/HumidifierPolicy.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(bool ok, const char* what) {
	printf("%-62s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

namespace {

const uint32_t SEC = 1000;
const uint32_t MIN = 60 * SEC;

HumidifierSettings defaults(void) {
	HumidifierSettings cfg;
	cfg.targetRh = 78.0f;
	cfg.raiseBandRh = 3.0f;
	cfg.burstSeconds = 120.0f;
	cfg.settleMinutes = 10.0f;
	cfg.plateGateC = 2.0f;
	cfg.maxDutyPercent = 25.0f;
	return cfg;
}

/// A chamber sitting in band, with no plate probe fitted.
HumidifierInputs nominal(uint32_t nowMs) {
	HumidifierInputs in;
	in.nowMs = nowMs;
	in.humidityValid = true;
	in.humidity = 78.0f;
	in.averageEnabled = true;
	in.humidityAvgValid = true;
	in.humidityAvg = 78.0f;
	return in;
}

/// A chamber the policy should want to humidify: the average is below the
/// raise band and the raw reading is below the target.
HumidifierInputs dry(uint32_t nowMs) {
	HumidifierInputs in = nominal(nowMs);
	in.humidity = 74.0f;
	in.humidityAvg = 74.0f;
	return in;
}

/// Run the policy forward, sampling once a second like the real task, and
/// report how many seconds the fan was on.
uint32_t runFor(HumidifierPolicy& p, const HumidifierSettings& cfg,
				HumidifierInputs in, uint32_t durationMs,
				uint32_t* endMs = nullptr) {
	uint32_t fanSeconds = 0;
	const uint32_t start = in.nowMs;
	while ((uint32_t)(in.nowMs - start) < durationMs) {
		HumidifierDecision d = p.update(in, cfg);
		if (d.fanOn) {
			fanSeconds++;
		}
		in.nowMs += SEC;
		in.manualBurstRequest = false;
	}
	if (endMs != nullptr) {
		*endMs = in.nowMs;
	}
	return fanSeconds;
}

void test_idle_in_band(void) {
	HumidifierPolicy p;
	const HumidifierSettings cfg = defaults();
	const uint32_t on = runFor(p, cfg, nominal(0), 30 * MIN);
	check(on == 0, "a chamber in band never runs the humidifier");
}

void test_dry_raises_a_bounded_burst(void) {
	HumidifierPolicy p;
	const HumidifierSettings cfg = defaults();
	// Raw held below the target so only burstSeconds can end this.
	const uint32_t on = runFor(p, cfg, dry(0), 5 * MIN);
	check(on >= 118 && on <= 122, "a dry chamber gets one burst of about burstSeconds");
}

void test_burst_stops_at_the_target_not_the_band(void) {
	HumidifierPolicy p;
	const HumidifierSettings cfg = defaults();
	HumidifierInputs in = dry(0);

	// Ten seconds in, the raw reading crosses back above the raise threshold
	// but is still short of the target. The burst must keep going: raising at
	// 75 and stopping at 75 is a fan cycling on its own hysteresis.
	runFor(p, cfg, in, 10 * SEC, &in.nowMs);
	in.humidity = 76.0f;
	HumidifierDecision d = p.update(in, cfg);
	check(d.fanOn, "a burst keeps running above the raise band but below the target");

	in.nowMs += SEC;
	in.humidity = 78.0f;
	d = p.update(in, cfg);
	check(!d.fanOn, "a burst stops the moment the raw reading reaches the target");
	check(d.state == HumidifierState::Settling, "and goes to settling, not straight to idle");
}

void test_hold_reads_raw_not_the_average(void) {
	HumidifierPolicy p;
	const HumidifierSettings cfg = defaults();
	HumidifierInputs in = dry(0);
	runFor(p, cfg, in, 10 * SEC, &in.nowMs);

	// The burst has done its job -- the raw reading is at the target -- while
	// the average is still where it was, because a 30-minute filter lags a
	// two-minute step by design. Holding on the average would run every burst
	// several time constants past the point it was needed.
	in.humidity = 79.0f;
	in.humidityAvg = 74.0f;
	const HumidifierDecision d = p.update(in, cfg);
	check(!d.fanOn, "the hold condition reads the raw value, not the lagging average");
}

void test_raise_reads_the_average_not_raw(void) {
	HumidifierPolicy p;
	const HumidifierSettings cfg = defaults();
	HumidifierInputs in = nominal(0);
	// Mid-compressor-cycle trough: raw dives, the chamber's actual water
	// content has not moved. Nothing should happen.
	in.humidity = 60.0f;
	in.humidityAvg = 78.0f;
	const uint32_t on = runFor(p, cfg, in, 5 * MIN);
	check(on == 0, "a raw trough with a healthy average raises nothing");
}

void test_average_not_ready_raises_nothing(void) {
	HumidifierPolicy p;
	const HumidifierSettings cfg = defaults();
	HumidifierInputs in = dry(0);
	in.humidityAvgValid = false;
	const uint32_t on = runFor(p, cfg, in, 5 * MIN);
	check(on == 0, "a filter that has not settled yet raises nothing");
}

void test_average_disabled_falls_back_to_raw(void) {
	HumidifierPolicy p;
	const HumidifierSettings cfg = defaults();
	HumidifierInputs in = dry(0);
	in.averageEnabled = false;
	in.humidityAvgValid = false;
	const uint32_t on = runFor(p, cfg, in, 5 * MIN);
	check(on > 0, "with the filter switched off the raw reading raises a burst");
}

void test_settle_spaces_bursts(void) {
	HumidifierPolicy p;
	HumidifierSettings cfg = defaults();
	cfg.maxDutyPercent = 0.0f; // Isolate the settle timer from the duty cap.

	// 30 minutes of a chamber that never gets wetter. With a 120 s burst and a
	// 10 minute settle that is a burst roughly every 12 minutes: three of them.
	const uint32_t on = runFor(p, cfg, dry(0), 30 * MIN);
	check(on >= 230 && on <= 370, "settle spaces repeated bursts instead of stacking them");
}

void test_cold_plate_holds_and_never_forces(void) {
	HumidifierPolicy p;
	const HumidifierSettings cfg = defaults();
	HumidifierInputs in = dry(0);
	in.plateConfigured = true;
	in.plateValid = true;
	in.plateC = -3.0f;

	uint32_t endMs = 0;
	// Twelve hours. Ventilation would have forced a burst through after six;
	// this must not, because moist air onto sub-zero metal is the exact
	// mechanism that armoured the evaporator.
	const uint32_t on = runFor(p, cfg, in, 12 * 60 * MIN, &endMs);
	check(on == 0, "a cold plate holds humidification off indefinitely");
	check(p.state() == HumidifierState::Pending, "and the state says so, rather than idling");

	// Plate comes back up; the burst that was owed runs.
	in.nowMs = endMs;
	in.plateC = 5.0f;
	const HumidifierDecision d = p.update(in, cfg);
	check(d.fanOn, "and the burst runs once the plate warms up");
}

void test_stale_plate_probe_is_treated_as_cold(void) {
	HumidifierPolicy p;
	const HumidifierSettings cfg = defaults();
	HumidifierInputs in = dry(0);
	in.plateConfigured = true;
	in.plateValid = false;
	const uint32_t on = runFor(p, cfg, in, 30 * MIN);
	check(on == 0, "a probe that has stopped reading is treated as cold");
}

void test_pending_request_is_withdrawn_when_no_longer_dry(void) {
	HumidifierPolicy p;
	const HumidifierSettings cfg = defaults();
	HumidifierInputs in = dry(0);
	in.plateConfigured = true;
	in.plateValid = true;
	in.plateC = -3.0f;
	runFor(p, cfg, in, 5 * MIN, &in.nowMs);
	check(p.state() == HumidifierState::Pending, "a cold plate parks the request in pending");

	// Frost coming back off the warming plate is what makes the plate warm
	// enough to proceed -- and it is also what makes the chamber wet. Firing
	// the burst raised five minutes ago would overshoot.
	in.humidity = 82.0f;
	const HumidifierDecision d = p.update(in, cfg);
	check(!d.fanOn, "a pending request is withdrawn once the chamber is wet again");
	check(p.state() == HumidifierState::Idle, "and the state returns to idle");
}

void test_duty_cap_stops_asking(void) {
	HumidifierPolicy p;
	HumidifierSettings cfg = defaults();
	cfg.settleMinutes = 0.0f;  // No quiet time, so only the cap can limit it.
	cfg.burstSeconds = 60.0f;

	// One hour of a chamber that never gets wetter, with settle disabled. The
	// only thing standing between this and a fan running for the whole hour is
	// the duty cap.
	const uint32_t on = runFor(p, cfg, dry(0), 60 * MIN);
	check(on > 0, "the cap does not block humidification outright");
	check(on <= 16 * 60, "the duty cap holds run time near 25% of a rolling hour");
	check(p.dutyPercent() <= 26.0f, "and the reported duty agrees");
}

void test_duty_cap_can_be_disabled(void) {
	HumidifierPolicy p;
	HumidifierSettings cfg = defaults();
	cfg.settleMinutes = 0.0f;
	cfg.maxDutyPercent = 0.0f;
	const uint32_t on = runFor(p, cfg, dry(0), 30 * MIN);
	check(on >= 29 * 60, "a zero cap means no cap");
}

void test_manual_burst_overrides_everything(void) {
	HumidifierPolicy p;
	const HumidifierSettings cfg = defaults();
	HumidifierInputs in = nominal(0);
	// Wet chamber, frozen plate, no request of any kind. A person asked anyway.
	in.humidity = 85.0f;
	in.humidityAvg = 85.0f;
	in.plateConfigured = true;
	in.plateValid = true;
	in.plateC = -8.0f;
	in.manualBurstRequest = true;

	const HumidifierDecision d = p.update(in, cfg);
	check(d.fanOn, "a manual request runs regardless of plate and humidity");
	check(d.trigger == HumidifierTrigger::Manual, "and is attributed to the manual trigger");

	in.manualBurstRequest = false;
	const uint32_t on = runFor(p, cfg, in, 10 * MIN);
	check(on >= 115 && on <= 125, "a manual burst is still bounded by burstSeconds");
}

void test_minimum_on_time(void) {
	HumidifierPolicy p;
	const HumidifierSettings cfg = defaults();
	HumidifierInputs in = dry(0);
	HumidifierDecision d = p.update(in, cfg);
	check(d.fanOn, "the burst starts");

	// The chamber reports itself wet one second later. Believing that
	// instantly would cycle the FET at the loop rate whenever a reading sat on
	// the threshold.
	in.nowMs += SEC;
	in.humidity = 90.0f;
	d = p.update(in, cfg);
	check(d.fanOn, "and cannot be cut inside the minimum on-time");
}

void test_tick_wraparound(void) {
	HumidifierPolicy p;
	const HumidifierSettings cfg = defaults();
	// Start just before the 32-bit millisecond counter wraps, which the real
	// device reaches after 49 days of uptime.
	const uint32_t on = runFor(p, cfg, dry(0xFFFFF000u), 30 * MIN);
	check(on > 0, "the policy still works across the tick counter wrap");
}

} // namespace

int main(void) {
	test_idle_in_band();
	test_dry_raises_a_bounded_burst();
	test_burst_stops_at_the_target_not_the_band();
	test_hold_reads_raw_not_the_average();
	test_raise_reads_the_average_not_raw();
	test_average_not_ready_raises_nothing();
	test_average_disabled_falls_back_to_raw();
	test_settle_spaces_bursts();
	test_cold_plate_holds_and_never_forces();
	test_stale_plate_probe_is_treated_as_cold();
	test_pending_request_is_withdrawn_when_no_longer_dry();
	test_duty_cap_stops_asking();
	test_duty_cap_can_be_disabled();
	test_manual_burst_overrides_everything();
	test_minimum_on_time();
	test_tick_wraparound();

	printf("\n%s\n", failures == 0 ? "all ok" : "FAILURES");
	return failures == 0 ? 0 : 1;
}

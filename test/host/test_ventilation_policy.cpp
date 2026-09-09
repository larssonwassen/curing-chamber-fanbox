// Host tests for the ventilation state machine, built with ASan/UBSan.
//
// The policy decides when to pull room air into a chamber whose only
// dehumidifier is a sub-zero plate. Getting it wrong is not a formatting bug --
// the previous controller could hold the fan on for hours, and the moisture it
// carried in froze the evaporator solid. These tests cover the bounds that stop
// that: bursts end, the plate can veto, humidity can only ever ask for more air
// and never for less, and every counter resets on a day boundary.

#include "control/VentilationPolicy.h"

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
const uint32_t HOUR = 60 * MIN;

VentilationSettings defaults(void) {
	VentilationSettings cfg;
	cfg.intervalHours = 12.0f;
	cfg.firstHourLocal = 6;
	cfg.burstSeconds = 90.0f;
	cfg.settleMinutes = 20.0f;
	cfg.plateGateC = 2.0f;
	cfg.maxDeferMinutes = 360.0f;
	cfg.maxDryBurstsPerDay = 6;
	cfg.humiditySetpoint = 75.0f;
	cfg.humidityUndershoot = 5.0f;
	cfg.minSecondsPerDay = 180.0f;
	return cfg;
}

/// A chamber that is comfortably in band, with no probe fitted.
VentilationInputs nominal(uint32_t nowMs) {
	VentilationInputs in;
	in.nowMs = nowMs;
	in.humidityValid = true;
	in.humidity = 78.0f;
	return in;
}

/// Run the policy forward, sampling once a second like the real task, and
/// report how many seconds the fan was on.
uint32_t runFor(VentilationPolicy& p, const VentilationSettings& cfg,
				VentilationInputs in, uint32_t durationMs,
				uint32_t* endMs = nullptr) {
	uint32_t fanSeconds = 0;
	const uint32_t start = in.nowMs;
	while ((uint32_t)(in.nowMs - start) < durationMs) {
		VentilationDecision d = p.update(in, cfg);
		if (d.fanOn) {
			fanSeconds++;
		}
		in.nowMs += SEC;
		if (in.haveClock) {
			in.localEpoch += 1;
		}
		in.manualBurstRequest = false;
	}
	if (endMs != nullptr) {
		*endMs = in.nowMs;
	}
	return fanSeconds;
}

} // namespace

int main(void) {
	const VentilationSettings cfg = defaults();

	// ---- Scheduling without a clock -------------------------------------
	{
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		uint32_t fan = runFor(p, cfg, in, 11 * HOUR);
		check(fan == 0, "no clock: nothing runs before the first interval");
	}

	{
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		uint32_t fan = runFor(p, cfg, in, 13 * HOUR);
		// One burst of 90 s, sampled once a second.
		check(fan >= 89 && fan <= 91, "no clock: one burst after the interval elapses");
	}

	{
		// The failure the old controller had: the fan never turning off.
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		uint32_t fan = runFor(p, cfg, in, 25 * HOUR);
		check(fan <= 200, "no clock: a day of running stays near the schedule, not hours of fan");
	}

	// ---- The plate gate --------------------------------------------------
	{
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		in.plateConfigured = true;
		in.plateValid = true;
		in.plateC = -8.0f; // compressor running, plate well below freezing
		uint32_t fan = runFor(p, cfg, in, 13 * HOUR);
		check(fan == 0, "cold plate defers the burst rather than frosting it");
		check(p.state() == VentState::Pending, "...and the burst stays owed");
	}

	{
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		in.plateConfigured = true;
		in.plateValid = true;
		in.plateC = -8.0f;
		uint32_t after = 0;
		runFor(p, cfg, in, 13 * HOUR, &after);
		in.nowMs = after;
		in.plateC = 6.0f; // compressor off, plate has warmed and drained
		uint32_t fan = runFor(p, cfg, in, 5 * MIN);
		check(fan >= 89 && fan <= 91, "the deferred burst runs once the plate warms");
	}

	{
		// A probe that stops answering must not block ventilation forever.
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		in.plateConfigured = true;
		in.plateValid = false;
		uint32_t fan = runFor(p, cfg, in, 12 * HOUR + 30 * MIN);
		check(fan == 0, "a stale probe is treated as cold");
		uint32_t fan2 = runFor(p, cfg, in, 25 * HOUR);
		check(fan2 > 0, "...but the defer timeout eventually ventilates anyway");
	}

	{
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		in.plateConfigured = false; // no probe fitted at all
		uint32_t fan = runFor(p, cfg, in, 13 * HOUR);
		check(fan >= 89 && fan <= 91, "with no probe fitted the schedule is ungated");
	}

	// ---- Humidity is a one-way vote --------------------------------------
	{
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		in.humidity = 60.0f; // well under setpoint - undershoot
		uint32_t fan = runFor(p, cfg, in, 6 * HOUR);
		check(fan > 90, "a dry chamber gets extra bursts before the schedule is due");
	}

	{
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		in.humidity = 60.0f;
		uint32_t fan = runFor(p, cfg, in, 20 * HOUR);
		// Six dry bursts plus one scheduled burst, at 90 s each.
		check(fan <= 7 * 91, "dry bursts are capped for the day");
	}

	{
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		in.humidity = 95.0f; // soaking wet
		uint32_t fan = runFor(p, cfg, in, 11 * HOUR);
		check(fan == 0, "high humidity never runs the fan: it cannot dry the chamber");
	}

	{
		// A dead sensor used to hold the fan off entirely. Bursts are bounded
		// now, so the schedule carries on without it.
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		in.humidityValid = false;
		uint32_t fan = runFor(p, cfg, in, 13 * HOUR);
		check(fan >= 89 && fan <= 91, "a stale humidity reading does not stop scheduled air");
	}

	// ---- Manual burst ----------------------------------------------------
	{
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		in.plateConfigured = true;
		in.plateValid = true;
		in.plateC = -15.0f;
		in.manualBurstRequest = true;
		uint32_t fan = runFor(p, cfg, in, 5 * MIN);
		check(fan >= 89 && fan <= 91, "a manual request ventilates even with a cold plate");
	}

	// ---- Wall-clock scheduling -------------------------------------------
	{
		// 2026-01-01 17:30 local. Nothing should run until 18:00.
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		in.haveClock = true;
		in.localEpoch = 1767200000 - (1767200000 % 86400) + 17 * 3600 + 30 * 60;
		uint32_t fan = runFor(p, cfg, in, 25 * MIN);
		check(fan == 0, "clock: the slot already in progress at boot is not re-run");

		uint32_t after = 0;
		runFor(p, cfg, in, 25 * MIN, &after);
		VentilationInputs in2 = in;
		in2.nowMs = after;
		in2.localEpoch = in.localEpoch + 25 * 60;
		uint32_t fan2 = runFor(p, cfg, in2, 10 * MIN);
		check(fan2 >= 89 && fan2 <= 91, "clock: the 18:00 slot runs one burst");
	}

	// ---- Daily minimum ---------------------------------------------------
	{
		// The default minimum is exactly what the schedule delivers, so it must
		// not add anything on its own.
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		uint32_t fan = runFor(p, cfg, in, 24 * HOUR);
		check(fan <= 200, "default daily minimum adds no bursts of its own");
	}

	{
		VentilationSettings c = defaults();
		c.minSecondsPerDay = 900.0f; // 15 minutes a day
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		uint32_t fan = runFor(p, c, in, 24 * HOUR);
		check(fan >= 700, "a raised daily minimum is topped up with make-up bursts");
		check(fan <= 1100, "...and not overshot");
	}

	{
		// Top-ups must be spread, not saved for the end of the day.
		VentilationSettings c = defaults();
		c.minSecondsPerDay = 900.0f;
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		uint32_t fan = runFor(p, c, in, 6 * HOUR);
		check(fan >= 90, "make-up bursts start early in the day, not at midnight");
	}

	{
		// Humidity-driven air counts towards the minimum: a chamber that is
		// already getting plenty of air should not get more on top.
		VentilationSettings c = defaults();
		c.minSecondsPerDay = 900.0f;
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		in.humidity = 60.0f;
		uint32_t fan = runFor(p, c, in, 24 * HOUR);
		check(fan <= 1100, "dry-driven bursts count towards the daily minimum");
	}

	// ---- Timer wrap -------------------------------------------------------
	{
		// The tick counter wraps every ~49 days. A chamber runs for months.
		VentilationPolicy p;
		VentilationInputs in = nominal(0xFFFFFFFFu - 6 * (uint32_t)HOUR);
		uint32_t fan = runFor(p, cfg, in, 13 * HOUR);
		check(fan >= 89 && fan <= 91, "scheduling survives the 32-bit millisecond wrap");
	}

	printf("%s\n", failures == 0 ? "all ok" : "FAILURES");
	return failures == 0 ? 0 : 1;
}

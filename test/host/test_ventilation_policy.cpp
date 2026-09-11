// Host tests for the ventilation state machine, built with ASan/UBSan.
//
// The policy decides when to pull room air into a chamber whose only
// dehumidifier is a sub-zero plate. Getting it wrong is not a formatting bug --
// the original controller could hold the fan on for hours, and the moisture it
// carried in froze the evaporator solid. These tests cover the bounds that stop
// that: bursts end, the plate can veto, a veto cannot last forever, and every
// counter resets on a day boundary.
//
// Humidity is deliberately absent from all of it. The exchange fan used to take
// a vote from the humidity reading, back when it was the only actuator that
// could raise humidity at all; the humidifier does that job now, and the
// exchange fan runs on the clock and the plate gate alone. There is no humidity
// field left in VentilationInputs to test with.

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
	cfg.settleMinutes = 1.5f;
	cfg.plateGateC = 2.0f;
	cfg.maxDeferMinutes = 360.0f;
	cfg.minSecondsPerDay = 180.0f;
	return cfg;
}

/// A chamber with no probe fitted and nothing asked for.
VentilationInputs nominal(uint32_t nowMs) {
	VentilationInputs in;
	in.nowMs = nowMs;
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
	// A three-minute schedule, for the tests that are about what a burst does
	// once it is running rather than about when one is raised. Waiting twelve
	// simulated hours for each of those would say nothing extra.
	VentilationSettings fastCfg = defaults();
	fastCfg.intervalHours = 0.05f; // 180 s

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
		// The failure the original controller had: the fan never turning off.
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
		// Stale air is a real problem, so a burst owed since this morning goes
		// ahead even if the plate never warms. This is the one thing the
		// humidifier deliberately does not do: it can afford to wait forever,
		// and air exchange cannot.
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		in.plateConfigured = true;
		in.plateValid = true;
		in.plateC = -9.0f;
		uint32_t fan = runFor(p, cfg, in, 20 * HOUR);
		check(fan > 0, "scheduled air forces through after the defer timeout");
	}

	{
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		in.plateConfigured = false; // no probe fitted at all
		uint32_t fan = runFor(p, cfg, in, 13 * HOUR);
		check(fan >= 89 && fan <= 91, "with no probe fitted the schedule is ungated");
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

	{
		// Observed 2026-09-09, when humidity could still raise bursts of its
		// own: one finished ten seconds into the 18:00 slot and the scheduled
		// burst ran ninety seconds later, taking the chamber from 63% to 87% RH
		// between them. The rule that fixed it outlives the trigger that
		// exposed it -- whatever asked for a burst, the slot it ends in has had
		// its air. Exercised here with a manual burst at 17:58:30.
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		in.haveClock = true;
		const int64_t base = 1767200000 - (1767200000 % 86400) + 17 * 3600 + 58 * 60 + 30;
		in.localEpoch = base;
		in.manualBurstRequest = true;

		uint32_t fan = 0;
		for (uint32_t t = 0; t < 20 * MIN; t += SEC) {
			in.nowMs = t;
			in.localEpoch = base + t / SEC;
			if (p.update(in, cfg).fanOn) {
				fan++;
			}
			in.manualBurstRequest = false;
		}
		// One burst, not two. Before this, the 18:00 slot could not tell that
		// the chamber had been ventilated ten seconds into it, and ran a second
		// full burst on top.
		check(fan >= 89 && fan <= 91, "a burst serves the schedule slot it ends in");
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
		// Every burst counts towards the minimum, whatever raised it. A
		// schedule that already delivers more than the floor must not have
		// make-up bursts stacked on top of it: hourly bursts are 2160 s a day
		// against a 900 s floor.
		VentilationSettings c = defaults();
		c.intervalHours = 1.0f;
		c.minSecondsPerDay = 900.0f;
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		uint32_t fan = runFor(p, c, in, 24 * HOUR);
		check(fan <= 24 * 91 + 91, "scheduled bursts count towards the daily minimum");
	}

	// ---- Timer wrap -------------------------------------------------------
	{
		// The tick counter wraps every ~49 days. A chamber runs for months.
		VentilationPolicy p;
		VentilationInputs in = nominal(0xFFFFFFFFu - 6 * (uint32_t)HOUR);
		uint32_t fan = runFor(p, cfg, in, 13 * HOUR);
		check(fan >= 89 && fan <= 91, "scheduling survives the 32-bit millisecond wrap");
	}

	// ---- A burst is not a commitment --------------------------------------
	//
	// Observed 2026-09-09: the plate touched the gate on its way up, the burst
	// started, and the compressor cut back in immediately. The plate was below
	// freezing ten seconds later and at -2.6 C when the burst finally ended,
	// having spent 85 of its 90 seconds doing the one thing the gate exists to
	// prevent. The policy used to decide once, when a burst was raised, and
	// never look again.

	{
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		in.plateConfigured = true;
		in.plateValid = true;
		uint32_t fan = 0;
		for (uint32_t t = 0; t < 10 * MIN; t += SEC) {
			in.nowMs = t;
			// Exactly on the gate until the burst is ten seconds old, then the
			// compressor comes back on.
			in.plateC = fan >= 10 ? -3.0f : fastCfg.plateGateC;
			if (p.update(in, fastCfg).fanOn) {
				fan++;
			}
		}
		check(fan >= 10 && fan <= 20, "a plate that dives mid-burst cuts the burst short");
	}

	{
		// The same dive, but immediately. The burst still gets its minimum
		// on-time: a relay that can be dropped one tick after it closed is a
		// worse problem than a few seconds of airflow.
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		in.plateConfigured = true;
		in.plateValid = true;
		uint32_t fan = 0;
		for (uint32_t t = 0; t < 10 * MIN; t += SEC) {
			in.nowMs = t;
			in.plateC = fan >= 1 ? -3.0f : fastCfg.plateGateC;
			if (p.update(in, fastCfg).fanOn) {
				fan++;
			}
		}
		check(fan >= 5 && fan <= 8, "a burst is never cut shorter than the minimum on-time");
	}

	{
		// A burst that overrode the gate to start must not then be cut off by
		// it, or forcing it through would amount to not running it at all.
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		in.plateConfigured = true;
		in.plateValid = true;
		in.plateC = -9.0f; // never warms
		// 12 h to the scheduled slot, then 6 h of deferral before it forces.
		uint32_t fan = runFor(p, cfg, in, 20 * HOUR);
		check(fan >= 89 && fan <= 91, "a burst forced past the gate runs its full length");
	}

	{
		// The probe quantises to 0.1 C, so a plate parked on the gate reads
		// either side of it from one sample to the next. Re-reading the gate
		// every tick must survive that without chattering the relay -- which is
		// what the stop hysteresis is for.
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		in.plateConfigured = true;
		in.plateValid = true;

		uint32_t minOn = 0xFFFFFFFFu;
		uint32_t minOff = 0xFFFFFFFFu;
		uint32_t run = 0;
		bool was = false;
		bool first = true;
		for (uint32_t t = 0; t < 1 * HOUR; t += SEC) {
			in.nowMs = t;
			in.plateC = fastCfg.plateGateC + ((t / SEC) % 2 == 0 ? 0.1f : -0.1f);
			const bool on = p.update(in, fastCfg).fanOn;
			if (!first && on != was) {
				if (was && run < minOn) {
					minOn = run;
				} else if (!was && run < minOff) {
					minOff = run;
				}
				run = 0;
			}
			run++;
			was = on;
			first = false;
		}
		check(minOn >= 89, "a plate sitting on the gate cannot chatter the relay");
		check(minOff >= 90, "...and cannot restart inside the settle window");
	}

	printf("%s\n", failures == 0 ? "all ok" : "FAILURES");
	return failures == 0 ? 0 : 1;
}

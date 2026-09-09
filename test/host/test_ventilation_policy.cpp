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
	cfg.settleMinutes = 1.5f;
	cfg.plateGateC = 2.0f;
	cfg.maxDeferMinutes = 360.0f;
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

/// Humidity as this chamber actually behaves: a large swing at the compressor
/// period, as frost goes onto the evaporator plate and comes back off it.
/// Triangular rather than sinusoidal so the test needs no math header -- the
/// shape is not what matters here, the amplitude is.
float cycling_humidity(uint32_t tMs, float mean, float amp, uint32_t periodMs) {
	const uint32_t half = periodMs / 2;
	const uint32_t phase = tMs % periodMs;
	const float up = phase < half
		? (float)phase / (float)half
		: 1.0f - (float)(phase - half) / (float)half;
	return (mean - amp) + 2.0f * amp * up;
}

/// Run the policy with humidity cycling the way the compressor makes it.
uint32_t runCycling(VentilationPolicy& p, const VentilationSettings& cfg,
					float mean, float amp, uint32_t durationMs) {
	VentilationInputs in = nominal(0);
	uint32_t fanSeconds = 0;
	for (uint32_t t = 0; t < durationMs; t += SEC) {
		in.nowMs = t;
		in.humidity = cycling_humidity(t, mean, amp, 45 * MIN);
		if (p.update(in, cfg).fanOn) {
			fanSeconds++;
		}
	}
	return fanSeconds;
}

} // namespace

int main(void) {
	const VentilationSettings cfg = defaults();
	// The same settings with the humidity filter switched off. The filter
	// withholds the dry trigger for a full time constant after boot, which is
	// correct but makes it impossible to test burst mechanics inside a few
	// simulated minutes. Tests that are about what a burst does once it is
	// running use this; tests that are about the trigger itself use cfg.
	VentilationSettings rawCfg = defaults();
	rawCfg.humidityAverageMinutes = 0.0f;

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
		// There is no daily cap on humidity-driven bursts, and none is needed:
		// burst-then-settle is itself the bound. 90 s of fan per 20 min of
		// settling is a ceiling of about 7% duty, which is a different animal
		// from the old controller holding the fan on for hours.
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		in.humidity = 60.0f;
		uint32_t fan = runFor(p, cfg, in, 20 * HOUR);
		const uint32_t ceiling = 20 * 3600 * 90 / (90 + 90);
		check(fan <= ceiling + 91, "burst-and-settle bounds a permanently dry chamber");
		check(fan > 6 * 91, "...without an arbitrary daily cap cutting it short");
	}

	{
		// The hole this replaced the cap with: the defer timeout used to apply
		// to every trigger, so a humidity burst held back by a frozen plate was
		// forced through after maxDeferMinutes anyway -- onto the exact surface
		// the gate exists to protect.
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		in.humidity = 55.0f;
		in.plateConfigured = true;
		in.plateValid = true;
		in.plateC = -9.0f;
		uint32_t fan = runFor(p, cfg, in, 20 * HOUR);
		check(fan == 0, "a dry chamber never forces air onto a sub-zero plate");
	}

	{
		// Scheduled air is different: stale air is a real problem, so a burst
		// owed since this morning goes ahead even if the plate stays cold.
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		in.humidity = 78.0f; // in band, so only the schedule can trigger
		in.plateConfigured = true;
		in.plateValid = true;
		in.plateC = -9.0f;
		uint32_t fan = runFor(p, cfg, in, 20 * HOUR);
		check(fan > 0, "scheduled air still forces through after the defer timeout");
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

	{
		// Observed 2026-09-09: a humidity burst finished ten seconds into the
		// 18:00 slot, and the scheduled burst ran ninety seconds later. Between
		// them they took the chamber from 63% to 87% RH. Whatever asked for it,
		// a burst is fresh air, and the slot it ends in has had its air.
		//
		// Start at 17:58:30 in a dry chamber, so a humidity burst is raised at
		// once and ends at about 18:00:00.
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		in.haveClock = true;
		const int64_t base = 1767200000 - (1767200000 % 86400) + 17 * 3600 + 58 * 60 + 30;
		in.localEpoch = base;
		in.humidity = 60.0f; // dry: asks for a burst immediately

		uint32_t fan = 0;
		for (uint32_t t = 0; t < 20 * MIN; t += SEC) {
			in.nowMs = t;
			in.localEpoch = base + t / SEC;
			if (t >= 2 * MIN) {
				// The burst did its job, so humidity stops asking. Anything
				// that runs after this point is the schedule, not the chamber.
				in.humidity = 78.0f;
			}
			if (p.update(in, rawCfg).fanOn) {
				fan++;
			}
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
		// Humidity-driven air counts towards the minimum: a chamber already
		// getting plenty of air should not get make-up bursts on top. Checked
		// as a comparison rather than against a number, because with no daily
		// cap a permanently dry chamber runs the burst-and-settle cycle all
		// day and the absolute figure says nothing on its own.
		VentilationSettings none = defaults();
		none.minSecondsPerDay = 0.0f;
		VentilationPolicy p1;
		VentilationInputs in1 = nominal(0);
		in1.humidity = 60.0f;
		const uint32_t withoutMinimum = runFor(p1, none, in1, 24 * HOUR);

		VentilationSettings with = defaults();
		with.minSecondsPerDay = 900.0f;
		VentilationPolicy p2;
		VentilationInputs in2 = nominal(0);
		in2.humidity = 60.0f;
		const uint32_t withMinimum = runFor(p2, with, in2, 24 * HOUR);

		check(withMinimum == withoutMinimum,
			  "dry-driven bursts count towards the daily minimum");
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
	// Both of these reproduce failures seen on the real chamber. The policy
	// used to decide once, when a burst was raised, and never look again.

	{
		// Observed 2026-09-09: the plate touched the gate on its way up, the
		// burst started, and the compressor cut back in immediately. The plate
		// was below freezing ten seconds later and at -2.6 C when the burst
		// finally ended, having spent 85 of its 90 seconds doing the one thing
		// the gate exists to prevent.
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		in.humidity = 60.0f; // dry: asks for a burst
		in.plateConfigured = true;
		in.plateValid = true;
		in.plateC = rawCfg.plateGateC; // exactly on the gate, so the burst starts
		uint32_t fan = 0;
		for (uint32_t t = 0; t < 5 * MIN; t += SEC) {
			in.nowMs = t;
			if (t >= 10 * SEC) {
				in.plateC = -3.0f; // the compressor comes back on
			}
			if (p.update(in, rawCfg).fanOn) {
				fan++;
			}
		}
		check(fan >= 5 && fan <= 20, "a plate that dives mid-burst cuts the burst short");
	}

	{
		// The same dive, but immediately. The burst still gets its minimum
		// on-time: a relay that can be dropped one tick after it closed is a
		// worse problem than a few seconds of airflow.
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		in.humidity = 60.0f;
		in.plateConfigured = true;
		in.plateValid = true;
		in.plateC = rawCfg.plateGateC;
		uint32_t fan = 0;
		for (uint32_t t = 0; t < 5 * MIN; t += SEC) {
			in.nowMs = t;
			if (t >= 1 * SEC) {
				in.plateC = -3.0f;
			}
			if (p.update(in, rawCfg).fanOn) {
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
		in.humidity = 78.0f; // in band, so only the schedule can trigger
		in.plateConfigured = true;
		in.plateValid = true;
		in.plateC = -9.0f; // never warms
		// 12 h to the scheduled slot, then 6 h of deferral before it forces.
		uint32_t fan = runFor(p, cfg, in, 20 * HOUR);
		check(fan >= 89 && fan <= 91, "a burst forced past the gate runs its full length");
	}

	{
		// Observed 2026-09-09: a burst raised at 63% RH waited seven minutes
		// for the plate and fired at 72.7% -- above the setpoint it was meant
		// to be climbing towards. Humidity rises steeply exactly while a burst
		// is pending, because the same warming plate that opens the gate is
		// giving its frost back to the air.
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		in.humidity = 60.0f;
		in.plateConfigured = true;
		in.plateValid = true;
		in.plateC = -9.0f;
		uint32_t fan = 0;
		for (uint32_t t = 0; t < 30 * MIN; t += SEC) {
			in.nowMs = t;
			if (t >= 5 * MIN) {
				in.humidity = 76.0f; // frost coming back off the plate
			}
			if (t >= 10 * MIN) {
				in.plateC = 5.0f; // gate opens, but the reason has gone
			}
			if (p.update(in, rawCfg).fanOn) {
				fan++;
			}
		}
		check(fan == 0, "a pending humidity burst is withdrawn once the chamber is no longer dry");
	}

	{
		// The deadband that keeps the previous test from cancelling every
		// burst it starts: raised at setpoint - undershoot, held to setpoint.
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		in.humidity = 60.0f;
		uint32_t fan = 0;
		for (uint32_t t = 0; t < 3 * MIN; t += SEC) {
			in.nowMs = t;
			if (t >= 10 * SEC) {
				// Working, but not finished.
				in.humidity = rawCfg.humiditySetpoint - 3.0f;
			}
			if (p.update(in, rawCfg).fanOn) {
				fan++;
			}
		}
		check(fan >= 89 && fan <= 91, "a humidity burst holds past the level that raised it");
	}

	{
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		in.humidity = 60.0f;
		uint32_t fan = 0;
		for (uint32_t t = 0; t < 3 * MIN; t += SEC) {
			in.nowMs = t;
			if (t >= 10 * SEC) {
				in.humidity = rawCfg.humiditySetpoint + 1.0f;
			}
			if (p.update(in, rawCfg).fanOn) {
				fan++;
			}
		}
		check(fan >= 5 && fan <= 20, "...and stops once the chamber reaches the setpoint");
	}

	{
		// The degenerate configuration: no deadband at all, with humidity
		// parked exactly on the threshold, so the test that raises a burst and
		// the test that holds one disagree on every single tick. Re-reading
		// conditions continuously has to survive this without chattering the
		// relay.
		VentilationSettings c = cfg;
		c.humidityUndershoot = 0.0f;
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		in.humidity = c.humiditySetpoint;

		uint32_t minOn = 0xFFFFFFFFu;
		uint32_t minOff = 0xFFFFFFFFu;
		uint32_t run = 0;
		bool was = false;
		bool first = true;
		for (uint32_t t = 0; t < 1 * HOUR; t += SEC) {
			in.nowMs = t;
			const bool on = p.update(in, c).fanOn;
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
		check(minOn >= 5, "a condition parked on its threshold cannot chatter the relay");
		check(minOff >= 90, "...and cannot restart inside the settle window");
	}

	// ---- Humidity is filtered before it may ask for air --------------------
	//
	// Measured on the chamber, 2026-09-09, over a clean 47-minute compressor
	// cycle with the fan idle: water content swung 3.38 to 6.60 g/kg, 49% of
	// its own peak, purely from frost cycling on and off the plate. Relative
	// humidity swung 27 points with it. None of that says anything about how
	// much water the chamber holds.

	{
		// A chamber sitting exactly on setpoint, swinging the measured amount
		// either side of it. Every cycle dips well past the trigger; none of
		// those dips mean the chamber is dry. It must not ventilate.
		VentilationPolicy p;
		uint32_t fan = runCycling(p, cfg, cfg.humiditySetpoint, 13.5f, 6 * HOUR);
		check(fan == 0, "a compressor cycle's humidity swing does not ask for air");
	}

	{
		// The same swing around a mean that is genuinely dry. This one should.
		VentilationPolicy p;
		uint32_t fan = runCycling(p, cfg, 62.0f, 13.5f, 6 * HOUR);
		check(fan > 90, "a chamber dry across the whole cycle still gets bursts");
	}

	{
		// Without the filter, the identical in-band chamber ventilates on the
		// trough of every cycle -- which is the behaviour being fixed. This is
		// what the device did on 2026-09-09, raising a burst at 63% RH.
		VentilationPolicy p;
		uint32_t fan = runCycling(p, rawCfg, cfg.humiditySetpoint, 13.5f, 6 * HOUR);
		check(fan > 90, "...and without the filter that same chamber ventilates");
	}

	{
		// Seeded from a single reading, the average is that reading. Boot at
		// the trough of a cycle and an unfiltered trigger would fire at once,
		// so the trigger waits out a full time constant first.
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		in.humidity = 60.0f; // dry, and dry on average -- but not yet known to be
		uint32_t early = runFor(p, cfg, in, 29 * MIN);
		check(early == 0, "the dry trigger waits for the humidity filter to warm up");

		VentilationInputs in2 = in;
		in2.nowMs = 29 * MIN;
		uint32_t late = runFor(p, cfg, in2, 20 * MIN);
		check(late > 0, "...and asks for air once it has");
	}

	{
		// The average raises on its own. Requiring the reading to agree as well
		// would mean the trigger may only speak at the bottom of a compressor
		// cycle, which is exactly when the plate is coldest and the gate shut:
		// measured on the chamber, the two conditions overlapped 22% of the
		// time. Here the chamber is dry on average and the reading has just
		// swung high, as it does on the warm half of every cycle. That is the
		// moment the fan should be free to run.
		VentilationPolicy p;
		VentilationInputs in = nominal(0);
		in.humidity = 60.0f;
		runFor(p, cfg, in, 29 * MIN); // warm the filter, still too early to fire

		VentilationInputs high = in;
		high.nowMs = 29 * MIN;
		high.humidity = cfg.humiditySetpoint + 5.0f; // well above the raise level
		uint32_t fan = runFor(p, cfg, high, 5 * MIN);
		check(fan > 0, "a dry average raises a burst through a high reading");
	}

	printf("%s\n", failures == 0 ? "all ok" : "FAILURES");
	return failures == 0 ? 0 : 1;
}

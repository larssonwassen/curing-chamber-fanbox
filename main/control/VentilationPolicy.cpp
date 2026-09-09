#include "VentilationPolicy.h"

namespace {

const int64_t SECONDS_PER_DAY = 86400;

/// Floor division that stays correct for negative values, which matters for
/// pre-1970 clocks and for the west-of-UTC case where localEpoch can dip below
/// a day boundary.
int64_t floor_div(int64_t a, int64_t b) {
	int64_t q = a / b;
	if ((a % b != 0) && ((a < 0) != (b < 0))) {
		q--;
	}
	return q;
}

/// Elapsed milliseconds, correct across the ~49-day wrap of a 32-bit tick
/// counter. Unsigned subtraction does the right thing; the cast is the point.
uint32_t elapsed_ms(uint32_t now, uint32_t then) {
	return (uint32_t)(now - then);
}

/// Shortest time the fan may be commanded on. Every burst re-checks its own
/// reasons on each tick, and a condition that flickers across its threshold
/// would otherwise cycle the relay at the loop rate. Nothing here is allowed to
/// stop a burst before this has passed; the burst length is the ceiling, this
/// is the floor.
const uint32_t MIN_ON_MS = 5000;

/// How far below `plateGateC` the plate must fall before it cuts a burst that
/// is already running. The probe quantises to 0.1 C, so a plate parked on the
/// gate reads either side of it from one sample to the next; without this a
/// burst would be started and cut by the same noise. Three counts is enough to
/// ignore that and still catch a plate genuinely on its way back down.
const float PLATE_STOP_HYSTERESIS_C = 0.3f;

int64_t slot_seconds(const VentilationSettings& cfg) {
	int64_t s = (int64_t)(cfg.intervalHours * 3600.0f);
	// A zero or negative interval would make every update "due", pinning the
	// fan on. One minute is already absurdly frequent; it is a floor, not a
	// recommendation.
	if (s < 60) {
		s = 60;
	}
	return s;
}

} // namespace

void VentilationPolicy::reset() {
	*this = VentilationPolicy();
}

/// The scheduled slot containing `localEpoch`: slots run every intervalHours
/// from firstHourLocal each local day.
///
/// Note that an interval that does not divide 24 restarts at firstHourLocal
/// every midnight, so the last slot of a day can be short. That is deliberate:
/// an anchored, predictable "06:00 and 18:00" is easier to reason about than a
/// free-running period that drifts around the clock.
static int64_t current_slot(int64_t localEpoch, const VentilationSettings& cfg) {
	const int64_t slot = slot_seconds(cfg);
	int64_t dayStart = floor_div(localEpoch, SECONDS_PER_DAY) * SECONDS_PER_DAY;
	int64_t anchor = dayStart + (int64_t)cfg.firstHourLocal * 3600;
	if (localEpoch < anchor) {
		// Before the day's first slot: we are still in the tail of yesterday's
		// schedule.
		anchor -= SECONDS_PER_DAY;
	}
	return anchor + floor_div(localEpoch - anchor, slot) * slot;
}

bool VentilationPolicy::scheduledDue(const VentilationInputs& in, const VentilationSettings& cfg) {
	if (in.haveClock) {
		const int64_t slot = current_slot(in.localEpoch, cfg);
		if (!haveRunSlot_) {
			// First sight of a real clock. Adopt the slot we happen to be
			// inside as already served rather than ventilating on the spot:
			// otherwise every boot -- including a reboot loop -- would trigger
			// a burst, and a device that reboots often would ventilate far more
			// than the schedule asks for.
			haveRunSlot_ = true;
			lastRunSlot_ = slot;
			return false;
		}
		return slot != lastRunSlot_;
	}

	// No clock. Fall back to elapsed time, anchored on boot so that the first
	// burst is a full interval away rather than immediate -- same reasoning as
	// above about reboots.
	const uint32_t intervalMs = (uint32_t)(slot_seconds(cfg) * 1000);
	const uint32_t since = everBurst_ ? elapsed_ms(in.nowMs, burstEndedMs_)
									  : elapsed_ms(in.nowMs, bootMs_);
	return since >= intervalMs;
}

/// Advance the humidity filter. First-order, evaluated from elapsed time
/// rather than from a tick count, so it does not depend on the caller's
/// cadence and survives a missed update.
void VentilationPolicy::updateHumidityAverage(const VentilationInputs& in, const VentilationSettings& cfg) {
	if (!in.humidityValid) {
		// Hold the last average rather than decaying it towards nothing. A
		// dropout should freeze the filter, not slowly invent a dry chamber.
		return;
	}
	if (!humidityAvgValid_) {
		humidityAvgValid_ = true;
		humidityAvg_ = in.humidity;
		humidityAvgMs_ = in.nowMs;
		humidityAvgSeedMs_ = in.nowMs;
		return;
	}
	const float tauMs = cfg.humidityAverageMinutes * 60000.0f;
	float alpha = 1.0f;
	if (tauMs > 0.0f) {
		alpha = (float)elapsed_ms(in.nowMs, humidityAvgMs_) / tauMs;
		if (alpha > 1.0f) {
			// A long gap -- a stall, or the first update after a wrap. Adopt
			// the current reading instead of extrapolating a filter that has
			// no information about what happened in between.
			alpha = 1.0f;
		}
	}
	humidityAvg_ += (in.humidity - humidityAvg_) * alpha;
	humidityAvgMs_ = in.nowMs;
}

bool VentilationPolicy::dryRequest(const VentilationInputs& in, const VentilationSettings& cfg) const {
	if (!in.humidityValid) {
		// A stale reading simply means humidity gets no vote. It does not stop
		// scheduled ventilation: bursts are bounded, so a sensor failure can no
		// longer leave the fan running indefinitely the way the old setpoint
		// loop could.
		return false;
	}
	if (everBurst_) {
		const uint32_t settleMs = (uint32_t)(cfg.settleMinutes * 60000.0f);
		if (elapsed_ms(in.nowMs, burstEndedMs_) < settleMs) {
			return false;
		}
	}
	const float raiseBelow = cfg.humiditySetpoint - cfg.humidityUndershoot;
	// Raised on the average, not on the reading. The trough of every compressor
	// cycle looks like a dry chamber: the plate gives its frost back to the air
	// on the warm half of the cycle and takes it again on the cold half, which
	// is worth tens of points of relative humidity in either direction and says
	// nothing at all about how much water the chamber actually holds.
	//
	// Requiring the raw reading to agree, as this did at first, quietly undoes
	// that. The two are only both below the line at the bottom of a cycle,
	// which is exactly when the plate is coldest and the gate is shut. Measured
	// on 2026-09-09 with the gate at 2 C: the raw reading was under the raise
	// level 63% of the time and the plate was above the gate 56% of the time,
	// but the two overlapped only 22% of the time, and one of that evening's
	// four gate openings arrived to find the raw reading had already climbed
	// back over the line. A trigger that may only speak while the plate is
	// frozen is a trigger that rarely gets to act.
	//
	// Holding is a different question and still reads the raw value; see
	// dryHolds(). "Is this chamber dry?" is about its state and needs the
	// filter. "Has this burst delivered enough yet?" is about what the fan just
	// did, which the filter is deliberately too slow to see.
	if (cfg.humidityAverageMinutes > 0.0f) {
		const uint32_t tauMs = (uint32_t)(cfg.humidityAverageMinutes * 60000.0f);
		// Until the filter has seen a full time constant it is still mostly the
		// single reading it was seeded from, and seeding at the trough of a
		// cycle would look exactly like a dry chamber. Withhold the dry trigger
		// rather than act on that: the schedule and the daily minimum still run,
		// and a humidity burst is never urgent.
		if (!humidityAvgValid_ || elapsed_ms(in.nowMs, humidityAvgSeedMs_) < tauMs) {
			return false;
		}
		return humidityAvg_ <= raiseBelow;
	}
	// With the filter switched off there is nothing to average, so the reading
	// itself is all there is to go on.
	return in.humidity <= raiseBelow;
}

/// Whether a humidity burst that has already been asked for is still worth
/// having.
///
/// Deliberately a weaker test than dryRequest(): a burst is raised at
/// `setpoint - undershoot` and held all the way up to `setpoint`. The gap is
/// what stops a burst from raising humidity past its own trigger and
/// immediately cancelling itself, and the numbers for it are already
/// configured -- no new knob.
bool VentilationPolicy::dryHolds(const VentilationInputs& in, const VentilationSettings& cfg) const {
	if (!in.humidityValid) {
		// The reading that raised this burst has since gone stale. Let the
		// burst finish rather than cancel it: it is bounded anyway, and a
		// sensor dropout is not evidence the chamber got wetter.
		return true;
	}
	return in.humidity < cfg.humiditySetpoint;
}

/// Whether the reason a burst was asked for still applies.
///
/// Only humidity can withdraw. Scheduled and make-up bursts exist to exchange
/// air on a timetable and have no condition to satisfy, and a manual burst is
/// somebody pressing a button -- second-guessing that is not this function's
/// job.
bool VentilationPolicy::triggerHolds(const VentilationInputs& in, const VentilationSettings& cfg) const {
	if (trigger_ == VentTrigger::Dry) {
		return dryHolds(in, cfg);
	}
	return true;
}

/// Whether the plate is still warm enough to keep a running burst going.
///
/// A burst that overrode the gate is not subject to it afterwards: forcing a
/// deferred scheduled burst through and then cutting it off on the first tick
/// would be the same as not running it at all.
bool VentilationPolicy::plateHolds(const VentilationInputs& in, const VentilationSettings& cfg) const {
	if (burstForced_ || !in.plateConfigured) {
		return true;
	}
	if (!in.plateValid) {
		return false;
	}
	return in.plateC >= (cfg.plateGateC - PLATE_STOP_HYSTERESIS_C);
}

bool VentilationPolicy::plateAllows(const VentilationInputs& in, const VentilationSettings& cfg) const {
	if (!in.plateConfigured) {
		return true; // No probe fitted: nothing to gate on.
	}
	if (!in.plateValid) {
		// Configured but not reading. Treat as cold -- the whole point of the
		// gate is to avoid dumping moist air onto sub-zero metal, and "I don't
		// know" is not a reason to assume the safe case. The defer timeout in
		// update() keeps this from blocking ventilation forever.
		return false;
	}
	return in.plateC >= cfg.plateGateC;
}

VentilationDecision VentilationPolicy::update(const VentilationInputs& in, const VentilationSettings& cfg) {
	if (!bootMsSet_) {
		bootMs_ = in.nowMs;
		bootMsSet_ = true;
	}
	rollDayWindow(in);
	updateHumidityAverage(in, cfg);

	VentilationDecision d;

	switch (state_) {
	case VentState::Running: {
		// A burst is not a commitment. The conditions that justified it are
		// re-read on every tick, because the thing this gate exists to prevent
		// -- moist room air meeting sub-zero metal -- is just as bad thirty
		// seconds into a burst as it was at the start. Only the minimum on-time
		// is unconditional.
		const uint32_t burstMs = (uint32_t)(cfg.burstSeconds * 1000.0f);
		const uint32_t elapsed = elapsed_ms(in.nowMs, burstStartedMs_);
		const char* stop = nullptr;
		if (elapsed >= burstMs) {
			stop = "burst complete";
		} else if (elapsed >= MIN_ON_MS) {
			if (!plateHolds(in, cfg)) {
				stop = "plate went cold; burst cut short";
			} else if (!triggerHolds(in, cfg)) {
				stop = "reason satisfied; burst cut short";
			}
		}

		if (stop != nullptr) {
			state_ = VentState::Settling;
			burstEndedMs_ = in.nowMs;
			everBurst_ = true;
			// Count what actually ran, not what was asked for: a burst whose
			// settings changed underneath it, or one cut short by the plate or
			// by its own reason, should contribute what it really contributed.
			dayRunMs_ += elapsed;
			// Whatever asked for it, a burst is fresh air, so it serves the
			// schedule slot it ends in. Without this the schedule cannot tell
			// that the chamber has just been ventilated: on 2026-09-09 a
			// humidity burst finished ten seconds into the 18:00 slot and the
			// scheduled burst followed ninety seconds later, taking the chamber
			// from 63% to 87% RH between them. The no-clock path already
			// behaves this way, because it measures the interval from the end
			// of the last burst rather than from a slot boundary.
			if (in.haveClock) {
				lastRunSlot_ = current_slot(in.localEpoch, cfg);
				haveRunSlot_ = true;
			}
			d.fanOn = false;
			d.state = state_;
			d.trigger = trigger_;
			d.reason = stop;
			return d;
		}
		d.fanOn = true;
		d.state = state_;
		d.trigger = trigger_;
		d.reason = trigger_ == VentTrigger::Scheduled ? "scheduled burst"
				 : trigger_ == VentTrigger::Dry       ? "humidity burst"
				 : trigger_ == VentTrigger::Makeup    ? "daily minimum burst"
													  : "manual burst";
		return d;
	}

	case VentState::Settling: {
		const uint32_t settleMs = (uint32_t)(cfg.settleMinutes * 60000.0f);
		if (elapsed_ms(in.nowMs, burstEndedMs_) >= settleMs) {
			state_ = VentState::Idle;
			trigger_ = VentTrigger::None;
		}
		d.fanOn = false;
		d.state = state_;
		d.reason = state_ == VentState::Settling ? "settling" : "idle";
		return d;
	}

	case VentState::Pending: {
		// A burst is owed. Hold it while the plate is below freezing-ish, so
		// the moisture we bring in has somewhere to go other than straight onto
		// the coldest surface in the box.
		// The timeout exists so a cold plate cannot hold *air exchange* off
		// forever: stale air is a real problem, and a burst owed since this
		// morning has to happen eventually. Humidity is not that. Forcing a
		// humidity burst onto sub-zero metal is precisely the mechanism that
		// armoured the evaporator in the first place, and unlike fresh air,
		// nothing goes wrong if it simply waits -- the fan cannot dry the
		// chamber, so a burst it never gets is a burst it did not need.
		const bool mayForce = trigger_ != VentTrigger::Dry;
		const bool expired = mayForce &&
			elapsed_ms(in.nowMs, pendingSinceMs_) >= (uint32_t)(cfg.maxDeferMinutes * 60000.0f);

		// A request is not a commitment either. Waiting for the plate takes
		// minutes, and the chamber does not hold still meanwhile -- humidity
		// climbs steeply as frost comes back off the warming plate. Firing a
		// burst that was asked for at 63% into a chamber that is now at 73% is
		// how a "the chamber is too dry" request ends up making it wetter.
		if (!triggerHolds(in, cfg)) {
			state_ = VentState::Idle;
			trigger_ = VentTrigger::None;
			d.fanOn = false;
			d.state = state_;
			d.reason = "request withdrawn; no longer dry";
			return d;
		}

		if (plateAllows(in, cfg) || expired) {
			state_ = VentState::Running;
			burstStartedMs_ = in.nowMs;
			burstForced_ = expired && !plateAllows(in, cfg);
			d.fanOn = true;
			d.state = state_;
			d.trigger = trigger_;
			d.reason = expired ? "deferred too long; ventilating anyway"
							   : "plate warm enough; ventilating";
			return d;
		}
		d.fanOn = false;
		d.state = state_;
		d.trigger = trigger_;
		d.reason = in.plateValid ? "waiting for the plate to warm"
								 : "waiting for a plate reading";
		return d;
	}

	case VentState::Idle:
	default:
		break;
	}

	// Idle: is anything due? Manual first -- it is an explicit instruction and
	// skips both the settle timer and the plate gate.
	if (in.manualBurstRequest) {
		trigger_ = VentTrigger::Manual;
		state_ = VentState::Running;
		burstStartedMs_ = in.nowMs;
		// Manual skips the gate on the way in, so it is not subject to it on
		// the way through either.
		burstForced_ = true;
		d.fanOn = true;
		d.state = state_;
		d.trigger = trigger_;
		d.reason = "manual burst";
		return d;
	}

	VentTrigger want = VentTrigger::None;
	if (scheduledDue(in, cfg)) {
		want = VentTrigger::Scheduled;
	} else if (dryRequest(in, cfg)) {
		want = VentTrigger::Dry;
	} else if (makeupRequest(in, cfg)) {
		want = VentTrigger::Makeup;
	}

	if (want == VentTrigger::None) {
		d.state = VentState::Idle;
		d.reason = "idle";
		return d;
	}

	// Claim the schedule slot at the moment the burst is triggered, not when it
	// finally runs: otherwise a burst deferred past the next slot boundary
	// would immediately be owed again.
	if (want == VentTrigger::Scheduled && in.haveClock) {
		lastRunSlot_ = current_slot(in.localEpoch, cfg);
		haveRunSlot_ = true;
	}

	trigger_ = want;
	if (plateAllows(in, cfg)) {
		state_ = VentState::Running;
		burstStartedMs_ = in.nowMs;
		burstForced_ = false;
		d.fanOn = true;
		d.reason = want == VentTrigger::Scheduled ? "scheduled burst"
				 : want == VentTrigger::Makeup    ? "daily minimum burst"
												  : "humidity burst";
	} else {
		state_ = VentState::Pending;
		pendingSinceMs_ = in.nowMs;
		d.fanOn = false;
		d.reason = in.plateValid ? "plate too cold; burst deferred"
								 : "no plate reading; burst deferred";
	}
	d.state = state_;
	d.trigger = trigger_;
	return d;
}

/// Reset the per-day counters when the day rolls over.
///
/// With a clock this follows the local calendar day, so "six bursts a day" and
/// "ten minutes a day" mean what a person means by them. Without a clock it is
/// a rolling 24 hours from boot, which is the best available approximation.
void VentilationPolicy::rollDayWindow(const VentilationInputs& in) {
	if (in.haveClock) {
		const int64_t day = floor_div(in.localEpoch, SECONDS_PER_DAY);
		if (!dayIndexSet_) {
			dayIndexSet_ = true;
			dayIndex_ = day;
			// Booting at 17:30 does not mean the chamber had no air since
			// midnight -- it means this firmware was not watching. Credit the
			// elapsed part of the day rather than immediately topping up a
			// deficit it cannot know about, which would otherwise make every
			// reboot trigger a burst.
			dayStartFraction_ = (float)(in.localEpoch - day * SECONDS_PER_DAY) / (float)SECONDS_PER_DAY;
			return;
		}
		if (day != dayIndex_) {
			dayIndex_ = day;
			dayRunMs_ = 0;
			dayStartFraction_ = 0.0f;
		}
		return;
	}

	if (!dayWindowSet_) {
		dayWindowStartMs_ = in.nowMs;
		dayWindowSet_ = true;
		return;
	}
	if (elapsed_ms(in.nowMs, dayWindowStartMs_) >= (uint32_t)SECONDS_PER_DAY * 1000u) {
		dayWindowStartMs_ = in.nowMs;
		dayRunMs_ = 0;
	}
}

/// True when the day's fan time is far enough behind a prorated target to be
/// worth a burst now.
///
/// Prorated rather than checked at the end of the day: a chamber that has had
/// no air since morning should get some at lunchtime, not a ten-minute blast at
/// 23:59. The shortfall has to exceed a whole burst before one is issued, which
/// is what stops this from chasing the target continuously -- without that, a
/// minimum equal to what the schedule already delivers would still fire an
/// extra burst every time the linear target crept ahead between scheduled runs.
bool VentilationPolicy::makeupRequest(const VentilationInputs& in, const VentilationSettings& cfg) const {
	if (cfg.minSecondsPerDay <= 0.0f) {
		return false;
	}
	if (everBurst_) {
		const uint32_t settleMs = (uint32_t)(cfg.settleMinutes * 60000.0f);
		if (elapsed_ms(in.nowMs, burstEndedMs_) < settleMs) {
			return false;
		}
	}

	float fractionOfDay;
	if (in.haveClock) {
		const int64_t secondsIntoDay = in.localEpoch - floor_div(in.localEpoch, SECONDS_PER_DAY) * SECONDS_PER_DAY;
		fractionOfDay = (float)secondsIntoDay / (float)SECONDS_PER_DAY;
	} else {
		if (!dayWindowSet_) {
			return false;
		}
		// The no-clock window already starts at boot, so nothing has been
		// missed and dayStartFraction_ stays zero.
		fractionOfDay = (float)elapsed_ms(in.nowMs, dayWindowStartMs_) / (float)(SECONDS_PER_DAY * 1000);
	}
	if (fractionOfDay > 1.0f) {
		fractionOfDay = 1.0f;
	}

	// Only the part of the day this policy has actually been watching counts.
	float watched = fractionOfDay - dayStartFraction_;
	if (watched <= 0.0f) {
		return false;
	}
	const float targetSeconds = cfg.minSecondsPerDay * watched;
	const float ranSeconds = (float)dayRunMs_ / 1000.0f;
	return (targetSeconds - ranSeconds) >= cfg.burstSeconds;
}

uint32_t VentilationPolicy::msUntilScheduled(const VentilationInputs& in, const VentilationSettings& cfg) const {
	if (state_ != VentState::Idle) {
		return 0;
	}
	const int64_t slot = slot_seconds(cfg);
	if (in.haveClock) {
		if (!haveRunSlot_) {
			return 0;
		}
		const int64_t next = lastRunSlot_ + slot;
		if (in.localEpoch >= next) {
			return 0;
		}
		return (uint32_t)((next - in.localEpoch) * 1000);
	}
	const uint32_t intervalMs = (uint32_t)(slot * 1000);
	const uint32_t since = everBurst_ ? elapsed_ms(in.nowMs, burstEndedMs_)
									  : elapsed_ms(in.nowMs, bootMs_);
	return since >= intervalMs ? 0 : intervalMs - since;
}

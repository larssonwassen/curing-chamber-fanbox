#include "HumidifierPolicy.h"

namespace {

/// Elapsed milliseconds, correct across the ~49-day wrap of a 32-bit tick
/// counter. Unsigned subtraction does the right thing; the cast is the point.
uint32_t elapsed_ms(uint32_t now, uint32_t then) {
	return (uint32_t)(now - then);
}

/// Shortest time the fan may be commanded on. Every burst re-checks its own
/// reasons on each tick, and a condition that flickers across its threshold
/// would otherwise cycle the FET at the loop rate. The burst length is the
/// ceiling; this is the floor.
const uint32_t MIN_ON_MS = 5000;

/// How far below `plateGateC` the plate must fall before it cuts a burst that
/// is already running. The probe quantises to 0.1 C, so a plate parked on the
/// gate reads either side of it from one sample to the next.
const float PLATE_STOP_HYSTERESIS_C = 0.3f;

/// Longest gap between calls that is credited to the duty window as run time.
/// A task that was starved, or a policy driven from a test that jumps the
/// clock, should not have the whole jump counted as fan time.
const uint32_t MAX_ACCRUAL_MS = 10000;

} // namespace

void HumidifierPolicy::reset() {
	*this = HumidifierPolicy();
}

float HumidifierPolicy::dutyPercent() const {
	uint64_t total = 0;
	for (int i = 0; i < DUTY_BUCKETS; i++) {
		total += dutyBucketMs_[i];
	}
	const uint64_t windowMs = (uint64_t)DUTY_BUCKETS * DUTY_BUCKET_MS;
	return (float)((double)total * 100.0 / (double)windowMs);
}

/// Advance the rolling window, clearing buckets as they age out.
///
/// Bounded by DUTY_BUCKETS: after a gap longer than the whole window every
/// bucket is stale anyway, and looping thousands of times to work that out
/// would be the same answer arrived at slowly.
void HumidifierPolicy::rollDutyWindow(const HumidifierInputs& in) {
	int rolled = 0;
	while (elapsed_ms(in.nowMs, dutyBucketStartMs_) >= DUTY_BUCKET_MS) {
		dutyBucketStartMs_ += DUTY_BUCKET_MS;
		dutyBucket_ = (dutyBucket_ + 1) % DUTY_BUCKETS;
		dutyBucketMs_[dutyBucket_] = 0;
		if (++rolled >= DUTY_BUCKETS) {
			// More than a window has passed. Nothing in it is current.
			for (int i = 0; i < DUTY_BUCKETS; i++) {
				dutyBucketMs_[i] = 0;
			}
			dutyBucketStartMs_ = in.nowMs;
			dutyBucket_ = 0;
			return;
		}
	}
}

void HumidifierPolicy::accrueRunTime(const HumidifierInputs& in) {
	const uint32_t dt = elapsed_ms(in.nowMs, lastTickMs_);
	if (fanWasOn_ && dt <= MAX_ACCRUAL_MS) {
		dutyBucketMs_[dutyBucket_] += dt;
	}
	lastTickMs_ = in.nowMs;
}

bool HumidifierPolicy::dutyAllows(const HumidifierSettings& cfg) const {
	if (cfg.maxDutyPercent <= 0.0f) {
		return true; // Cap switched off.
	}
	return dutyPercent() < cfg.maxDutyPercent;
}

/// Whether the chamber is dry enough to ask for water.
///
/// Read on the average, for the reason set out in the header: the raw signal
/// swings tens of points per compressor cycle without the chamber gaining or
/// losing any water at all, and a trigger on it fires at every trough.
bool HumidifierPolicy::dryRequest(const HumidifierInputs& in, const HumidifierSettings& cfg) const {
	const float raiseBelow = cfg.targetRh - cfg.raiseBandRh;

	if (!in.averageEnabled) {
		// With the filter switched off there is nothing to average, so the
		// reading itself is all there is to go on.
		return in.humidityValid && in.humidity <= raiseBelow;
	}
	if (!in.humidityAvgValid) {
		// The filter has not settled. Acting on a half-warm average is worse
		// than waiting -- the chamber has been dry for hours by the time this
		// matters, and another few minutes changes nothing.
		return false;
	}
	return in.humidityAvg <= raiseBelow;
}

/// Whether a burst still has a reason to be running.
///
/// On the raw reading, not the average: the whole point of a burst is to move
/// the instantaneous humidity, and the filter is built to lag exactly that
/// step. Holding on the average would run every burst until the filter caught
/// up, which is several time constants after the chamber was already wet
/// enough.
bool HumidifierPolicy::dryHolds(const HumidifierInputs& in, const HumidifierSettings& cfg) const {
	if (!in.humidityValid) {
		// The reading that raised this burst has gone stale. Let the burst
		// finish rather than cancel it: it is bounded by burstSeconds anyway,
		// and a sensor dropout is not evidence the chamber got wetter.
		return true;
	}
	return in.humidity < cfg.targetRh;
}

bool HumidifierPolicy::triggerHolds(const HumidifierInputs& in, const HumidifierSettings& cfg) const {
	if (trigger_ == HumidifierTrigger::Dry) {
		return dryHolds(in, cfg);
	}
	return true;
}

bool HumidifierPolicy::plateAllows(const HumidifierInputs& in, const HumidifierSettings& cfg) const {
	if (!in.plateConfigured) {
		return true; // No probe fitted: nothing to gate on.
	}
	if (!in.plateValid) {
		// Configured but not reading. Treat as cold. Unlike ventilation there
		// is no defer timeout to rescue this, which is the intended trade: a
		// dead probe stops humidification until somebody notices, and
		// humidifier_state sitting in "pending" is how they notice.
		return false;
	}
	return in.plateC >= cfg.plateGateC;
}

bool HumidifierPolicy::plateHolds(const HumidifierInputs& in, const HumidifierSettings& cfg) const {
	if (burstForced_ || !in.plateConfigured) {
		return true;
	}
	if (!in.plateValid) {
		return false;
	}
	return in.plateC >= (cfg.plateGateC - PLATE_STOP_HYSTERESIS_C);
}

HumidifierDecision HumidifierPolicy::update(const HumidifierInputs& in, const HumidifierSettings& cfg) {
	if (!bootMsSet_) {
		bootMsSet_ = true;
		lastTickMs_ = in.nowMs;
		dutyBucketStartMs_ = in.nowMs;
	}
	// Order matters: accrue against the bucket the time was actually spent in,
	// then roll. Rolling first would post the last interval to a fresh bucket.
	accrueRunTime(in);
	rollDutyWindow(in);

	HumidifierDecision d;

	switch (state_) {
	case HumidifierState::Idle: {
		if (in.manualBurstRequest) {
			state_ = HumidifierState::Running;
			trigger_ = HumidifierTrigger::Manual;
			burstStartedMs_ = in.nowMs;
			burstForced_ = true;
			d.fanOn = true;
			d.reason = "manual burst";
			break;
		}

		// Normally unreachable, because Settling only falls through to Idle once
		// its window has closed. It catches the case where settleMinutes is
		// raised while idle, which retroactively reopens a window the policy
		// had already left.
		const uint32_t settleMs = (uint32_t)(cfg.settleMinutes * 60000.0f);
		if (everBurst_ && elapsed_ms(in.nowMs, burstEndedMs_) < settleMs) {
			state_ = HumidifierState::Settling;
			d.reason = "settling";
			break;
		}

		if (!dryRequest(in, cfg)) {
			d.reason = "idle";
			break;
		}
		if (!dutyAllows(cfg)) {
			// Asked for, refused. Worth its own reason string: a chamber that
			// sits here is one where the target cannot be met inside the duty
			// the tray and the mould risk allow.
			d.reason = "dry, but at the duty cap";
			break;
		}

		trigger_ = HumidifierTrigger::Dry;
		if (plateAllows(in, cfg)) {
			state_ = HumidifierState::Running;
			burstStartedMs_ = in.nowMs;
			burstForced_ = false;
			d.fanOn = true;
			d.reason = "dry; humidifying";
		} else {
			state_ = HumidifierState::Pending;
			d.reason = "dry, but the plate is too cold to add moisture";
		}
		break;
	}

	case HumidifierState::Pending: {
		if (in.manualBurstRequest) {
			state_ = HumidifierState::Running;
			trigger_ = HumidifierTrigger::Manual;
			burstStartedMs_ = in.nowMs;
			burstForced_ = true;
			d.fanOn = true;
			d.reason = "manual burst";
			break;
		}
		// A request is not a commitment. Waiting for the plate takes minutes,
		// and the chamber does not hold still meanwhile -- humidity climbs
		// steeply as frost comes back off the warming plate, which is the very
		// event that makes the plate warm enough to proceed. Firing a burst
		// raised at 74% into a chamber now sitting at 80% is how "too dry" ends
		// up as "too wet".
		if (!triggerHolds(in, cfg)) {
			state_ = HumidifierState::Idle;
			trigger_ = HumidifierTrigger::None;
			d.reason = "request withdrawn; no longer dry";
			break;
		}
		if (!dutyAllows(cfg)) {
			state_ = HumidifierState::Idle;
			trigger_ = HumidifierTrigger::None;
			d.reason = "request withdrawn; at the duty cap";
			break;
		}
		if (plateAllows(in, cfg)) {
			state_ = HumidifierState::Running;
			burstStartedMs_ = in.nowMs;
			burstForced_ = false;
			d.fanOn = true;
			d.reason = "plate warm enough; humidifying";
			break;
		}
		// There is deliberately no timeout here. See the header: ventilation
		// forces a deferred burst through because stale air is a real problem;
		// humidification never does, because a burst it never gets is a burst
		// the plate would have turned straight into frost.
		d.reason = "waiting for the plate to warm";
		break;
	}

	case HumidifierState::Running: {
		const uint32_t elapsed = elapsed_ms(in.nowMs, burstStartedMs_);
		const uint32_t burstMs = (uint32_t)(cfg.burstSeconds * 1000.0f);

		// Nothing may cut a burst shorter than this, whatever else changed.
		if (elapsed < MIN_ON_MS) {
			d.fanOn = true;
			d.reason = "humidifying";
			break;
		}

		const char* stop = nullptr;
		if (elapsed >= burstMs) {
			stop = "burst complete";
		} else if (!triggerHolds(in, cfg)) {
			stop = "target reached";
		} else if (!plateHolds(in, cfg)) {
			stop = "plate fell below the gate";
		} else if (!dutyAllows(cfg)) {
			stop = "duty cap reached mid-burst";
		}

		if (stop != nullptr) {
			state_ = HumidifierState::Settling;
			trigger_ = HumidifierTrigger::None;
			burstEndedMs_ = in.nowMs;
			everBurst_ = true;
			burstForced_ = false;
			d.reason = stop;
			break;
		}

		d.fanOn = true;
		d.reason = "humidifying";
		break;
	}

	case HumidifierState::Settling: {
		if (in.manualBurstRequest) {
			state_ = HumidifierState::Running;
			trigger_ = HumidifierTrigger::Manual;
			burstStartedMs_ = in.nowMs;
			burstForced_ = true;
			d.fanOn = true;
			d.reason = "manual burst";
			break;
		}
		const uint32_t settleMs = (uint32_t)(cfg.settleMinutes * 60000.0f);
		if (elapsed_ms(in.nowMs, burstEndedMs_) >= settleMs) {
			state_ = HumidifierState::Idle;
			d.reason = "idle";
			break;
		}
		d.reason = "settling";
		break;
	}
	}

	d.state = state_;
	d.trigger = (d.fanOn || state_ == HumidifierState::Pending) ? trigger_
																: HumidifierTrigger::None;
	fanWasOn_ = d.fanOn;
	return d;
}

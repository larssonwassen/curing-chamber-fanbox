#ifndef _HUMIDIFIER_POLICY_H
#define _HUMIDIFIER_POLICY_H

#include <stdint.h>

// Decides when the humidifier fan runs. Like VentilationPolicy, this is
// deliberately free of ESP-IDF and FreeRTOS so it builds on the host and can be
// tested there. See test/host/test_humidifier_policy.cpp.
//
// The humidifier is a second box inside the chamber: a fan blowing over a tray
// of distilled water with a sanitiser in it, ducted back into the chamber air.
// What that changes about the control problem:
//
//   * Unlike the exchange fan, this actuator really can raise humidity. The
//     exchange fan pulls in room air carrying roughly the chamber's own
//     moisture, so its authority over humidity is marginal -- measured at 0.66
//     RH points for a doubling of fan time. Air leaving a saturated tray is at
//     the tray's temperature and saturated, so every second of run time adds
//     water the chamber did not have.
//   * It is still one-way. Nothing here can dry the chamber; that is the
//     evaporator plate's job and it is not under this firmware's control.
//   * The tray is a standing pool of water in a food chamber. Run time is
//     therefore capped, not because the fan cannot keep up, but because a fan
//     over a tray at high duty holds the chamber near saturation, and the
//     things that grow at 90% RH on a curing surface are not the ones wanted.
//
// Three behaviours are inherited wholesale from the ventilation policy, for
// exactly the reasons documented there:
//
//   * A burst is *raised* on a filtered average and *held* on the raw reading.
//     The compressor swings raw humidity by tens of points every half hour as
//     frost goes onto the plate and comes back off it, and a trigger reading
//     that raw signal fires at the trough of every cycle. The burst's own
//     effect, though, is a step the filter is meant to lag -- so the thing that
//     decides when to stop has to be the instantaneous value. The average is
//     not recomputed here: the caller passes in the one VentilationPolicy
//     already maintains, so both loops act on the same number.
//   * Conditions are re-read every tick rather than latched at the moment a
//     burst is raised. A request that waited out a cold plate for seven minutes
//     is a request made about a chamber that no longer exists.
//   * A burst has a minimum on-time, and the raise and hold thresholds differ,
//     so a condition sitting on its threshold cannot cycle the fan at the loop
//     rate.
//
// One behaviour is deliberately *not* inherited. Ventilation will eventually
// force a burst past a cold plate, because stale air is a real problem and a
// scheduled burst has to happen sometime. Humidification never forces. Blowing
// saturated air onto sub-zero metal is precisely the mechanism that armoured
// the evaporator in ice, and nothing goes wrong if it waits: a chamber that is
// too dry stays too dry, which is the failure mode this whole box exists to
// fix, but it is a slow and reversible one. Frost on the plate is neither.

enum class HumidifierState {
	Idle,      ///< Nothing asked for.
	Pending,   ///< A burst is wanted but the plate is too cold to add moisture.
	Running,   ///< Fan on.
	Settling,  ///< Fan off, waiting for the chamber to mix before re-reading.
};

enum class HumidifierTrigger {
	None,
	Dry,     ///< The averaged humidity fell below the raise band.
	Manual,  ///< Somebody asked for a burst from ThingsBoard.
};

struct HumidifierSettings {
	/// Humidity the humidifier is trying to reach. Separate from the
	/// ventilation policy's humiditySetpoint on purpose: that one decides when
	/// the chamber is dry enough to be worth spending *fresh air* on, and it is
	/// a weak actuator asking a cheap question. This one is the number the
	/// chamber is actually supposed to sit at.
	float targetRh = 78.0f;
	/// How far below the target the average must fall before a burst is raised.
	/// The burst then runs until the raw reading reaches the target, so this is
	/// the whole hysteresis band.
	float raiseBandRh = 3.0f;
	float burstSeconds = 120.0f;
	/// Quiet time after a burst before another may be raised. Wants to be long
	/// enough for the water the burst added to actually reach the sensor --
	/// this is a duct, a chamber volume and a sensor time constant in series,
	/// so it is minutes, not seconds. Too short and the loop stacks bursts on
	/// top of each other chasing a reading that has not caught up yet.
	float settleMinutes = 10.0f;
	/// Plate temperature above which the humidifier may run. Shares the
	/// ventilation gate's attribute because it is the same physical question:
	/// is the coldest surface in the box warm enough that added moisture has
	/// somewhere to go other than straight onto it as frost.
	float plateGateC = 2.0f;
	/// Ceiling on the fraction of a rolling hour the fan may run. Zero or
	/// negative disables the cap.
	float maxDutyPercent = 25.0f;
};

struct HumidifierInputs {
	uint32_t nowMs = 0;          ///< Milliseconds since boot; may wrap.

	bool  humidityValid = false;
	float humidity = 0.0f;       ///< Raw reading. Decides when to stop.

	/// The filtered humidity VentilationPolicy maintains. `averageEnabled`
	/// distinguishes the two ways this can be unavailable: the filter being
	/// switched off, in which case the raw reading is all there is and the
	/// policy falls back to it, and the filter simply not being warm yet, in
	/// which case there is nothing trustworthy to act on and no burst is
	/// raised.
	bool  averageEnabled = true;
	bool  humidityAvgValid = false;
	float humidityAvg = 0.0f;

	bool  plateConfigured = false;
	bool  plateValid = false;
	float plateC = 0.0f;

	/// A person asked for a burst now, edge-detected by the caller. Bypasses
	/// the settle timer, the plate gate and the duty cap -- it is the only way
	/// to exercise the fan on the bench, and second-guessing an explicit
	/// request is not this policy's job.
	bool manualBurstRequest = false;
};

struct HumidifierDecision {
	bool              fanOn = false;
	HumidifierState   state = HumidifierState::Idle;
	HumidifierTrigger trigger = HumidifierTrigger::None;
	/// Static string naming why the fan is where it is. For logs, not control.
	const char* reason = "idle";
};

class HumidifierPolicy {
public:
	HumidifierPolicy() {}

	/// Advance the state machine. Call at a steady cadence; the policy derives
	/// all its timing from `in.nowMs` rather than from how often it is called.
	HumidifierDecision update(const HumidifierInputs& in, const HumidifierSettings& cfg);

	HumidifierState state() const { return state_; }

	/// Percentage of the last hour the fan has run. This is the number the duty
	/// cap is enforced against, and it is worth publishing: a humidifier
	/// sitting on its cap is a humidifier being asked for more than it is
	/// allowed to give, which means either the target is too high for the
	/// chamber or the tray has run dry.
	float dutyPercent() const;

	/// Test seam: nothing in production calls this.
	void reset();

private:
	bool dryRequest(const HumidifierInputs& in, const HumidifierSettings& cfg) const;
	bool dryHolds(const HumidifierInputs& in, const HumidifierSettings& cfg) const;
	bool triggerHolds(const HumidifierInputs& in, const HumidifierSettings& cfg) const;
	bool plateAllows(const HumidifierInputs& in, const HumidifierSettings& cfg) const;
	bool plateHolds(const HumidifierInputs& in, const HumidifierSettings& cfg) const;
	bool dutyAllows(const HumidifierSettings& cfg) const;
	void rollDutyWindow(const HumidifierInputs& in);
	void accrueRunTime(const HumidifierInputs& in);

	HumidifierState   state_ = HumidifierState::Idle;
	HumidifierTrigger trigger_ = HumidifierTrigger::None;

	uint32_t burstStartedMs_ = 0;
	uint32_t burstEndedMs_ = 0;
	bool     everBurst_ = false;
	/// A manual burst overrode the plate gate to start, so the gate does not
	/// get to cut it off afterwards -- that would be the same as refusing it.
	bool     burstForced_ = false;

	bool     bootMsSet_ = false;

	// The rolling duty window: one hour, in five-minute buckets. A bucket
	// rather than a leaky accumulator so the number means something exact --
	// "minutes run in the last hour" -- which is what gets published.
	static const int      DUTY_BUCKETS = 12;
	static const uint32_t DUTY_BUCKET_MS = 5u * 60u * 1000u;
	uint32_t dutyBucketMs_[DUTY_BUCKETS] = {0};
	int      dutyBucket_ = 0;
	uint32_t dutyBucketStartMs_ = 0;

	// Run-time accrual. The policy is told the time, not the interval, so it
	// works out how long the fan was on since the previous call itself.
	uint32_t lastTickMs_ = 0;
	bool     fanWasOn_ = false;
};

#endif // _HUMIDIFIER_POLICY_H

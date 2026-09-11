#ifndef _VENTILATION_POLICY_H
#define _VENTILATION_POLICY_H

#include <stdint.h>

// Decides when the exchange fan runs. Deliberately free of ESP-IDF and
// FreeRTOS so it builds on the host and can be tested there -- this is the part
// with the interesting failure modes, and none of them need hardware to
// reproduce. See test/host/test_ventilation_policy.cpp.
//
// This is a timer, not a controller. Its job is fresh air: a chamber with meat
// hanging in it needs the air changed on a timetable whether or not anything
// else is happening, and how much air it needs is a function of the clock, not
// of any measurement this firmware takes.
//
// It used to be more than that. The fan doubled as the only way to raise
// humidity, so an extra burst could be asked for whenever the chamber read dry,
// and most of the complication here existed to keep that trigger from doing
// harm. The measured authority was never much: doubling the fan's daily run
// time moved the chamber by 0.66 RH points. There is a humidifier fitted now,
// with real authority over humidity and a duty cap of its own, so the exchange
// fan gives the job up entirely. Humidity no longer appears in this policy's
// inputs, its settings, or its triggers.
//
// What survives from that era, because it was never really about humidity:
//
//   * The plate gate. Moist room air meeting sub-zero evaporator metal is what
//     armoured the backplate in ice, and a scheduled burst carries exactly that
//     air. So a burst waits while the plate is cold -- but only up to
//     `maxDeferMinutes`, because stale air is a real problem and a burst owed
//     since this morning has to happen eventually. The humidifier deliberately
//     has no such timeout; it can afford to wait, and this cannot.
//   * A burst is never a commitment. The plate is re-read on every tick, since
//     the thing the gate prevents is just as bad thirty seconds in as it was at
//     the start. A burst that overrode the gate to start is not then cut off by
//     it, which would be the same as never running it.
//   * A minimum on-time under the burst length, so a plate sitting on its
//     threshold cannot cycle the relay at the loop rate.
//
// The state below records *history* -- when the last burst ran, which schedule
// slot has been served, how much fan time the day has had.
//
// The predecessor to all of this was a bang-bang loop that held the fan on
// until humidity reached a setpoint. That could run for hours, and the moisture
// it carried in froze onto the plate faster than the plate could shed it
// between compressor cycles -- which is how the backplate ended up armoured in
// ice in the first place.

enum class VentState {
	Idle,      ///< Nothing due.
	Pending,   ///< A burst is due but the plate is too cold to admit moist air.
	Running,   ///< Fan on.
	Settling,  ///< Fan off, waiting for the chamber to equalise before re-reading.
};

enum class VentTrigger {
	None,
	Scheduled,  ///< The timer came round. This is the one that matters.
	Makeup,     ///< The day's minimum ventilation time was falling behind.
	Manual,     ///< Somebody asked for a burst from ThingsBoard.
};

struct VentilationSettings {
	float intervalHours = 12.0f;     ///< Spacing of scheduled bursts.
	int   firstHourLocal = 6;        ///< Local hour anchoring the schedule.
	float burstSeconds = 90.0f;      ///< How long the fan runs per burst.
	float settleMinutes = 1.5f;      ///< Quiet time after a burst before another may start.
	float plateGateC = 2.0f;         ///< Hold bursts while the plate is colder.
	float maxDeferMinutes = 360.0f;  ///< Give up gating and ventilate anyway.
	/// Floor on total fan time per day. Every burst counts towards it,
	/// whatever triggered it, and the shortfall is topped up with extra bursts
	/// spread across the day rather than saved for a lump at midnight.
	///
	/// At the default this is exactly what the schedule already delivers
	/// (2 x 90 s), so it changes nothing until raised. Raise it when the
	/// chamber needs more air than the interval delivers -- with humidity out
	/// of the picture, this and `intervalHours` are the whole dosage.
	float minSecondsPerDay = 180.0f;
};

struct VentilationInputs {
	uint32_t nowMs = 0;          ///< Milliseconds since boot; may wrap.

	bool    haveClock = false;   ///< True once SNTP has set the system time.
	int64_t localEpoch = 0;      ///< Unix time shifted into local time.

	/// A person asked for a burst now, edge-detected by the caller. Bypasses
	/// the settle timer and the plate gate: an explicit request should do what
	/// it says, and it is the only way to exercise the fan before a probe is
	/// fitted.
	bool manualBurstRequest = false;

	bool  plateConfigured = false; ///< A probe exists in this build.
	bool  plateValid = false;      ///< ...and has produced a recent reading.
	float plateC = 0.0f;
};

struct VentilationDecision {
	bool        fanOn = false;
	VentState   state = VentState::Idle;
	VentTrigger trigger = VentTrigger::None;
	/// Static string naming why the fan is where it is. For logs, not control.
	const char* reason = "idle";
};

class VentilationPolicy {
public:
	VentilationPolicy() {}

	/// Advance the state machine. Call at a steady cadence; the policy derives
	/// all its timing from `in.nowMs` rather than from how often it is called.
	VentilationDecision update(const VentilationInputs& in, const VentilationSettings& cfg);

	VentState state() const { return state_; }
	/// Milliseconds until the next scheduled burst, or 0 when one is due or
	/// running. Only meaningful without a clock; with one, the schedule is
	/// absolute and this is an estimate.
	uint32_t msUntilScheduled(const VentilationInputs& in, const VentilationSettings& cfg) const;
	/// Fan seconds accumulated in the current day window.
	uint32_t secondsToday() const { return dayRunMs_ / 1000; }

	/// Test seam: nothing in production calls this.
	void reset();

private:
	bool scheduledDue(const VentilationInputs& in, const VentilationSettings& cfg);
	bool makeupRequest(const VentilationInputs& in, const VentilationSettings& cfg) const;
	void rollDayWindow(const VentilationInputs& in);
	bool plateAllows(const VentilationInputs& in, const VentilationSettings& cfg) const;

	// The "should this still be happening?" half. plateAllows() decides whether
	// to *start*; this decides whether to *continue*, and is re-read on every
	// tick of a running burst.
	bool plateHolds(const VentilationInputs& in, const VentilationSettings& cfg) const;

	VentState   state_ = VentState::Idle;
	VentTrigger trigger_ = VentTrigger::None;

	uint32_t burstStartedMs_ = 0;
	uint32_t burstEndedMs_ = 0;
	uint32_t pendingSinceMs_ = 0;
	bool     everBurst_ = false;
	/// This burst overrode the plate gate to start -- a manual request, or a
	/// scheduled one deferred past `maxDeferMinutes`. Such a burst is not cut
	/// off by the gate afterwards, because that would undo the override.
	bool     burstForced_ = false;

	// Scheduling. With a clock the schedule is absolute, so the anchor is the
	// slot that last ran; without one it is elapsed time since the last burst.
	bool     haveRunSlot_ = false;
	int64_t  lastRunSlot_ = 0;
	uint32_t bootMs_ = 0;
	bool     bootMsSet_ = false;

	// The day window. With a clock this is the local calendar day, so the
	// counters reset at local midnight; without one it is a rolling 24 hours
	// from boot. The daily minimum hangs off it.
	uint32_t dayRunMs_ = 0;
	uint32_t dayWindowStartMs_ = 0;
	bool     dayWindowSet_ = false;
	int64_t  dayIndex_ = 0;
	bool     dayIndexSet_ = false;
	/// How far into the day the current window opened. Non-zero only for the
	/// partial day a boot lands in.
	float    dayStartFraction_ = 0.0f;
};

#endif // _VENTILATION_POLICY_H

#ifndef _VENTILATION_POLICY_H
#define _VENTILATION_POLICY_H

#include <stdint.h>

// Decides when the exchange fan runs. Deliberately free of ESP-IDF and
// FreeRTOS so it builds on the host and can be tested there -- this is the part
// with the interesting failure modes, and none of them need hardware to
// reproduce. See test/host/test_ventilation_policy.cpp.
//
// The model this encodes:
//
//   * The fan is a one-way actuator with weak authority. It pulls room air into
//     the chamber, which at a 10 C setpoint carries roughly the same absolute
//     moisture as the chamber air -- sometimes a little more, in a dry house a
//     little less. So the fan can nudge humidity up, and cannot usefully bring
//     it down.
//   * The real dehumidifier is the evaporator plate, which is far stronger than
//     the fan and not under this firmware's control at all.
//   * The meat is the real humidifier: a drying subprimal gives up on the order
//     of tens of grams of water a day, against roughly one gram per air change.
//
// Which is why this is not a setpoint controller. Ventilation happens on a
// schedule, because its actual job is fresh air; humidity may ask for extra
// bursts when the chamber is dry, bounded by burst-and-settle; and nothing here
// ever tries to dry the chamber out, because the fan cannot.
//
// The state below records *history* -- when the last burst ran, which schedule
// slot has been served, how much fan time the day has had. It deliberately does
// not record *intent*: a burst that is owed, or one already running, re-reads
// the plate and the humidity on every tick and stops as soon as its reasons
// stop holding. An earlier version latched the decision at the moment a burst
// was raised, which produced two failures that looked unrelated and were not.
// A humidity burst asked for at 63% RH sat waiting on a cold plate for seven
// minutes and then fired into a chamber that had climbed to 73%; and a burst
// that started the instant the plate touched the gate kept running for another
// eighty-five seconds while the plate dived to -2.6 C, which is precisely the
// moist-air-onto-cold-metal event the gate exists to prevent.
//
// Re-reading a condition every tick invites the opposite failure, where a
// condition sitting on its threshold cycles the fan at the loop rate. Three
// things stop that: humidity bursts are raised at `setpoint - undershoot` but
// held until `setpoint`, the plate must fall a little below the gate before it
// cuts a burst, and no burst may be stopped before a minimum on-time.
//
// The predecessor was a bang-bang loop that held the fan on until humidity
// reached a setpoint. That could run for hours, and the moisture it carried in
// froze onto the plate faster than the plate could shed it between compressor
// cycles -- which is how the backplate ended up armoured in ice.

enum class VentState {
	Idle,      ///< Nothing due.
	Pending,   ///< A burst is due but the plate is too cold to admit moist air.
	Running,   ///< Fan on.
	Settling,  ///< Fan off, waiting for the chamber to equalise before re-reading.
};

enum class VentTrigger {
	None,
	Scheduled,  ///< The timer came round. This is the one that matters.
	Dry,        ///< Humidity below the setpoint band asked for an extra burst.
	Makeup,     ///< The day's minimum ventilation time was falling behind.
	Manual,     ///< Somebody asked for a burst from ThingsBoard.
};

struct VentilationSettings {
	float intervalHours = 12.0f;     ///< Spacing of scheduled bursts.
	int   firstHourLocal = 6;        ///< Local hour anchoring the schedule.
	float burstSeconds = 90.0f;      ///< How long the fan runs per burst.
	float settleMinutes = 1.5f;      ///< Quiet time before humidity may ask again.
	float plateGateC = 2.0f;         ///< Hold bursts while the plate is colder.
	float maxDeferMinutes = 360.0f;  ///< Give up gating and ventilate anyway.
	/// Floor on total fan time per day. Every burst counts towards it,
	/// whatever triggered it, and the shortfall is topped up with extra bursts
	/// spread across the day rather than saved for a lump at midnight.
	///
	/// At the default this is exactly what the schedule already delivers
	/// (2 x 90 s), so it changes nothing until raised. Raise it when the
	/// chamber needs more air than humidity happens to ask for.
	float minSecondsPerDay = 180.0f;
	float humiditySetpoint = 75.0f;
	float humidityUndershoot = 5.0f;
};

struct VentilationInputs {
	uint32_t nowMs = 0;          ///< Milliseconds since boot; may wrap.

	bool    haveClock = false;   ///< True once SNTP has set the system time.
	int64_t localEpoch = 0;      ///< Unix time shifted into local time.

	bool  humidityValid = false;
	float humidity = 0.0f;

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
	bool dryRequest(const VentilationInputs& in, const VentilationSettings& cfg) const;
	bool makeupRequest(const VentilationInputs& in, const VentilationSettings& cfg) const;
	void rollDayWindow(const VentilationInputs& in);
	bool plateAllows(const VentilationInputs& in, const VentilationSettings& cfg) const;

	// The "should this still be happening?" half. plateAllows()/dryRequest()
	// decide whether to *start*; these decide whether to *continue*, and are
	// re-read on every tick of a pending or running burst.
	bool dryHolds(const VentilationInputs& in, const VentilationSettings& cfg) const;
	bool triggerHolds(const VentilationInputs& in, const VentilationSettings& cfg) const;
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
	// from boot. Both the dry-burst cap and the daily minimum hang off it.
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

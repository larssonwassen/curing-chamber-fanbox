#include "HumidityAverage.h"

namespace {

/// Elapsed milliseconds, correct across the ~49-day wrap of a 32-bit tick
/// counter. Unsigned subtraction does the right thing; the cast is the point.
uint32_t elapsed_ms(uint32_t now, uint32_t then) {
	return (uint32_t)(now - then);
}

} // namespace

void HumidityAverage::reset() {
	*this = HumidityAverage();
}

void HumidityAverage::update(uint32_t nowMs, bool humidityValid, float humidity, float tauMinutes) {
	if (!humidityValid) {
		// Hold the last average rather than decaying it towards nothing. A
		// dropout should freeze the filter, not slowly invent a dry chamber.
		// Readiness is frozen with it: a filter that stopped being fed is not
		// getting any more trustworthy while it waits.
		return;
	}

	const float tauMs = tauMinutes > 0.0f ? tauMinutes * 60000.0f : 0.0f;

	if (!valid_) {
		valid_ = true;
		avg_ = humidity;
		lastMs_ = nowMs;
		seedMs_ = nowMs;
		// With the filter disabled the average is the reading, so there is
		// nothing to warm up.
		ready_ = (tauMs == 0.0f);
		return;
	}

	float alpha = 1.0f;
	if (tauMs > 0.0f) {
		alpha = (float)elapsed_ms(nowMs, lastMs_) / tauMs;
		if (alpha > 1.0f) {
			// A long gap -- a stall, or the first update after a wrap. Adopt
			// the current reading instead of extrapolating a filter that has
			// no information about what happened in between.
			alpha = 1.0f;
		}
	}
	avg_ += (humidity - avg_) * alpha;
	lastMs_ = nowMs;
	ready_ = (tauMs == 0.0f) || (elapsed_ms(nowMs, seedMs_) >= (uint32_t)tauMs);
}

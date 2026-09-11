#ifndef _HUMIDITY_AVERAGE_H
#define _HUMIDITY_AVERAGE_H

#include <stdint.h>

// A first-order filter over chamber humidity, long enough to span a compressor
// cycle. Free of ESP-IDF and FreeRTOS so it builds on the host; see
// test/host/test_humidity_average.cpp.
//
// Why this exists at all: the real dehumidifier in this chamber is the
// evaporator plate, and it is not under this firmware's control. Over a
// compressor cycle an instantaneous humidity reading mostly reports where the
// compressor is, not how wet the chamber is. Measured on 2026-09-09 over a
// clean 47-minute cycle with the fan idle, the chamber's water content swung
// 3.38 to 6.60 g/kg -- 49% of its own peak -- as frost went onto the plate and
// came back off it. At the cycle's mean temperature that is 49 points of
// relative humidity, netted down to the 27 points actually observed only
// because air temperature swings in phase and pulls the other way.
//
// So anything deciding "is this chamber dry?" has to read a filtered value.
// Anything deciding "has this burst delivered enough yet?" must not, because a
// burst's own effect is exactly the step this filter is built to lag.
//
// This lived inside VentilationPolicy until the exchange fan stopped reacting
// to humidity at all. It now sits with the sensor that produces the raw signal,
// which is where the humidifier -- its only remaining consumer -- reads it.
class HumidityAverage {
public:
	/// Advance the filter. Derives its own timing from `nowMs` rather than
	/// from how often it is called, so it survives a missed update. A
	/// `tauMinutes` of zero disables the filter, and the average tracks the
	/// reading exactly.
	void update(uint32_t nowMs, bool humidityValid, float humidity, float tauMinutes);

	/// True once any reading has seeded the filter. The value is meaningful
	/// but young: it is still mostly the single sample it was seeded from.
	bool valid() const { return valid_; }

	/// True once the filter has been running for a full time constant. Until
	/// then the average is dominated by its seed, and seeding at the trough of
	/// a compressor cycle looks exactly like a dry chamber -- so a trigger
	/// that acts on dryness should wait for this rather than for valid().
	bool ready() const { return ready_; }

	float value() const { return avg_; }

	void reset();

private:
	float    avg_ = 0.0f;
	bool     valid_ = false;
	bool     ready_ = false;
	uint32_t lastMs_ = 0;
	uint32_t seedMs_ = 0;
};

#endif // _HUMIDITY_AVERAGE_H

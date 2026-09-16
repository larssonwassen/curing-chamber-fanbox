// Host tests for the humidity filter, built with ASan/UBSan.
//
// This is the number the humidifier decides dryness on. A raw reading in this
// chamber mostly reports where the compressor is: measured 2026-09-09 over a
// clean 47-minute cycle with the fan idle, water content swung 3.38 to
// 6.60 g/kg -- 49% of its own peak -- purely from frost cycling on and off the
// evaporator plate, which is 27 points of relative humidity that say nothing
// about how much water the chamber holds. A trigger reading that signal fires
// at the trough of every cycle, which is also when the plate is coldest and the
// gate shut.
//
// So the filter has two jobs, and both are tested here: average the swing away,
// and refuse to be trusted until it has actually seen a cycle.

#include "climate/HumidityAverage.h"

#include <stdio.h>

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

const float TAU_MIN = 30.0f;

/// Humidity as this chamber actually behaves: a large swing at the compressor
/// period, as frost goes onto the plate and comes back off it. Triangular
/// rather than sinusoidal so the test needs no math header -- the shape is not
/// what matters here, the amplitude is.
float cycling(uint32_t tMs, float mean, float amp, uint32_t periodMs) {
	const uint32_t half = periodMs / 2;
	const uint32_t phase = tMs % periodMs;
	const float up = phase < half
		? (float)phase / (float)half
		: 1.0f - (float)(phase - half) / (float)half;
	return (mean - amp) + 2.0f * amp * up;
}

bool near(float a, float b, float tol) {
	const float d = a - b;
	return (d < 0 ? -d : d) <= tol;
}

} // namespace

int main(void) {
	{
		HumidityAverage f;
		check(!f.valid(), "an untouched filter has nothing to say");
		check(!f.ready(), "...and is certainly not ready");
	}

	{
		// Seeded from a single reading, the average is that reading -- which is
		// exactly the trap: boot at the trough of a cycle and an unguarded
		// trigger fires at once.
		HumidityAverage f;
		f.update(0, true, 61.0f, TAU_MIN);
		check(f.valid(), "one reading seeds the filter");
		check(near(f.value(), 61.0f, 0.001f), "...and the average is that reading");
		check(!f.ready(), "...but it is not ready until a time constant has passed");
	}

	{
		HumidityAverage f;
		for (uint32_t t = 0; t <= 30 * MIN; t += SEC) {
			f.update(t, true, 61.0f, TAU_MIN);
		}
		check(f.ready(), "a full time constant of readings makes it ready");
	}

	{
		// The whole point. A chamber sitting on 75% swinging the measured 27
		// points peak-to-peak must average to 75%, not to its troughs.
		HumidityAverage f;
		float lowest = 200.0f, highest = -200.0f;
		for (uint32_t t = 0; t <= 6 * 60 * MIN; t += SEC) {
			const float rh = cycling(t, 75.0f, 13.5f, 45 * MIN);
			f.update(t, true, rh, TAU_MIN);
			if (t >= 60 * MIN) { // once warmed up
				if (f.value() < lowest)  { lowest = f.value(); }
				if (f.value() > highest) { highest = f.value(); }
			}
		}
		check(near(f.value(), 75.0f, 1.5f), "a compressor swing averages to its mean");
		// 27 points in, 6.1 out with this triangular wave: the filter keeps
		// about a fifth of a swing a time constant and a half long.
		check(highest - lowest <= 8.0f,
			  "...and what is left of the swing is a fifth of the 27 points in");
	}

	{
		// A dropout freezes the filter rather than decaying it towards nothing:
		// a sensor that stopped answering is not evidence of a drying chamber.
		HumidityAverage f;
		for (uint32_t t = 0; t <= 30 * MIN; t += SEC) {
			f.update(t, true, 70.0f, TAU_MIN);
		}
		const float held = f.value();
		for (uint32_t t = 30 * MIN; t <= 90 * MIN; t += SEC) {
			f.update(t, false, 0.0f, TAU_MIN);
		}
		check(near(f.value(), held, 0.001f), "a dropout holds the average where it was");
		check(f.ready(), "...and does not un-ready it");
	}

	{
		// A gap longer than the time constant carries no information about what
		// happened in between, so the filter adopts the reading rather than
		// extrapolating.
		HumidityAverage f;
		f.update(0, true, 40.0f, TAU_MIN);
		f.update(4 * 60 * MIN, true, 80.0f, TAU_MIN);
		check(near(f.value(), 80.0f, 0.001f), "a long gap adopts the current reading");
	}

	{
		// Zero disables the filter: the average is the reading, and there is
		// nothing to warm up.
		HumidityAverage f;
		f.update(0, true, 61.0f, 0.0f);
		check(near(f.value(), 61.0f, 0.001f), "a zero time constant tracks the reading");
		check(f.ready(), "...and is ready immediately");
		f.update(SEC, true, 44.0f, 0.0f);
		check(near(f.value(), 44.0f, 0.001f), "...on every sample");
	}

	{
		// The tick counter wraps every ~49 days. A chamber runs for months, and
		// a filter that mistook a wrap for a four-billion-millisecond gap would
		// throw its history away in the middle of a cure.
		HumidityAverage f;
		const uint32_t start = 0xFFFFF000u;
		uint32_t t = start;
		for (int i = 0; i <= 40 * 60; i++) { // 40 minutes at 1 Hz, straight over the wrap
			f.update(t, true, 70.0f, TAU_MIN);
			t += SEC;
		}
		check(f.ready(), "readiness survives the 32-bit millisecond wrap");
		check(near(f.value(), 70.0f, 0.001f), "...and so does the average");
	}

	printf("%s\n", failures == 0 ? "all ok" : "FAILURES");
	return failures == 0 ? 0 : 1;
}

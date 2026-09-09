#ifndef _PLATE_PROBE_H
#define _PLATE_PROBE_H

// Evaporator-plate temperature.
//
// The plate is the coldest surface in the chamber and the only dehumidifier in
// it: whatever moisture the meat gives up, and whatever the fan brings in,
// leaves the air by freezing onto that plate. Knowing its temperature turns two
// blind spots into measurements --
//
//   * whether it is safe to ventilate right now (below freezing, incoming
//     moisture accretes as frost rather than condensing and draining), and
//   * whether frost is winning over the long run, which shows up as the plate
//     running progressively colder while the chamber runs warmer.
//
// The probe is optional. With CONFIG_PLATE_PROBE_NONE the firmware behaves as
// it did before one was fitted: no gating, no plate telemetry.

namespace PlateProbe {

/// Start sampling. Safe to call when no probe is configured -- it does nothing.
void begin(void);

/// True when this build has a probe compiled in, whether or not it is reading.
bool isConfigured(void);

/// True when a reading has arrived recently enough to act on.
bool isFresh(void);

/// Last successful reading in degrees Celsius. Meaningless unless isFresh().
float temperature(void);

/// Name of the configured probe, for logs and telemetry.
const char* description(void);

} // namespace PlateProbe

#endif // _PLATE_PROBE_H

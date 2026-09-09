#ifndef _VENTILATION_H
#define _VENTILATION_H

#include "VentilationPolicy.h"

// Drives the exchange fan from VentilationPolicy.
//
// This half is the part that touches hardware and shared state: it samples the
// sensors, reads the shared attributes, switches the fan relay, and publishes
// what it did. All of the decision-making lives in VentilationPolicy, which is
// tested on the host.

namespace Ventilation {

/// Start the control task. Sensors and the fan GPIO must already be set up.
void begin(void);

/// Human-readable name for a state, for logs and telemetry.
const char* stateName(VentState s);

} // namespace Ventilation

#endif // _VENTILATION_H

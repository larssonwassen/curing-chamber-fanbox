#ifndef _HUMIDIFIER_H
#define _HUMIDIFIER_H

#include "HumidifierPolicy.h"

// Drives the humidifier fan from HumidifierPolicy.
//
// This half is the part that touches hardware and shared state: it switches the
// high-side FET, counts the fan's tachometer, reads the shared attributes and
// publishes what it did. All of the decision-making lives in HumidifierPolicy,
// which is tested on the host.
//
// The hardware is a separate board in the cabinet: a 2N7000 inverting into a
// high-side IRF4905 that switches the fan's 12 V. High-side rather than
// low-side for two reasons -- the fan sits over a tray of water, so the wiring
// should be dead rather than live when it is off, and the tach transistor is
// referenced to the fan's ground pin, which a low-side switch would leave
// floating and the tach unreadable.

namespace Humidifier {

/// Start the control task. Call after the GPIOs are configured in app_main.
void begin(void);

/// Human-readable name for a state, for logs and telemetry.
const char* stateName(HumidifierState s);

} // namespace Humidifier

#endif // _HUMIDIFIER_H

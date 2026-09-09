#ifndef _POTENTIOMETER_H
#define _POTENTIOMETER_H

// The panel potentiometer that sets fan speed.
//
// Reads ADC1 channel 0 (GPIO 1, A0 on the silkscreen) and publishes the
// result as telemetry->fanDuty, which fan_task turns into an EMC2101 duty
// cycle. The knob is the only speed control: ventilation decides *when* the
// fan runs, this decides *how hard*.

namespace Potentiometer {

/// Start sampling.
void begin(void);

} // namespace Potentiometer

#endif // _POTENTIOMETER_H

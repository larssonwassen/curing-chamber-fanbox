#ifndef _CLIMATE_SENSOR_H
#define _CLIMATE_SENSOR_H

// Chamber air temperature and humidity, from the SHT31 on the I2C bus.
//
// Lifted out of main.cpp so that the ventilation controller can ask about
// staleness without reaching into a translation-unit-local static, and so that
// the plate probe and this sensor read the same way.

namespace ClimateSensor {

/// Start sampling. The I2C bus must already be up.
void begin(void);

/// True when a reading has arrived recently enough to act on. False until the
/// first successful read, because 0.0 %% is not a measurement.
bool isFresh(void);

float temperature(void);
float humidity(void);

} // namespace ClimateSensor

#endif // _CLIMATE_SENSOR_H

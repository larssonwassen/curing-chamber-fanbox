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

/// Chamber humidity filtered over a compressor cycle, with its time constant
/// taken from the `humidity_average_minutes` shared attribute. Anything asking
/// "is this chamber dry?" wants this rather than humidity(); see
/// HumidityAverage.h for why.
float humidityAverage(void);

/// True once any reading has seeded the average. It is meaningful from here
/// on, which is when it starts being published as telemetry.
bool humidityAverageValid(void);

/// True once the average has been running for a full time constant. A control
/// decision about dryness should wait for this, not merely for valid.
bool humidityAverageReady(void);

} // namespace ClimateSensor

#endif // _CLIMATE_SENSOR_H

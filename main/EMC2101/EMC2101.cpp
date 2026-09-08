#include "EMC2101.h"
#include "esp_log.h"

static const char *TAG = "emc2101";

static long map(long x, long in_min, long in_max, long out_min, long out_max) {
	const long run = in_max - in_min;
	if (run == 0) {
		ESP_LOGE(TAG, "map(): Invalid input range, min == max");
		return -1; // AVR returns -1, SAM returns 0
	}
	const long rise = out_max - out_min;
	const long delta = x - in_min;
	return (delta * rise) / run + out_min;
}

/**
 * @brief Construct a new EMC2101::EMC2101 object
 *
 */
EMC2101::EMC2101(void) {}

/**
 * @brief Destroy the EMC2101::EMC2101 object
 *
 */
EMC2101::~EMC2101(void) {
	end();
}

/*!
 *    @brief  Sets up the hardware and initializes I2C
 *    @param  i2c_address
 *            The I2C address to be used.
 *    @param  wire
 *            The Wire object to be used for I2C connections.
 *    @return True if initialization was successful, otherwise false.
 */
bool EMC2101::begin(uint8_t i2c_address, I2CBus *bus) {
	if (i2c_dev) {
		delete i2c_dev; // remove old interface
	}

	i2c_dev = new I2CDevice(bus, i2c_address);
	if (!i2c_dev) {
		ESP_LOGE(TAG, "Failed to allocate memory for I2CDevice");
		return false;
	}

	if (!i2c_dev->begin()) {
		ESP_LOGE(TAG, "Device begin failed.");
		delete i2c_dev;
		i2c_dev = nullptr;
		return false;
	}

	if (!_init()) {
		end();
		return false;
	}
	return true;
}

void EMC2101::end() {
	if (i2c_dev) {
		i2c_dev->end();
		delete i2c_dev;
		i2c_dev = nullptr;
	}
}

/*!  @brief Initializer for post i2c/spi init
 *   @returns True if chip identified and initialized
 */
bool EMC2101::_init(void) {

	BusIO_Register chip_id = BusIO_Register(i2c_dev, EMC2101_WHOAMI, 1);

	uint32_t val = chip_id.read();
	if (val == (uint32_t)-1) {
		ESP_LOGE(TAG, "Failed to read chip ID");
		return false;
	}
	uint8_t id = val & 0xFF;
	if (id != EMC2101_CHIP_ID && id != EMC2101_ALT_CHIP_ID) {
		// Continue anyway. The part on this board reports 0x17, which Microchip
		// documents for neither the EMC2101 (0x16) nor the EMC2101-R (0x28), yet
		// it drives the fan and returns tachometer readings correctly. Refusing
		// to initialise over an unrecognised ID would break a working device;
		// saying nothing would hide a genuinely wrong part.
		ESP_LOGW(TAG, "Unrecognised chip ID 0x%02X (expected 0x%02X or 0x%02X); "
					  "continuing on the assumption the register map matches",
				 id, EMC2101_CHIP_ID, EMC2101_ALT_CHIP_ID);
	}

	enableTachInput(true);
	invertFanSpeed(false);
	setPWMFrequency(0x1F);
	configPWMClock(1, 0);
	DACOutEnabled(false); // output PWM mode by default
	LUTEnabled(false);
	setDutyCycle(100);

	enableForcedTemperature(false);

	// Set to highest rate
	setDataRate(EMC2101_RATE_32_HZ);

	return true;
}

/**
 * @brief Enable using the TACH/ALERT pin as an input to read the fan speed
 * signal from a 4-pin fan
 *
 * @param tach_enable true: to enable tach signal input, false to disable and
 * use the tach pin as interrupt & status output
 * @return true: sucess false: failure
 */
bool EMC2101::enableTachInput(bool tach_enable) {
	if (i2c_dev == nullptr) {
		ESP_LOGW(TAG, "I2C device not begun");
		return false;
	}
	BusIO_Register reg_config = BusIO_Register(i2c_dev, EMC2101_REG_CONFIG);
	BusIO_RegisterBits tach_mode_enable_bit = BusIO_RegisterBits(&reg_config, 1, 2);
	return tach_mode_enable_bit.write(tach_enable);
}

/**
 * @brief Set the cotroller to interperate fan speed settings opposite of the
 * normal behavior
 *
 * @param invert_speed If true, fan duty cycle / DAC value settings will work
 * backwards; Setting the highest value (100) will set the fan to it's lowest
 * PWM value, and setting the fan to the lowest value (0) will set the fan
 * output to it's highest setting.
 * @return true:sucess false:failure
 */
bool EMC2101::invertFanSpeed(bool invert_speed) {
	if (i2c_dev == nullptr) {
		ESP_LOGW(TAG, "I2C device not begun");
		return false;
	}
	BusIO_Register fan_config = BusIO_Register(i2c_dev, EMC2101_FAN_CONFIG);
	BusIO_RegisterBits invert_fan_output_bit = BusIO_RegisterBits(&fan_config, 1, 4);
	return invert_fan_output_bit.write(invert_speed);
}

/**
 * @brief Configure the PWM clock by selecting the clock source and overflow bit
 *
 * @param clksel The clock select true: Use a 1.4kHz base PWM clock
 * false: Use the default 360kHz PWM clock
 *
 * @param clkovr Clock override
 * When true, override the base clock selected by `clksel` and use the frequency
 * divisor to set the PWM frequency
 *
 * @return true:success false:failure
 */
bool EMC2101::configPWMClock(bool clksel, bool clkovr) {
	if (i2c_dev == nullptr) {
		ESP_LOGW(TAG, "I2C device not begun");
		return false;
	}
	BusIO_Register fan_config = BusIO_Register(i2c_dev, EMC2101_FAN_CONFIG);
	BusIO_RegisterBits clksel_bit = BusIO_RegisterBits(&fan_config, 1, 3);
	clksel_bit.write(clksel);
	BusIO_RegisterBits clkovr_bit = BusIO_RegisterBits(&fan_config, 1, 2);
	return clkovr_bit.write(clkovr);
}

/**
 * @brief Configure the fan's spinup behavior when transitioning from
 * off/minimal speed to a higher speed (except on power up)
 *
 * @param spinup_drive The duty cycle to drive the fan with during spin up
 * **defaults to 100%**
 *
 * @param spinup_time The amount of time to keep the fan at the given drive
 * setting. **Defaults to 3.2 seconds**
 *
 * @return true:success false: failure
 */
bool EMC2101::configFanSpinup(uint8_t spinup_drive, uint8_t spinup_time) {
	if (i2c_dev == nullptr) {
		ESP_LOGW(TAG, "I2C device not begun");
		return false;
	}
	BusIO_Register spin_config = BusIO_Register(i2c_dev, EMC2101_FAN_SPINUP);
	BusIO_RegisterBits _spin_drive_bits = BusIO_RegisterBits(&spin_config, 2, 3);
	if (!_spin_drive_bits.write(spinup_drive)) {
		return false;
	}
	BusIO_RegisterBits _spin_time_bits = BusIO_RegisterBits(&spin_config, 3, 0);
	return _spin_time_bits.write(spinup_time);
}

/**
 * @brief Configure the fan's spinup behavior when transitioning from
 * off/minimal speed to a higher speed (except on power up)
 *
 * @param tach_spinup If true, drive the fan at 100% until the speed is above
 * the speed set with `setFanMinRPM`. If previously set, `spinup_drive` and
 * `spinup_time` are ignored
 *
 * @return true:success false: failure
 */
bool EMC2101::configFanSpinup(bool tach_spinup) {
	if (i2c_dev == nullptr) {
		ESP_LOGW(TAG, "I2C device not begun");
		return false;
	}
	// This should be settable by the constructor
	BusIO_Register spin_config = BusIO_Register(i2c_dev, EMC2101_FAN_SPINUP);
	BusIO_RegisterBits tach_spinup_en = BusIO_RegisterBits(&spin_config, 1, 5);
	return tach_spinup_en.write(tach_spinup);
}

/**
 * @brief Get the amount of hysteresis in Degrees celcius of hysteresis applied
 * to temperature readings used for the LUT. As the temperature drops, the
 * controller will switch to a lower LUT entry when the measured value is
 * belowthe lower entry's threshold, minus the hysteresis value
 *
 * @return uint8_t The current LUT hysteresis value
 */
uint8_t EMC2101::getLUTHysteresis(void) {
	if (i2c_dev == nullptr) {
		ESP_LOGW(TAG, "I2C device not begun");
		return 0;
	}
	BusIO_Register lut_temperature_hysteresis = BusIO_Register(i2c_dev, EMC2101_LUT_HYSTERESIS);
	return lut_temperature_hysteresis.read();
}

/**
 * @brief Set the amount of hysteresis in degrees celcius of hysteresis applied
 * to temperature readings used for the LUT.
 *
 * @param hysteresis  The hysteresis value in degrees celcius. As the
 * temperature drops, the controller will switch to a lower LUT entry when the
 * measured value is `hystersis` degrees below the lower entry's temperature
 * threshold
 *
 * @return bool true:success false:failure
 */
bool EMC2101::setLUTHysteresis(uint8_t hysteresis) {
	if (i2c_dev == nullptr) {
		ESP_LOGW(TAG, "I2C device not begun");
		return false;
	}
	BusIO_Register lut_temperature_hysteresis = BusIO_Register(i2c_dev, EMC2101_LUT_HYSTERESIS);
	return lut_temperature_hysteresis.write(hysteresis);
}

/**
 * @brief Create a new mapping between temperature and fan speed in the Look Up
 * Table. Requires the LUT to be enabled with `LUTEnabled(true)`
 *
 * @param index The index in the LUT, from 0-7. Note that the temperature
 * thresholds should increase with the LUT index, so the temperature value for a
 * given LUT entry is higher than the temperature for the previous index:
 * @code
 * // NO!:
 * setLUT(0, 30, 25); // 25% PWM @ 30 Degrees C
 * setLUT(1, 20, 10); // WRONG! 10% PWM @ 20 Degrees C should be before 30
 * // degrees from index 0 setLUT(2, 40, 50); // 50% PWM @ 40 Degrees C
 *
 * // YES:
 * setLUT(0, 20, 10); // 10% PWM @ 20 Degrees C
 * setLUT(1, 30, 25); // 25% PWM @ 30 Degrees C
 * setLUT(2, 40, 50); // 50% PWM @ 40 Degrees C
 *@endcode
 *
 * @param temp_thresh When the temperature is more than this threshold, the fan
 * will be set to the given PWM value
 * @param fan_pwm The pwm-based fan speed for the given temperature threshold.
 * When DAC output is enabled, this determins the percentage of the maximum
 * output voltage to be used for the given temperature threshold
 * @return true:success false:failure
 */
bool EMC2101::setLUT(uint8_t index, uint8_t temp_thresh, uint8_t fan_pwm) {
	if (i2c_dev == nullptr) {
		ESP_LOGW(TAG, "I2C device not begun");
		return false;
	}
	if (index > 7) {
		return false;
	}
	if (temp_thresh > MAX_LUT_TEMP) {
		return false;
	}
	if (fan_pwm > 100) {
		return false;
	}

	uint8_t temp_reg_addr = EMC2101_LUT_START + (2 * index); // speed/pwm is +1
	BusIO_Register lut_temp = BusIO_Register(i2c_dev, temp_reg_addr);
	BusIO_Register lut_pwm = BusIO_Register(i2c_dev, temp_reg_addr + 1);

	float scalar = (float)fan_pwm / 100.0;
	uint8_t scaled_pwm = (uint8_t)(scalar * MAX_LUT_SPEED);

	bool lut_enabled = LUTEnabled();
	LUTEnabled(false);
	if (!lut_temp.write(temp_thresh)) {
		return false;
	}

	if (!lut_pwm.write(scaled_pwm)) {
		return false;
	}
	LUTEnabled(lut_enabled);

	return true;
}

/**
 * @brief Get the fan speed setting used while the LUT is being updated and is
 * unavailable or not in use. The speed is  given as the fan's PWM duty cycle
 * represented as a float percentage. The value **roughly** approximates the
 * percentage of the fan's maximum speed"""
 *
 * @return uint8_t The current manually set fan duty cycle
 */
uint8_t EMC2101::getDutyCycle(void) {
	if (i2c_dev == nullptr) {
		ESP_LOGW(TAG, "I2C device not begun");
		return 0;
	}
	BusIO_Register _fan_setting = BusIO_Register(i2c_dev, EMC2101_REG_FAN_SETTING);
	uint8_t raw_duty_cycle = _fan_setting.read() & MAX_LUT_SPEED;
	return (uint8_t)((raw_duty_cycle / (float)MAX_LUT_SPEED) * 100);
}

/**
 * @brief Set the fan speed.
 *
 *
 * @param pwm_duty_cycle The  duty cycle percentage as an integer
 * The speed is  given as the fan's PWM duty cycle and **roughly** approximates
 * the percentage of the fan's maximum speed
 * @return true: success false: failure
 */
bool EMC2101::setDutyCycle(uint8_t pwm_duty_cycle) {
	if (i2c_dev == nullptr) {
		ESP_LOGW(TAG, "I2C device not begun");
		return false;
	}
	BusIO_Register _fan_setting = BusIO_Register(i2c_dev, EMC2101_REG_FAN_SETTING);

	// convert from a percentage to that percentage of the max duty cycle
	pwm_duty_cycle = map(pwm_duty_cycle, 0, 100, 0, 63);

	bool lut_enabled = LUTEnabled();
	LUTEnabled(false);
	if (!_fan_setting.write(pwm_duty_cycle)) {
		return false;
	}
	return LUTEnabled(lut_enabled);
}

/**
 * @brief Get the LUT enable status
 *
 * @return true: LUT usage enabled false: LUT disabled
 */
bool EMC2101::LUTEnabled(void) {
	if (i2c_dev == nullptr) {
		ESP_LOGW(TAG, "I2C device not begun");
		return false;
	}
	BusIO_Register fan_config = BusIO_Register(i2c_dev, EMC2101_FAN_CONFIG);
	BusIO_RegisterBits _lut_disable_bit = BusIO_RegisterBits(&fan_config, 1, 5);
	return !_lut_disable_bit.read();
}

/**
 * @brief Enable or disable the temperature-to-fan speed Look Up Table (LUT)
 *
 * @param enable_lut True to enable the LUT, setting the fan speed depending on
 * the configured temp to speed mapping. False disables the LUT, defaulting to
 * the speed setting from `setDutyCycle`
 * @return true:success false: failure
 */
bool EMC2101::LUTEnabled(bool enable_lut) {
	if (i2c_dev == nullptr) {
		ESP_LOGW(TAG, "I2C device not begun");
		return false;
	}
	BusIO_Register fan_config = BusIO_Register(i2c_dev, EMC2101_FAN_CONFIG);
	BusIO_RegisterBits _lut_disable_bit = BusIO_RegisterBits(&fan_config, 1, 5);
	return _lut_disable_bit.write(!enable_lut);
}

/**
 * @brief Get the mimnum RPM setting for the attached fan
 *
 * @return uint16_t the current minimum RPM setting
 */
uint16_t EMC2101::getFanMinRPM(void) {
	if (i2c_dev == nullptr) {
		ESP_LOGW(TAG, "I2C device not begun");
		return 0;
	}
	uint8_t buffer[2];
	BusIO_Register tach_limit_lsb = BusIO_Register(i2c_dev, EMC2101_TACH_LIMIT_LSB);
	BusIO_Register tach_limit_msb = BusIO_Register(i2c_dev, EMC2101_TACH_LIMIT_MSB);
	// Check the reads: on failure `buffer` keeps whatever was on the stack, and
	// that garbage was previously divided into.
	if (!tach_limit_msb.read(buffer) || !tach_limit_lsb.read(buffer + 1)) {
		ESP_LOGW(TAG, "Failed to read tach limit registers");
		return 0;
	}

	uint16_t raw_limit = buffer[0] << 8;
	raw_limit |= buffer[1];
	// 0xFFFF is the "no limit" encoding; 0 would divide by zero.
	if (raw_limit == 0xFFFF || raw_limit == 0) {
		return 0;
	}
	return EMC2101_FAN_RPM_NUMERATOR / raw_limit;
}

/**
 * @brief Set the minimum speed of the attached fan
 *
 * Used to determine the fan state
 *
 * @param min_rpm The minimum speed of the fan. Any setting below this will
 * return 0 and mark the fan as non-operational
 * @return true: success false: failure
 */
bool EMC2101::setFanMinRPM(uint16_t min_rpm) {
	if (i2c_dev == nullptr) {
		ESP_LOGW(TAG, "I2C device not begun");
		return false;
	}
	BusIO_Register tach_limit_lsb = BusIO_Register(i2c_dev, EMC2101_TACH_LIMIT_LSB);
	BusIO_Register tach_limit_msb = BusIO_Register(i2c_dev, EMC2101_TACH_LIMIT_MSB);
	// speed is given in RPM, convert to raw value (MSB+LSB):
	if (min_rpm == 0) {
		ESP_LOGW(TAG, "setFanMinRPM(0) would divide by zero; ignoring");
		return false;
	}
	// The quotient exceeds 16 bits below roughly 83 RPM; 0xFFFF is the
	// register's "no limit" encoding, which is the right saturation point.
	uint32_t raw = EMC2101_FAN_RPM_NUMERATOR / min_rpm;
	uint16_t raw_value = (raw > 0xFFFF) ? 0xFFFF : (uint16_t)raw;
	if (!tach_limit_lsb.write(raw_value & 0xFF)) {
		return false;
	}
	if (!tach_limit_msb.write((raw_value >> 8) & 0xFF)) {
		return false;
	}
	return true;
}

/**
 * @brief Read the external temperature diode
 *
 * @return float the current temperature in degrees C
 */
float EMC2101::getExternalTemperature(void) {
	if (i2c_dev == nullptr) {
		ESP_LOGW(TAG, "I2C device not begun");
		return 0.0;
	}
	// chip doesn't like doing multi-byte reads so we'll get each byte separately
	// and join
	uint8_t buffer[2];
	BusIO_Register ext_temp_lsb = BusIO_Register(i2c_dev, EMC2101_EXTERNAL_TEMP_LSB);
	BusIO_Register ext_temp_msb = BusIO_Register(i2c_dev, EMC2101_EXTERNAL_TEMP_MSB);

	// Read **MSB** first to match 'Data Read Interlock' behavoior from 6.1 of
	// datasheet
	if (!ext_temp_msb.read(buffer) || !ext_temp_lsb.read(buffer + 1)) {
		ESP_LOGW(TAG, "Failed to read external temperature registers");
		return 0.0;
	}

	int16_t raw_ext = buffer[0] << 8;
	raw_ext |= buffer[1];

	raw_ext >>= 5;
	return raw_ext * _TEMP_LSB;
}

/**
 * @brief Read the internal temperature sensor
 *
 * @return int8_t the current temperature in degrees celcius
 */
int8_t EMC2101::getInternalTemperature(void) {
	if (i2c_dev == nullptr) {
		ESP_LOGW(TAG, "I2C device not begun");
		return 0;
	}
	// _INTERNAL_TEMP = const(0x00)
	BusIO_Register int_temp_lsb = BusIO_Register(i2c_dev, EMC2101_INTERNAL_TEMP);

	return (int8_t)int_temp_lsb.read();
}

/**
 * @brief Read the current fan speed in RPM.
 *
 * @return uint16_t The current fan speed, 0 if no tachometer input
 */
uint16_t EMC2101::getFanRPM(void) {
	if (i2c_dev == nullptr) {
		ESP_LOGW(TAG, "I2C device not begun");
		return 0;
	}
	uint8_t buffer[2];
	BusIO_Register fan_speed_lsb = BusIO_Register(i2c_dev, EMC2101_TACH_LSB);
	BusIO_Register fan_speed_msb = BusIO_Register(i2c_dev, EMC2101_TACH_MSB);

	// Read LSB first to match 'Data Read Interlock' behavoior from 6.1 of
	// datasheet
	if (!fan_speed_lsb.read(buffer + 1) || !fan_speed_msb.read(buffer)) {
		ESP_LOGW(TAG, "Failed to read tach registers");
		return 0;
	}

	uint16_t raw_ext = buffer[0] << 8;
	raw_ext |= buffer[1];

	if (raw_ext == 0xFFFF || raw_ext == 0) {
		return 0;
	}

	return EMC2101_FAN_RPM_NUMERATOR / raw_ext;
}

/**
 * @brief Gets the current rate at which pressure and temperature measurements
 * are taken
 *
 * @return emc2101_rate_t The current data rate
 */
emc2101_rate_t EMC2101::getDataRate(void) {
	if (i2c_dev == nullptr) {
		ESP_LOGW(TAG, "I2C device not begun");
		return EMC2101_RATE_UNKNOWN;
	}
	BusIO_Register rate_reg = BusIO_Register(i2c_dev, EMC2101_REG_DATA_RATE, 1);
	BusIO_RegisterBits data_rate = BusIO_RegisterBits(&rate_reg, 4, 0);
	return (emc2101_rate_t)data_rate.read();
}

/**
 * @brief Sets the rate at which pressure and temperature measurements
 *
 * @param new_data_rate The data rate to set. Must be a `emc2101_rate_t`
 * @return bool true:success false:failure
 */
bool EMC2101::setDataRate(emc2101_rate_t new_data_rate) {
	if (i2c_dev == nullptr) {
		ESP_LOGW(TAG, "I2C device not begun");
		return false;
	}
	BusIO_Register rate_reg = BusIO_Register(i2c_dev, EMC2101_REG_DATA_RATE, 1);
	BusIO_RegisterBits data_rate = BusIO_RegisterBits(&rate_reg, 4, 0);
	return data_rate.write(new_data_rate);
}

/**
 * @brief Enable or disable outputting the fan control signal as a DC voltage
 * instead of the default PWM output
 *
 * @param enable_dac_out true will enable DAC output, false disables DAC output
 * @return true:success false: failure
 */
bool EMC2101::DACOutEnabled(bool enable_dac_out) {
	if (i2c_dev == nullptr) {
		ESP_LOGW(TAG, "I2C device not begun");
		return false;
	}
	BusIO_Register reg_config = BusIO_Register(i2c_dev, EMC2101_REG_CONFIG);
	BusIO_RegisterBits dac_output_enabled_bit = BusIO_RegisterBits(&reg_config, 1, 4);
	if (!dac_output_enabled_bit.write(enable_dac_out)) {
		return false;
	}
	return true;
}

/**
 * @brief Get the current DAC output enable setting
 *
 * @return true: DAC output enabled
 * @return false DAC output disabled
 */
bool EMC2101::DACOutEnabled(void) {
	if (i2c_dev == nullptr) {
		ESP_LOGW(TAG, "I2C device not begun");
		return false;
	}
	BusIO_Register reg_config = BusIO_Register(i2c_dev, EMC2101_REG_CONFIG);
	BusIO_RegisterBits dac_output_enabled_bit = BusIO_RegisterBits(&reg_config, 1, 4);
	return dac_output_enabled_bit.read();
}

/**
 * @brief Read the final PWM frequency and "effective resolution" of the PWM
 * driver. No effect when DAC output is enabled
 *
 * See the datasheet for additional information:
 * http://ww1.microchip.com/downloads/en/DeviceDoc/2101.pdf
 *
 * @return uint8_t The PWM freq register setting
 */
uint8_t EMC2101::getPWMFrequency(void) {
	if (i2c_dev == nullptr) {
		ESP_LOGW(TAG, "I2C device not begun");
		return 0;
	}
	BusIO_Register pwm_freq_reg = BusIO_Register(i2c_dev, EMC2101_PWM_FREQ);
	return pwm_freq_reg.read();
}

/**
 * @brief Set the final PWM frequency and "effective resolution" of the PWM
 * driver. No effect when DAC output is enabled
 *
 * See the datasheet for additional information:
 * http://ww1.microchip.com/downloads/en/DeviceDoc/2101.pdf
 *
 * @param pwm_freq The new PWM frequency setting
 *
 * @return bool true:success false:failure
 */
bool EMC2101::setPWMFrequency(uint8_t pwm_freq) {
	if (i2c_dev == nullptr) {
		ESP_LOGW(TAG, "I2C device not begun");
		return false;
	}
	BusIO_Register pwm_freq_reg = BusIO_Register(i2c_dev, EMC2101_PWM_FREQ);
	return pwm_freq_reg.write(pwm_freq);
}

/**
 * @brief Get the alternate PWM frequency digide value to use instead of the
 * clock selection bit when the clock select override is set
 *
 * @return uint8_t The alternate divisor setting
 */
uint8_t EMC2101::getPWMDivisor(void) {
	if (i2c_dev == nullptr) {
		ESP_LOGW(TAG, "I2C device not begun");
		return 0;
	}
	BusIO_Register pwm_divisor_reg = BusIO_Register(i2c_dev, EMC2101_PWM_DIV);
	return pwm_divisor_reg.read();
}

/**
 * @brief Get the alternate PWM frequency digide value to use instead of the
 * clock selection bit when the clock select override is set
 * @param pwm_divisor The alternate divisor setting
 * @return true:success false: failure
 */
bool EMC2101::setPWMDivisor(uint8_t pwm_divisor) {
	if (i2c_dev == nullptr) {
		ESP_LOGW(TAG, "I2C device not begun");
		return false;
	}
	BusIO_Register pwm_divisor_reg = BusIO_Register(i2c_dev, EMC2101_PWM_DIV);
	return pwm_divisor_reg.write(pwm_divisor);
}

/**
 * @brief Force the LUT to use the temperature set by `setForcedTemperature`.
 *
 * This can be used to use the LUT to set the fan speed based on a different
 * source than the external temperature diode. This can also be used to verify
 * LUT configuration
 *
 * @param enable_forced True to force the LUT to use the forced temperature
 * @return true: success false: failure
 */
bool EMC2101::enableForcedTemperature(bool enable_forced) {
	if (i2c_dev == nullptr) {
		ESP_LOGW(TAG, "I2C device not begun");
		return false;
	}
	BusIO_Register fan_config = BusIO_Register(i2c_dev, EMC2101_FAN_CONFIG);
	BusIO_RegisterBits forced_temp_en_bit = BusIO_RegisterBits(&fan_config, 1, 6);
	return forced_temp_en_bit.write(enable_forced);
}

/**
 * @brief Set the alternate temperature to use to look up a fan setting in the
 * look up table
 *
 * @param forced_temperature The alternative temperature reading used for LUT
 * lookups.
 * @return true: success false: falure
 */
bool EMC2101::setForcedTemperature(int8_t forced_temperature) {
	if (i2c_dev == nullptr) {
		ESP_LOGW(TAG, "I2C device not begun");
		return false;
	}
	BusIO_Register forced_temp_reg = BusIO_Register(i2c_dev, EMC2101_TEMP_FORCE);
	return forced_temp_reg.write(forced_temperature);
}

/**
 * @brief Get the alternate temperature to use to look up a fan setting in the
 * look up table
 *
 * @return int8_t The alternative temperature reading used for LUT
 * lookups
 */
int8_t EMC2101::getForcedTemperature(void) {
	if (i2c_dev == nullptr) {
		ESP_LOGW(TAG, "I2C device not begun");
		return 0;
	}
	BusIO_Register forced_temp_reg = BusIO_Register(i2c_dev, EMC2101_TEMP_FORCE);
	return forced_temp_reg.read();
}
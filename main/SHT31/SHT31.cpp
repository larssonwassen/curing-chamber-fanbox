#include "SHT31.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cmath>

static const char *TAG = "sht31";

// CRC-8 polynomial for SHT31: x^8 + x^5 + x^4 + 1
#define SHT31_CRC8_POLYNOMIAL 0x31

/**
 * @brief Construct a new SHT31::SHT31 object
 */
SHT31::SHT31(void) {
	_repeatability = SHT31_REPEATABILITY_HIGH;
	_last_read_time = 0;
	_last_temperature = 0.0f;
	_last_humidity = 0.0f;
	_has_valid_data = false;
	i2c_dev = nullptr;
}

/**
 * @brief Destroy the SHT31::SHT31 object
 */
SHT31::~SHT31(void) {
	end();
}

/**
 * @brief Initialize the SHT31 sensor
 * @param i2c_address The I2C address to use
 * @param bus The I2C bus to use
 * @return True if initialization was successful, false otherwise
 */
bool SHT31::begin(uint8_t i2c_address, I2CBus *bus) {
	if (i2c_dev) {
		delete i2c_dev;
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

/**
 * @brief Deinitialize the SHT31 sensor
 */
void SHT31::end() {
	if (i2c_dev) {
		delete i2c_dev;
		i2c_dev = nullptr;
	}
}

/**
 * @brief Initialize the SHT31 sensor
 * @return True if initialization successful, false otherwise
 */
bool SHT31::_init(void) {
	ESP_LOGI(TAG, "Initializing SHT31");

	// Wait a bit for sensor to be ready
	vTaskDelay(pdMS_TO_TICKS(100));

	// First, try to check if sensor is responsive without reset
	if (isConnected()) {
		ESP_LOGI(TAG, "SHT31 is responsive, skipping soft reset");
		return true;
	}

	ESP_LOGI(TAG, "SHT31 not responsive, attempting soft reset");

	// Perform soft reset
	if (!softReset()) {
		ESP_LOGE(TAG, "Failed to perform soft reset");
		return false;
	}

	// Wait for reset to complete
	vTaskDelay(pdMS_TO_TICKS(100));

	// Clear status register
	if (!clearStatus()) {
		ESP_LOGE(TAG, "Failed to clear status register");
		return false;
	}

	// Check if sensor is responsive
	if (!isConnected()) {
		ESP_LOGE(TAG, "SHT31 not responding after reset");
		return false;
	}

	return true;
}

/**
 * @brief Send a command to the SHT31 sensor
 * @param command The 16-bit command to send
 * @return True if command was sent successfully, false otherwise
 */
bool SHT31::_sendCommand(uint16_t command) {
	uint8_t cmd_data[2];
	cmd_data[0] = (command >> 8) & 0xFF;  // MSB
	cmd_data[1] = command & 0xFF;         // LSB

	ESP_LOGD(TAG, "Sending SHT31 command: 0x%04X (0x%02X 0x%02X)", command, cmd_data[0], cmd_data[1]);

	bool result = i2c_dev->write(cmd_data, 2, true, nullptr, 0);
	if (!result) {
		ESP_LOGE(TAG, "Failed to send SHT31 command: 0x%04X", command);
	}

	return result;
}

/**
 * @brief Read response data from the SHT31 sensor
 * @param data Buffer to store the response data
 * @param length Number of bytes to read
 * @return True if read was successful, false otherwise
 */
bool SHT31::_readResponse(uint8_t *data, uint8_t length) {
	// SHT31 uses command-based communication, not register-based
	// For measurement data, we need to read after the measurement command
	// Use I2C device read method that handles bus locking properly
	ESP_LOGD(TAG, "Reading %d bytes from SHT31", length);
	bool result = i2c_dev->read(data, length, true);
	if (!result) {
		ESP_LOGE(TAG, "Failed to read %d bytes from SHT31", length);
	}
	return result;
}

/**
 * @brief Calculate CRC-8 checksum for SHT31 data
 * @param data Data buffer to calculate CRC for
 * @param len Length of data buffer
 * @return CRC-8 checksum
 */
uint8_t SHT31::_crc8(const uint8_t *data, int len) {
	uint8_t crc = 0xFF;
	
	for (int i = 0; i < len; i++) {
		crc ^= data[i];
		for (int bit = 8; bit > 0; --bit) {
			if (crc & 0x80) {
				crc = (crc << 1) ^ SHT31_CRC8_POLYNOMIAL;
			} else {
				crc = (crc << 1);
			}
		}
	}
	
	return crc;
}

/**
 * @brief Verify CRC checksum for received data
 * @param data Data buffer to verify
 * @param crc Expected CRC value
 * @return True if CRC is valid, false otherwise
 */
bool SHT31::_verifyCRC(const uint8_t *data, uint8_t crc) {
	return _crc8(data, 2) == crc;
}

/**
 * @brief Convert raw temperature value to Celsius
 * @param raw_temp Raw temperature value from sensor
 * @return Temperature in degrees Celsius
 */
float SHT31::_convertRawTemperature(uint16_t raw_temp) {
	return 175.0f * (float)raw_temp / 65535.0f - 45.0f;
}

/**
 * @brief Convert raw humidity value to percentage
 * @param raw_humidity Raw humidity value from sensor
 * @return Humidity percentage (0-100%)
 */
float SHT31::_convertRawHumidity(uint16_t raw_humidity) {
	return 100.0f * (float)raw_humidity / 65535.0f;
}

/**
 * @brief Read temperature and humidity from sensor
 * @param temperature Pointer to store temperature value
 * @param humidity Pointer to store humidity value
 * @return True if reading was successful, false otherwise
 */
bool SHT31::readTempHumidity(float *temperature, float *humidity) {
	uint16_t temp_raw, hum_raw;
	
	if (!readTempHumidityRaw(&temp_raw, &hum_raw)) {
		return false;
	}
	
	*temperature = _convertRawTemperature(temp_raw);
	*humidity = _convertRawHumidity(hum_raw);
	
	// Store last valid reading
	_last_temperature = *temperature;
	_last_humidity = *humidity;
	_last_read_time = xTaskGetTickCount();
	_has_valid_data = true;
	
	return true;
}

/**
 * @brief Read raw temperature and humidity values from sensor
 * @param temp_raw Pointer to store raw temperature value
 * @param hum_raw Pointer to store raw humidity value
 * @return True if reading was successful, false otherwise
 */
bool SHT31::readTempHumidityRaw(uint16_t *temp_raw, uint16_t *hum_raw) {
	uint16_t command;
	uint8_t delay_ms;
	
	// Select command based on repeatability setting
	// Use non-stretch commands to avoid clock stretching issues
	switch (_repeatability) {
		case SHT31_REPEATABILITY_HIGH:
			command = SHT31_CMD_MEAS_HIGHREP;
			delay_ms = SHT31_MEAS_DURATION_HIGH;
			break;
		case SHT31_REPEATABILITY_MEDIUM:
			command = SHT31_CMD_MEAS_MEDREP;
			delay_ms = SHT31_MEAS_DURATION_MED;
			break;
		case SHT31_REPEATABILITY_LOW:
			command = SHT31_CMD_MEAS_LOWREP;
			delay_ms = SHT31_MEAS_DURATION_LOW;
			break;
		default:
			ESP_LOGE(TAG, "Invalid repeatability setting");
			return false;
	}
	
	// Send measurement command
	if (!_sendCommand(command)) {
		ESP_LOGE(TAG, "Failed to send measurement command");
		return false;
	}
	
	// Wait for measurement to complete
	// Add extra delay to ensure measurement is ready
	vTaskDelay(pdMS_TO_TICKS(delay_ms + 5));
	
	// Read 6 bytes: temp_msb, temp_lsb, temp_crc, hum_msb, hum_lsb, hum_crc
	uint8_t data[6];
	ESP_LOGD(TAG, "Attempting to read 6 bytes from SHT31");
	if (!_readResponse(data, 6)) {
		ESP_LOGE(TAG, "Failed to read measurement data");
		return false;
	}
	
	ESP_LOGD(TAG, "Read data: %02X %02X %02X %02X %02X %02X", data[0], data[1], data[2], data[3], data[4], data[5]);

	// Verify CRC for temperature data
	if (!_verifyCRC(&data[0], data[2])) {
		ESP_LOGE(TAG, "Temperature CRC verification failed");
		return false;
	}
	
	// Verify CRC for humidity data
	if (!_verifyCRC(&data[3], data[5])) {
		ESP_LOGE(TAG, "Humidity CRC verification failed");
		return false;
	}
	
	// Extract raw values
	*temp_raw = (data[0] << 8) | data[1];
	*hum_raw = (data[3] << 8) | data[4];
	
	return true;
}

/**
 * @brief Read temperature from sensor
 * @return Temperature in degrees Celsius, or NaN if error
 */
float SHT31::readTemperature(void) {
	float temperature, humidity;
	if (readTempHumidity(&temperature, &humidity)) {
		return temperature;
	}
	return NAN;
}

/**
 * @brief Read humidity from sensor
 * @return Humidity percentage (0-100%), or NaN if error
 */
float SHT31::readHumidity(void) {
	float temperature, humidity;
	if (readTempHumidity(&temperature, &humidity)) {
		return humidity;
	}
	return NAN;
}

/**
 * @brief Set the measurement repeatability
 * @param repeatability Repeatability setting
 * @return True if setting was successful, false otherwise
 */
bool SHT31::setRepeatability(sht31_repeatability_t repeatability) {
	if (repeatability > SHT31_REPEATABILITY_LOW) {
		ESP_LOGE(TAG, "Invalid repeatability setting");
		return false;
	}
	
	_repeatability = repeatability;
	ESP_LOGI(TAG, "Repeatability set to %d", repeatability);
	return true;
}

/**
 * @brief Get the current repeatability setting
 * @return Current repeatability setting
 */
sht31_repeatability_t SHT31::getRepeatability(void) {
	return _repeatability;
}

/**
 * @brief Perform a soft reset of the sensor
 * @return True if reset was successful, false otherwise
 */
bool SHT31::softReset(void) {
	if (!_sendCommand(SHT31_CMD_SOFTRESET)) {
		ESP_LOGE(TAG, "Failed to send soft reset command");
		return false;
	}
	
	// Wait for reset to complete
	vTaskDelay(pdMS_TO_TICKS(10));
	
	// Reset internal state
	_has_valid_data = false;
	_last_read_time = 0;
	
	ESP_LOGI(TAG, "Soft reset completed");
	return true;
}

/**
 * @brief Read the status register
 * @return Status register value, or 0 if error
 */
uint16_t SHT31::readStatus(void) {
	uint16_t status = 0;
	(void)readStatus(&status);
	return status;
}

/**
 * @brief Read the status register, reporting failure separately from the value.
 *
 * The uint16_t-returning overload cannot distinguish a failed read from a
 * healthy sensor whose status bits are all clear -- both come back as 0.
 *
 * @param out Receives the status word; untouched on failure
 * @return True on a successful, CRC-verified read
 */
bool SHT31::readStatus(uint16_t* out) {
	if (!_sendCommand(SHT31_CMD_READSTATUS)) {
		ESP_LOGE(TAG, "Failed to send read status command");
		return false;
	}

	// Read 3 bytes: status_msb, status_lsb, status_crc
	uint8_t data[3];
	if (!_readResponse(data, 3)) {
		ESP_LOGE(TAG, "Failed to read status data");
		return false;
	}

	// Verify CRC for status data
	if (!_verifyCRC(&data[0], data[2])) {
		ESP_LOGE(TAG, "Status CRC verification failed");
		return false;
	}

	*out = (uint16_t)((data[0] << 8) | data[1]);
	return true;
}

/**
 * @brief Clear the status register
 * @return True if clear was successful, false otherwise
 */
bool SHT31::clearStatus(void) {
	if (!_sendCommand(SHT31_CMD_CLEARSTATUS)) {
		ESP_LOGE(TAG, "Failed to send clear status command");
		return false;
	}
	
	ESP_LOGI(TAG, "Status register cleared");
	return true;
}

/**
 * @brief Enable or disable the internal heater
 * @param enable True to enable heater, false to disable
 * @return True if command was successful, false otherwise
 */
bool SHT31::enableHeater(bool enable) {
	uint16_t command = enable ? SHT31_CMD_HEATER_ENABLE : SHT31_CMD_HEATER_DISABLE;
	
	if (!_sendCommand(command)) {
		ESP_LOGE(TAG, "Failed to send heater command");
		return false;
	}
	
	ESP_LOGI(TAG, "Heater %s", enable ? "enabled" : "disabled");
	return true;
}

/**
 * @brief Check if the internal heater is enabled
 * @return True if heater is enabled, false otherwise
 */
bool SHT31::isHeaterEnabled(void) {
	uint16_t status = readStatus();
	return (status & SHT31_STATUS_HEATER_ENABLED) != 0;
}

/**
 * @brief Check if the sensor is connected and responsive
 * @return True if sensor is connected, false otherwise
 */
bool SHT31::isConnected(void) {
	// A status word of 0 is a perfectly valid reading -- it means every status
	// bit is clear -- so testing `status != 0` reported a healthy sensor as
	// disconnected. What matters is whether the read itself succeeded.
	uint16_t status = 0;
	if (!readStatus(&status)) {
		return false;
	}
	ESP_LOGD(TAG, "SHT31 status register: 0x%04X", status);
	return true;
} 
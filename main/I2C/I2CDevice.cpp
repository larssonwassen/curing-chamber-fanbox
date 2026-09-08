#include "I2CDevice.h"

static const char *TAG = "i2c_device";

/*!
 *    @brief  Create an I2C device at a given address
 *    @param  bus The I2CBus the device is on
 *    @param  addr The 7-bit I2C address for the device
 */
I2CDevice::I2CDevice(I2CBus* bus, uint8_t addr) {
	_bus = bus;
	_addr = addr;
	_begun = false;
	_maxBufferSize = I2C_BUFFER_LENGTH;
	_device = NULL;
}

I2CDevice::~I2CDevice() {
	end();
}

/*!
 *    @brief  Initializes and does basic address detection
 *    @return True if I2C initialized and a device with the addr found
 */
bool I2CDevice::begin() {
	if (_begun) {
		return true;
	}

	if (!_bus) {
		ESP_LOGE(TAG, "I2C bus is null");
		return false;
	}

	if (!_bus->begin()) {
		ESP_LOGE(TAG, "Failed to initialize I2C bus");
		return false;
	}

	_device = _bus->addDevice(_addr);
	if (_device == NULL) {
		ESP_LOGE(TAG, "Failed to add I2C device at address 0x%02X", _addr);
		return false;
	}

	_begun = true;
	return detected();
}

/*!
 *    @brief  De-initialize device, remove it from the bus
 */
void I2CDevice::end(void) {
	if (_begun) {
		if (_device != NULL) {
			_bus->removeDevice(_device);
			_device = NULL;
		}
		_begun = false;
	}
}

/*!
 *    @brief  Scans I2C for the device
 *    @return True if the device is detected
 */
bool I2CDevice::detected(void) {
	if (!_begun && !begin()) {
		ESP_LOGE(TAG, "I2CDevice not begun, cant detect.");
		return false;
	}
	ESP_LOGD(TAG, "Scanning for I2C device at address: 0x%02X", _addr);
	esp_err_t err = _bus->probeDevice(_addr, _bus->getTimeOut());
	if (err == ESP_OK) {
		ESP_LOGD(TAG, "Device detected at address 0x%02X", _addr);
		return true;
	} else {
		ESP_LOGD(TAG, "Device not detected at address 0x%02X", _addr);
		return false;
	}
}

/*!
 *    @brief  Write data to the I2C device
 *    @param  buffer Pointer to buffer of data to write
 *    @param  len Number of bytes from buffer to write
 *    @param  stop Whether to send an I2C STOP signal on write
 *    @param  prefix_buffer Pointer to optional data to write before buffer
 *    @param  prefix_len Number of bytes from prefix buffer to write
 *    @return True if write was successful, otherwise false
 */
bool I2CDevice::write(const uint8_t *buffer, size_t len, bool stop, const uint8_t *prefix_buffer, size_t prefix_len) {
	if (!buffer || (len == 0)) {
		ESP_LOGE(TAG, "Invalid buffer or length");
		return false;
	}

	if ((len + prefix_len) > maxBufferSize()) {
		ESP_LOGE(TAG, "I2CDevice could not write such a large buffer");
		return false;
	}

	if (!_begun) {
		ESP_LOGE(TAG, "I2CDevice not begun");
		return false;
	}

	if (!_bus->beginTransmission(_device)) {
		ESP_LOGE(TAG, "Failed to begin transmission");
		return false;
	}

	// Write the prefix data (usually a register address)
	if ((prefix_len != 0) && (prefix_buffer != nullptr)) {
		if (_bus->write(prefix_buffer, prefix_len) != prefix_len) {
			ESP_LOGD(TAG, "I2CDevice failed to write prefix data");
			_bus->endTransmission(true); // Ensure bus is released
			return false;
		}
	}

	// Write the main data
	if (_bus->write(buffer, len) != len) {
		ESP_LOGD(TAG, "I2CDevice failed to write data");
		_bus->endTransmission(true); // Ensure bus is released
		return false;
	}

	uint8_t result = _bus->endTransmission(stop);
	if (result == 0) {
		return true;
	} else {
		ESP_LOGD(TAG, "I2CDevice endTransmission failed with error code %d", result);
		return false;
	}
}

/*!
 *    @brief  Read from I2C into a buffer from the I2C device
 *    @param  buffer Pointer to buffer of data to read into
 *    @param  len Number of bytes from buffer to read
 *    @param  stop Whether to send an I2C STOP signal on read
 *    @return True if read was successful, otherwise false
 */
bool I2CDevice::read(uint8_t *buffer, size_t len, bool stop) {
	size_t pos = 0;
	while (pos < len) {
		size_t read_len = ((len - pos) > maxBufferSize()) ? maxBufferSize() : (len - pos);
		bool read_stop = (pos < (len - read_len)) ? false : stop;
		if (!_read(buffer + pos, read_len, read_stop))
			return false;
		pos += read_len;
	}
	return true;
}

/*!
 *    @brief  Internal method to read data
 *    @param  buffer Pointer to buffer of data to read into
 *    @param  len Number of bytes from buffer to read
 *    @param  stop Whether to send an I2C STOP signal on read
 *    @return True if read was successful, otherwise false
 */
bool I2CDevice::_read(uint8_t *buffer, size_t len, bool stop) {
	if (!_begun) {
		ESP_LOGE(TAG, "I2CDevice not begun");
		return false;
	}

	size_t recv = _bus->requestFrom(_device, len, stop);
	if (recv != len) {
		ESP_LOGD(TAG, "I2CDevice did not receive enough data: %d", recv);
		return false;
	}

	for (size_t i = 0; i < len; i++) {
		buffer[i] = _bus->read();
	}

	return true;
}

/*!
 *    @brief  Write data, then read data from I2C into another buffer
 *    @param  write_buffer Pointer to buffer of data to write
 *    @param  write_len Number of bytes from buffer to write
 *    @param  read_buffer Pointer to buffer of data to read into
 *    @param  read_len Number of bytes from buffer to read
 *    @param  stop Whether to send an I2C STOP signal between the write and read
 *    @return True if write & read was successful, otherwise false
 */
bool I2CDevice::write_then_read(const uint8_t *write_buffer, size_t write_len, uint8_t *read_buffer, size_t read_len, bool stop) {
	if (!_begun) {
		ESP_LOGE(TAG, "I2CDevice not begun");
		return false;
	}

	// Begin transmission
	_bus->beginTransmission(_device);

	// Write data
	if (_bus->write(write_buffer, write_len) != write_len) {
		ESP_LOGD(TAG, "I2CDevice failed to write in write_then_read");
		_bus->endTransmission(true); // Ensure bus is released
		return false;
	}

	// End transmission without stop to perform repeated start
	uint8_t result = _bus->endTransmission(false);
	if (result != 0) {
		ESP_LOGD(TAG, "I2CDevice endTransmission failed in write_then_read with error code %d", result);
		return false;
	}

	// Read data
	if (!read(read_buffer, read_len, stop)) {
		ESP_LOGD(TAG, "I2CDevice read failed in write_then_read");
		return false;
	}

	return true;
}

/*!
 *    @brief  Returns the 7-bit address of this device
 *    @return The 7-bit address of this device
 */
uint8_t I2CDevice::address(void) { return _addr; }

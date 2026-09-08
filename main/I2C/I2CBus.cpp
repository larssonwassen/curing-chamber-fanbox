#include "I2CBus.h"
#include "driver/i2c_types.h"

static const char *TAG = "i2c_bus";

/**
 * @brief Minimum size, in bytes, of the internal private structure used to describe
 * I2C commands link.
 */
#define I2C_INTERNAL_STRUCT_SIZE (24)

/**
 * @brief The following macro is used to determine the recommended size of the
 * buffer to pass to `i2c_cmd_link_create_static()` function.
 * It requires one parameter, `TRANSACTIONS`, describing the number of transactions
 * intended to be performed on the I2C port.
 * For example, if one wants to perform a read on an I2C device register, `TRANSACTIONS`
 * must be at least 2, because the commands required are the following:
 *  - write device register
 *  - read register content
 *
 * Signals such as "(repeated) start", "stop", "nack", "ack" shall not be counted.
 */
#define I2C_LINK_RECOMMENDED_SIZE(TRANSACTIONS) (2 * I2C_INTERNAL_STRUCT_SIZE + I2C_INTERNAL_STRUCT_SIZE * (5 * TRANSACTIONS))

I2CBus::I2CBus()
    :initialized(false)
    ,num(I2C_NUM_0)
    ,sda(SDA)
    ,scl(SCL)
    ,bufferSize(I2C_BUFFER_LENGTH) // default Wire Buffer Size
    ,rxBuffer(NULL)
    ,rxIndex(0)
    ,rxLength(0)
    ,txBuffer(NULL)
    ,txLength(0)
    ,txAddress(0)
    ,_timeOutMillis(50)
    ,nonStop(false)
    ,nonStopTask(NULL)
    ,lock(NULL)
    ,numDevices(0)
{}

I2CBus::~I2CBus()
{
    end();
    if (lock != NULL){
        vSemaphoreDelete(lock);
    }
}


bool I2CBus::allocateBusBuffer(void)
{
    // Allocate both buffers or none
    if (rxBuffer == NULL) {
        rxBuffer = (uint8_t *)malloc(bufferSize);
        if (rxBuffer == NULL) {
            ESP_LOGE(TAG, "Can't allocate memory for I2C_%d rxBuffer", num);
            return false;
        }
    }
    if (txBuffer == NULL) {
        txBuffer = (uint8_t *)malloc(bufferSize);
        if (txBuffer == NULL) {
            ESP_LOGE(TAG, "Can't allocate memory for I2C_%d txBuffer", num);
            freeBusBuffer();  // Free rxBuffer for safety
            return false;
        }
    }
    return true;
}

void I2CBus::freeBusBuffer(void)
{
    if (rxBuffer != NULL) {
        free(rxBuffer);
        rxBuffer = NULL;
    }
    if (txBuffer != NULL) {
        free(txBuffer);
        txBuffer = NULL;
    }
}

size_t I2CBus::setBufferSize(size_t bSize)
{
    // Minimum size is 32 bytes (I2C FIFO length for ESP32/S2/S3/C3)
    if (bSize < 32) {
        ESP_LOGE(TAG, "Minimum Wire Buffer size is 32 bytes");
        return 0;
    }
    if (lock == NULL) {
        lock = xSemaphoreCreateMutex();
        if(lock == NULL){
            ESP_LOGE(TAG, "xSemaphoreCreateMutex failed");
            return 0;
        }
    }

    // Acquire lock
    if (xSemaphoreTake(lock, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "could not acquire lock");
        return 0;
    }

    // Allocate or reallocate buffers
    if (rxBuffer != NULL || txBuffer != NULL) {
        if (bSize != bufferSize) {
            freeBusBuffer();
            bufferSize = bSize;
            if (!allocateBusBuffer()) {
                bSize = 0; // Return error
                ESP_LOGE(TAG, "Buffer allocation failed");
            }
        }
    } else {
        bufferSize = bSize;
    }
    // Release lock
    xSemaphoreGive(lock);
    return bSize;
}


// Master Begin
bool I2CBus::begin()
{
    bool started = false;
    esp_err_t err = ESP_OK;
    if (lock == NULL) {
        lock = xSemaphoreCreateMutex();
        if(lock == NULL){
            ESP_LOGE(TAG, "xSemaphoreCreateMutex failed");
            return false;
        }
    }
    // Acquire lock
    if (xSemaphoreTake(lock, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "could not acquire lock");
        return false;
    }

    if (initialized) {
        started = true;
    } else if (allocateBusBuffer()) {
        ESP_LOGI(TAG, "Initializing I2C Master: sda=%d scl=%d", sda, scl);

        i2c_master_bus_config_t conf;
        conf.i2c_port = num;
        conf.sda_io_num = sda;
        conf.scl_io_num = scl;
        conf.clk_source = I2C_CLK_SRC_DEFAULT;
        conf.glitch_ignore_cnt = 7;
        conf.intr_priority = 0;
        conf.trans_queue_depth = 0;

        err = i2c_new_master_bus(&conf, &bus_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2c_new_master_bus failed");
        } else {
            initialized = true;
        }

        started = (err == ESP_OK);
    }

    if (!started) freeBusBuffer();
    // Release lock
    xSemaphoreGive(lock);
    return started;
}

bool I2CBus::end()
{
    esp_err_t err = ESP_OK;
    if (lock != NULL) {
        // Acquire lock
        if (xSemaphoreTake(lock, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "could not acquire lock");
            return false;
        }
        if (initialized) {
            // Remove all devices
            for (size_t i = 0; i < numDevices; ++i) {
                i2c_master_bus_rm_device(devices[i].handle);
            }
            numDevices = 0;

            err = i2c_del_master_bus(bus_handle);
            if (err == ESP_OK) {
                initialized = false;
            }
        }
        freeBusBuffer();
        // Release lock
        xSemaphoreGive(lock);
    }
    return (err == ESP_OK);
}

void I2CBus::setTimeOut(uint16_t timeOutMillis)
{
    _timeOutMillis = timeOutMillis;
}

uint16_t I2CBus::getTimeOut()
{
    return _timeOutMillis;
}

// New method to add a device
esp_err_t I2CBus::probeDevice(uint16_t address, uint32_t timeOutMillis) {
    if (!initialized) {
        ESP_LOGE(TAG, "Bus not initialized");
        return ESP_FAIL;
    }

    esp_err_t ret = i2c_master_probe(bus_handle, address, timeOutMillis);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "Device not found at address 0x%02X", address);
    }
    return ret;
}
i2c_master_dev_handle_t I2CBus::addDevice(uint16_t address, uint32_t sclHz)
{
    if (!initialized) {
        ESP_LOGE(TAG, "Bus not initialized");
        return NULL;
    }

    // Acquire lock
    if (xSemaphoreTake(lock, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Could not acquire lock");
        return NULL;
    }

    i2c_master_dev_handle_t device = NULL;
    esp_err_t ret = ESP_OK;

    // Check if device already exists
    for (size_t i = 0; i < numDevices; i++) {
        if (devices[i].address == address) {
            device = devices[i].handle;
            ESP_LOGW(TAG, "Device with address 0x%X already added", address);
            xSemaphoreGive(lock);
            return device;
        }
    }

    if (numDevices >= MAX_I2C_DEVICES) {
        ESP_LOGE(TAG, "Maximum number of I2C devices reached");
        xSemaphoreGive(lock);
        return NULL;
    }

    // Create device handle
    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = sclHz,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = false,
        },
    };

    ret = i2c_master_bus_add_device(bus_handle, &device_config, &device);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device with address 0x%X", address);
        device = NULL;
    } else {
        // Store the device
        devices[numDevices].address = address;
        devices[numDevices].handle = device;
        numDevices++;
        ESP_LOGI(TAG, "Added I2C device with address 0x%X", address);
    }

    // Release lock
    xSemaphoreGive(lock);

    return device;
}

// New method to remove a device
esp_err_t I2CBus::removeDevice(i2c_master_dev_handle_t device)
{
    if (!initialized) {
        ESP_LOGE(TAG, "Bus not initialized");
        return ESP_FAIL;
    }

    // Acquire lock
    if (xSemaphoreTake(lock, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Could not acquire lock");
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_OK;
    bool found = false;

    // Find the device and remove it
    for (size_t i = 0; i < numDevices; i++) {
        if (devices[i].handle == device) {
            ret = i2c_master_bus_rm_device(device);
            if (ret == ESP_OK) {
                // Shift devices down
                for (size_t j = i; j < numDevices - 1; j++) {
                    devices[j] = devices[j + 1];
                }
                numDevices--;
                ESP_LOGI(TAG, "Removed I2C device");
            } else {
                ESP_LOGE(TAG, "Failed to remove I2C device");
            }
            found = true;
            break;
        }
    }

    if (!found) {
        ESP_LOGW(TAG, "I2C device not found");
        ret = ESP_FAIL;
    }

    // Release lock
    xSemaphoreGive(lock);

    return ret;
}

// Updated beginTransmission to accept device handle
bool I2CBus::beginTransmission(i2c_master_dev_handle_t device)
{
    if (nonStop && nonStopTask == xTaskGetCurrentTaskHandle()) {
        ESP_LOGE(TAG, "Unfinished Repeated Start transaction! Expected requestFrom, not beginTransmission! Clearing...");
        // Release lock
        xSemaphoreGive(lock);
    }

    // Acquire lock
    if (lock == NULL || xSemaphoreTake(lock, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "could not acquire lock");
        return false;
    }

    nonStop = false;
    txLength = 0;
    txAddress = 0;
    for (size_t i = 0; i < numDevices; i++) {
        if (devices[i].handle == device) {
            txAddress = devices[i].address;
            break;
        }
    }

    if (txAddress == 0) {
        ESP_LOGE(TAG, "Device not found");
        xSemaphoreGive(lock);
        return false;
    }

    // Store device handle (if needed)
    // For simplicity, we assume that the device handle will be passed to endTransmission
    return true;
}

// Updated endTransmission
uint8_t I2CBus::endTransmission(bool sendStop)
{
    if (txBuffer == NULL){
        ESP_LOGE(TAG, "NULL TX buffer pointer");
        return 4;
    }
    esp_err_t err = ESP_OK;

    // We need to get the device handle somehow
    // For simplicity, we'll assume that txAddress holds the device address

    // Find the device handle based on txAddress
    i2c_master_dev_handle_t device = NULL;
    for (size_t i = 0; i < numDevices; ++i) {
        if (devices[i].address == txAddress) {
            device = devices[i].handle;
            break;
        }
    }

    if (device == NULL) {
        ESP_LOGE(TAG, "Device not found for address 0x%X", txAddress);
        xSemaphoreGive(lock);
        return 4;
    }

    if (sendStop) {
        err = halI2CWrite(device, txBuffer, txLength, _timeOutMillis);

        // Release lock
        xSemaphoreGive(lock);

    } else {
        // Mark as non-stop
        nonStop = true;
        nonStopTask = xTaskGetCurrentTaskHandle();
    }

    switch(err) {
        case ESP_OK: return 0;
        case ESP_FAIL: return 2;
        case ESP_ERR_TIMEOUT: return 5;
        default: return 4;
    }
}

// Updated halI2CWrite to accept device handle
esp_err_t I2CBus::halI2CWrite(i2c_master_dev_handle_t device, const uint8_t* buff, size_t size, uint32_t timeOutMillis) {
    esp_err_t ret = ESP_FAIL;

    if (!initialized) {
        ESP_LOGE(TAG, "Bus is not initialized");
        return ret;
    }

    if (device == NULL) {
        ESP_LOGE(TAG, "Invalid device handle");
        return ret;
    }

    ret = i2c_master_transmit(device, buff, size, timeOutMillis);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_transmit failed");
    }

    return ret;
}

// Updated halI2CWriteReadNonStop to accept device handle
esp_err_t I2CBus::halI2CWriteReadNonStop(i2c_master_dev_handle_t device, const uint8_t* wbuff, size_t wsize, uint8_t* rbuff, size_t rsize, uint32_t timeOutMillis, size_t *readCount) {
    esp_err_t ret = ESP_FAIL;

    if (!initialized) {
        ESP_LOGE(TAG, "Bus is not initialized");
        return ret;
    }

    if (device == NULL) {
        ESP_LOGE(TAG, "Invalid device handle");
        return ret;
    }

    ret = i2c_master_transmit_receive(device, wbuff, wsize, rbuff, rsize, timeOutMillis);
    if (ret == ESP_OK){
        *readCount = rsize;
    } else {
        *readCount = 0;
        ESP_LOGE(TAG, "i2c_master_transmit_receive failed");
    }

    return ret;
}

// Updated halI2CRead to accept device handle
esp_err_t I2CBus::halI2CRead(i2c_master_dev_handle_t device, uint8_t* buff, size_t size, uint32_t timeOutMillis, size_t *readCount) {
    esp_err_t ret = ESP_FAIL;

    if (!initialized) {
        ESP_LOGE(TAG, "Bus is not initialized");
        return ret;
    }

    if (device == NULL) {
        ESP_LOGE(TAG, "Invalid device handle");
        return ret;
    }

    ret = i2c_master_receive(device, buff, size, timeOutMillis);
    if (ret == ESP_OK) {
        *readCount = size;
    } else {
        *readCount = 0;
        ESP_LOGE(TAG, "i2c_master_receive failed");
    }

    return ret;
}

// Updated requestFrom to accept device handle
size_t I2CBus::requestFrom(i2c_master_dev_handle_t device, size_t size, bool sendStop)
{
    if (rxBuffer == NULL || txBuffer == NULL){
        ESP_LOGE(TAG, "NULL buffer pointer");
        return 0;
    }

    esp_err_t err = ESP_OK;
    if (nonStop && nonStopTask == xTaskGetCurrentTaskHandle()) {
        nonStop = false;
        rxIndex = 0;
        rxLength = 0;
        err = halI2CWriteReadNonStop(device, txBuffer, txLength, rxBuffer, size, _timeOutMillis, &rxLength);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "halI2CWriteReadNonStop returned Error %d", err);
        }

        // Release lock
        xSemaphoreGive(lock);

    } else {
        // Acquire lock
        if(lock == NULL || xSemaphoreTake(lock, portMAX_DELAY) != pdTRUE){
            ESP_LOGE(TAG, "could not acquire lock");
            return 0;
        }
        rxIndex = 0;
        rxLength = 0;
        err = halI2CRead(device, rxBuffer, size, _timeOutMillis, &rxLength);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "halI2CRead returned Error %d", err);
        }
        // Release lock
        xSemaphoreGive(lock);
    }

    return rxLength;
}

size_t I2CBus::write(uint8_t data)
{
    if (txBuffer == NULL){
        ESP_LOGE(TAG, "NULL TX buffer pointer");
        return 0;
    }
    if (txLength >= bufferSize) {
        return 0;
    }
    txBuffer[txLength++] = data;
    return 1;
}

size_t I2CBus::write(const uint8_t *data, size_t quantity)
{
    for (size_t i = 0; i < quantity; ++i) {
        if (!write(data[i])) {
            return i;
        }
    }
    return quantity;
}

int I2CBus::available(void)
{
    return (int)(rxLength - rxIndex);
}

int I2CBus::read(void)
{
    int value = -1;
    if (rxBuffer == NULL) {
        ESP_LOGE(TAG, "NULL RX buffer pointer");
        return value;
    }
    if (rxIndex < rxLength) {
        value = rxBuffer[rxIndex++];
    }
    return value;
}

int I2CBus::peek(void)
{
    int value = -1;
    if (rxBuffer == NULL) {
        ESP_LOGE(TAG, "NULL RX buffer pointer");
        return value;
    }
    if (rxIndex < rxLength) {
        value = rxBuffer[rxIndex];
    }
    return value;
}

void I2CBus::flush(void)
{
    rxIndex = 0;
    rxLength = 0;
    txLength = 0;
}

I2CBus I2CBus0 = I2CBus();
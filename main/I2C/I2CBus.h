#ifndef I2CBus_h
#define I2CBus_h

#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include "driver/i2c_master.h"
#include "driver/i2c_types.h"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "pins.h"

#define I2C_BUFFER_LENGTH 128

class I2CBus {
protected:
    bool initialized;
    i2c_port_t num;
    gpio_num_t sda;
    gpio_num_t scl;

    size_t bufferSize;
    uint8_t *rxBuffer;
    size_t rxIndex;
    size_t rxLength;

    uint8_t *txBuffer;
    size_t txLength;
    uint16_t txAddress;

    uint32_t _timeOutMillis;
    bool nonStop;
    TaskHandle_t nonStopTask;
    SemaphoreHandle_t lock;
    i2c_master_bus_handle_t bus_handle;

    // New members for device management
    static const size_t MAX_I2C_DEVICES = 10;
    struct I2CDeviceEntry {
        uint16_t address;
        i2c_master_dev_handle_t handle;
    };
    I2CDeviceEntry devices[MAX_I2C_DEVICES];
    size_t numDevices;

private:
    bool initPins();
    bool allocateBusBuffer(void);
    void freeBusBuffer(void);

    // Updated methods to accept device handles
    esp_err_t halI2CWrite(i2c_master_dev_handle_t device, const uint8_t* buff, size_t size, uint32_t timeOutMillis);
    esp_err_t halI2CWriteReadNonStop(i2c_master_dev_handle_t device, const uint8_t* wbuff, size_t wsize, uint8_t* rbuff, size_t rsize, uint32_t timeOutMillis, size_t *readCount);
    esp_err_t halI2CRead(i2c_master_dev_handle_t device, uint8_t* buff, size_t size, uint32_t timeOutMillis, size_t *readCount);

public:
    I2CBus();
    ~I2CBus();

    bool begin();
    bool end();

    size_t setBufferSize(size_t bSize);

    void setTimeOut(uint16_t timeOutMillis); // default timeout of i2c transactions is 50ms
    uint16_t getTimeOut();

    // New methods for device management
    i2c_master_dev_handle_t addDevice(uint16_t address, uint32_t sclHz = 100000);
    esp_err_t removeDevice(i2c_master_dev_handle_t device);
    esp_err_t probeDevice(uint16_t address, uint32_t timeOutMillis);

    // Updated methods to work with device handles
    bool beginTransmission(i2c_master_dev_handle_t device);
    uint8_t endTransmission(bool sendStop = true);

    size_t requestFrom(i2c_master_dev_handle_t device, size_t size, bool sendStop = true);

    size_t write(uint8_t);
    size_t write(const uint8_t *, size_t);
    int available(void);
    int read(void);
    int peek(void);
    void flush(void);

    inline size_t write(const char * s)
    {
        return write((uint8_t*) s, strlen(s));
    }
    inline size_t write(unsigned long n)
    {
        return write((uint8_t)n);
    }
    inline size_t write(long n)
    {
        return write((uint8_t)n);
    }
    inline size_t write(unsigned int n)
    {
        return write((uint8_t)n);
    }
    inline size_t write(int n)
    {
        return write((uint8_t)n);
    }
};

extern I2CBus I2CBus0;

#endif // I2CBus_h
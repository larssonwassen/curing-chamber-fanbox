#ifndef I2CDEVICE_H
#define I2CDEVICE_H

#include "I2CBus.h"
#include "BusIO_Register.h"
#include "esp_log.h"

class I2CBus;

class I2CDevice {
public:
  I2CDevice(I2CBus* bus, uint8_t addr);
  ~I2CDevice();
  bool begin();
  void end();

  bool detected();

  bool read(uint8_t *buffer, size_t len, bool stop = true);
  bool write(const uint8_t *buffer, size_t len, bool stop = true, const uint8_t *prefix_buffer = nullptr, size_t prefix_len = 0);
  bool write_then_read(const uint8_t *write_buffer, size_t write_len, uint8_t *read_buffer, size_t read_len, bool stop = false);

  uint8_t address(void);
  size_t maxBufferSize() { return _maxBufferSize; }

private:
  I2CBus* _bus;
  i2c_master_dev_handle_t _device;
  uint8_t _addr;
  bool _begun;
  size_t _maxBufferSize;

  bool _read(uint8_t *buffer, size_t len, bool stop);
};

#endif // I2CDEVICE_H

#ifndef _SHT31_H
#define _SHT31_H

#include "I2C/I2CDevice.h"

// Default I2C addresses
#define SHT31_I2CADDR_DEFAULT 0x44	///< SHT31 default I2C address
#define SHT31_I2CADDR_ALTERNATE 0x45	///< SHT31 alternate I2C address

// Commands
#define SHT31_CMD_MEAS_HIGHREP_STRETCH 0x2C06	///< High repeatability measurement with clock stretch
#define SHT31_CMD_MEAS_MEDREP_STRETCH 0x2C0D	///< Medium repeatability measurement with clock stretch
#define SHT31_CMD_MEAS_LOWREP_STRETCH 0x2C10	///< Low repeatability measurement with clock stretch
#define SHT31_CMD_MEAS_HIGHREP 0x2400	///< High repeatability measurement
#define SHT31_CMD_MEAS_MEDREP 0x240B	///< Medium repeatability measurement
#define SHT31_CMD_MEAS_LOWREP 0x2416	///< Low repeatability measurement
#define SHT31_CMD_READSTATUS 0xF32D	///< Read status register
#define SHT31_CMD_CLEARSTATUS 0x3041	///< Clear status register
#define SHT31_CMD_SOFTRESET 0x30A2	///< Soft reset
#define SHT31_CMD_HEATER_ENABLE 0x306D	///< Enable heater
#define SHT31_CMD_HEATER_DISABLE 0x3066	///< Disable heater

// Status register bits
#define SHT31_STATUS_ALERT_PENDING 0x8000	///< Alert pending status
#define SHT31_STATUS_HEATER_ENABLED 0x2000	///< Heater enabled status
#define SHT31_STATUS_HUMIDITY_ALERT 0x0800	///< Humidity tracking alert
#define SHT31_STATUS_TEMP_ALERT 0x0400	///< Temperature tracking alert
#define SHT31_STATUS_RESET_DETECTED 0x0010	///< System reset detected
#define SHT31_STATUS_COMMAND_STATUS 0x0002	///< Command status
#define SHT31_STATUS_WRITE_CRC_STATUS 0x0001	///< Write data checksum status

// Measurement timing (milliseconds)
#define SHT31_MEAS_DURATION_HIGH 15	///< High repeatability measurement duration
#define SHT31_MEAS_DURATION_MED 6	///< Medium repeatability measurement duration
#define SHT31_MEAS_DURATION_LOW 4	///< Low repeatability measurement duration

/**
 * @brief SHT31 repeatability modes
 */
typedef enum {
	SHT31_REPEATABILITY_HIGH = 0,	///< High repeatability
	SHT31_REPEATABILITY_MEDIUM = 1,	///< Medium repeatability
	SHT31_REPEATABILITY_LOW = 2	///< Low repeatability
} sht31_repeatability_t;

/**
 * @brief SHT31 temperature and humidity sensor driver
 */
class SHT31 {
public:
	SHT31(void);
	~SHT31(void);

	bool begin(uint8_t i2c_addr = SHT31_I2CADDR_DEFAULT, I2CBus *bus = &I2CBus0);
	void end();

	bool readTempHumidity(float *temperature, float *humidity);
	bool readTempHumidityRaw(uint16_t *temp_raw, uint16_t *hum_raw);
	
	float readTemperature(void);
	float readHumidity(void);
	
	bool setRepeatability(sht31_repeatability_t repeatability);
	sht31_repeatability_t getRepeatability(void);
	
	bool softReset(void);
	uint16_t readStatus(void);
	bool readStatus(uint16_t* out);
	bool clearStatus(void);
	
	bool enableHeater(bool enable);
	bool isHeaterEnabled(void);
	
	bool isConnected(void);

private:
	bool _init(void);
	bool _sendCommand(uint16_t command);
	bool _readResponse(uint8_t *data, uint8_t length);
	uint8_t _crc8(const uint8_t *data, int len);
	bool _verifyCRC(const uint8_t *data, uint8_t crc);
	float _convertRawTemperature(uint16_t raw_temp);
	float _convertRawHumidity(uint16_t raw_humidity);
	
	sht31_repeatability_t _repeatability;
	uint32_t _last_read_time;
	float _last_temperature;
	float _last_humidity;
	bool _has_valid_data;
	I2CDevice *i2c_dev;
};

#endif // _SHT31_H 
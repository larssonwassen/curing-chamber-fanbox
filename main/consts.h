#ifndef _CONSTS_H
#define _CONSTS_H

#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "AtomicVariable/AtomicVariable.h"
#include <netdb.h>
#include <lwip/netdb.h>
#include <lwip/dns.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>

/**
 * @brief Base class for classes that need change tracking via event groups
 */
class ChangeTrackable {
private:
	static const int CHANGED_BIT = BIT0;

protected:
	EventGroupHandle_t eventGroup;

	static void onChange(void* data) {
		ChangeTrackable* self = (ChangeTrackable*)data;
		ESP_LOGV("consts", "Attributes changed");
		xEventGroupSetBits(self->eventGroup, CHANGED_BIT);
	}

	ChangeTrackable(): eventGroup(xEventGroupCreate()) {
		xEventGroupSetBits(eventGroup, CHANGED_BIT);
	}

public:
	bool hasChanged(void) {
		return (xEventGroupGetBits(eventGroup) & CHANGED_BIT) != 0;
	}

	void clearChanged(void) {
		xEventGroupClearBits(eventGroup, CHANGED_BIT);
	}

	bool waitForChange(TickType_t timeout = portMAX_DELAY) {
		return (xEventGroupWaitBits(eventGroup, CHANGED_BIT, false, true, timeout) & CHANGED_BIT) != 0;
	}
};

class Telemetry : public ChangeTrackable {
public:
	AtomicVariable<uint8_t> fanDuty;
	AtomicVariable<uint16_t> fanRPM;
	AtomicVariable<double> temperature;
	AtomicVariable<double> humidity;

	Telemetry(void):
		ChangeTrackable(),
		// Deliberately no NVS keys. These update on every sensor read (~1 Hz), and
		// persisting them wore the NVS partition past its rated ~100k erase cycles
		// per sector. Telemetry is transient by nature -- there is nothing here
		// worth restoring across a reboot. Legacy keys are purged in init_consts().
		fanDuty(0, nullptr, onChange, this),
		fanRPM(0, nullptr, onChange, this),
		temperature(0.0f, nullptr, onChange, this),
		humidity(0.0f, nullptr, onChange, this)
	{}
};
extern Telemetry* telemetry;

class SharedAttributes : public ChangeTrackable {
public:
	AtomicVariable<bool> fanEnabled;
	AtomicVariable<bool> ctrlLoopEnabled;
	AtomicVariable<esp_log_level_t> uartLogLevel;
	AtomicVariable<esp_log_level_t> streamerLogLevel;
	AtomicVariable<double> humiditySetpoint;
	AtomicVariable<double> humidityOvershootLimit;
	AtomicVariable<double> humidityUndershootLimit;

	SharedAttributes():
		ChangeTrackable(),
		fanEnabled(true, "fe", onChange, this),
		ctrlLoopEnabled(true, "cle", onChange, this),
		uartLogLevel(ESP_LOG_DEBUG, "ull", onChange, this),
		streamerLogLevel(ESP_LOG_INFO, "sll", onChange, this),
		humiditySetpoint(75.0, "hsp", onChange, this),
		humidityOvershootLimit(5.0, "hos", onChange, this),
		humidityUndershootLimit(5.0, "hus", onChange, this)
	{}
};
extern SharedAttributes* shared_attributes;

class Attributes : public ChangeTrackable {
public:
	AtomicVariable<bool> fanEnabled;
	AtomicVariable<bool> fanRunning;

	Attributes():
		ChangeTrackable(),
		fanEnabled(true, nullptr, onChange, this),
		fanRunning(false, nullptr, onChange, this)
	{}
};
extern Attributes* attributes;

#define MAX(x, y) ((x) > (y) ? (x) : (y))
#define MIN(x, y) ((x) > (y) ? (y) : (x))

#ifndef INET_ADDRSTRLEN
#define INET_ADDRSTRLEN 16
#endif
#ifndef INET6_ADDRSTRLEN
#define INET6_ADDRSTRLEN 46
#endif

extern EventGroupHandle_t network_state_event_group;
extern const int WIFI_CONNECTED_BIT;
extern const int WIFI_FAIL_BIT;
extern const int MQTT_CONNECTED_BIT;


void init_consts(void);
void log_nvs_stats(const char* when);
void getMAC(char* out);
void print_ip_info(const char* tag);
void dns_debug(const char *host, const char *service);
void waitForBit(int bit);

#endif // _CONSTS_H
